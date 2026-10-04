#include "Papyrus.h"

#include "Json.h"
#include "MainThread.h"
#include "PapyrusCallPolicy.h"
#include "ToolRegistry.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>

#include "RE/O/Object.h"
#include "RE/P/PackUnpack.h"  // BindID — the engine's form→Papyrus-object binding helper

// <Windows.h> (via pch) defines a GetObject macro (GDI) that otherwise rewrites
// Variable::GetObject() to GetObjectA. Variable.h undefs it for its own scope only.
#undef GetObject

namespace dvb::Papyrus
{
	namespace
	{
		namespace BSScript = RE::BSScript;

		BSScript::Internal::VirtualMachine* GetVM()
		{
			auto* vm = BSScript::Internal::VirtualMachine::GetSingleton();
			if (!vm)
				throw ToolError(503, "Papyrus VM unavailable (game not far enough loaded?)");
			return vm;
		}

		std::string Lower(std::string a_s)
		{
			std::transform(a_s.begin(), a_s.end(), a_s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_s;
		}

		std::string Str(const char* a_s) { return a_s ? std::string(a_s) : std::string{}; }

		// A returned Object → { formId, formType, editorId, name } when it is form-backed,
		// else just the class name (aliases / active effects aren't forms). Guarded by walking
		// the script parent chain to "Form" so we never read a non-form pointer as a TESForm.
		json ObjectToJson(BSScript::Internal::VirtualMachine* a_vm, const BSScript::Variable& a_var)
		{
			auto obj = a_var.GetObject();
			if (!obj)
				return json{ { "none", true } };

			auto*             typeInfo = obj->GetTypeInfo();
			const std::string cls = typeInfo ? Str(typeInfo->GetName()) : std::string{};

			bool formBacked = false;
			for (auto* t = typeInfo; t; t = t->GetParent())
				if (Str(t->GetName()) == "Form") {
					formBacked = true;
					break;
				}

			json out{ { "scriptType", cls } };
			if (!formBacked)
				return out;

			RE::VMTypeID typeID{};
			auto*        policy = a_vm->GetObjectHandlePolicy();
			if (policy && a_vm->GetTypeIDForScriptObject(cls.c_str(), typeID)) {
				if (auto* form = static_cast<RE::TESForm*>(policy->GetObjectForHandle(typeID, obj->GetHandle()))) {
					out["formId"] = std::format("0x{:08X}", form->GetFormID());
					out["formType"] = std::format("{}", static_cast<int>(form->GetFormType()));
					out["editorId"] = Str(form->GetFormEditorID());
					if (const char* n = form->GetName(); n && *n)
						out["name"] = n;
				}
			}
			return out;
		}

		// A result Variable → JSON. Scalars map directly; objects resolve to form info;
		// arrays map element-wise; none becomes null (returnedType reports the real type).
		json VariableToJson(BSScript::Internal::VirtualMachine* a_vm, const BSScript::Variable& a_var)
		{
			if (a_var.IsBool())
				return a_var.GetBool();
			if (a_var.IsInt())
				return a_var.GetSInt();
			if (a_var.IsFloat())
				return a_var.GetFloat();
			if (a_var.IsString())
				return std::string(a_var.GetString());
			if (a_var.IsObject())
				return ObjectToJson(a_vm, a_var);
			if (a_var.IsArray()) {
				json out = json::array();
				if (auto arr = a_var.GetArray())
					for (RE::BSScript::Array::size_type i = 0; i < arr->size(); ++i)
						out.push_back(VariableToJson(a_vm, (*arr)[i]));
				return out;
			}
			return nullptr;  // none — see returnedType
		}

		// Parse a hex FormID, or nullptr. Requires the WHOLE string to be hex and in 32-bit range
		// — std::stoul would otherwise accept "14G" as 0x14 and silently truncate overflow.
		RE::TESForm* FormByHex(const std::string& a_hex)
		{
			std::size_t        consumed = 0;
			unsigned long long id = 0;
			try {
				id = std::stoull(a_hex, &consumed, 16);
			} catch (...) {
				return nullptr;
			}
			if (consumed != a_hex.size() || id > 0xFFFFFFFFull)
				return nullptr;
			return RE::TESForm::LookupByID(static_cast<RE::FormID>(id));
		}

		// Resolve a form reference to a TESForm, or nullptr. An explicit `0x..` is a FormID;
		// otherwise try EditorID first (so an all-hex EditorID isn't misread as a FormID), then
		// fall back to a bare hex FormID (the `14` shorthand).
		RE::TESForm* LookupForm(const std::string& a_ref)
		{
			if (a_ref.size() > 2 && a_ref[0] == '0' && (a_ref[1] == 'x' || a_ref[1] == 'X'))
				return FormByHex(a_ref.substr(2));
			if (auto* form = RE::TESForm::LookupByEditorID(a_ref))
				return form;
			return FormByHex(a_ref);
		}

		// Find or create+bind a script Object of `a_className` for `a_form` — the engine's own
		// form→Papyrus binding (FindBoundObject → CreateObject → BindID; see CommonLib PackUnpack).
		// Dispatching/packing on a form that isn't bound is what CTD'd. Null if it can't bind.
		RE::BSTSmartPointer<BSScript::Object> BindFormObject(BSScript::Internal::VirtualMachine* a_vm, RE::TESForm* a_form, const char* a_className)
		{
			RE::BSTSmartPointer<BSScript::Object> obj;
			auto*                                 policy = a_vm->GetObjectHandlePolicy();
			if (!policy || !a_className)
				return obj;
			const auto typeID = static_cast<RE::VMTypeID>(a_form->GetFormType());
			const auto handle = policy->GetHandleForObject(typeID, a_form);
			if (handle == policy->EmptyHandle())
				return obj;
			if (!a_vm->FindBoundObject(handle, a_className, obj) || !obj)
				if (a_vm->CreateObject(a_className, obj) && obj)
					RE::BSScript::BindID(obj, a_form, typeID);
			return obj;
		}

		BSScript::Variable JsonToVariable(BSScript::Internal::VirtualMachine* a_vm, const json& a_arg, const BSScript::TypeInfo* a_paramType = nullptr);

		// Pack a form into a Variable typed as `a_paramType` expects. PackHandle would type the
		// form as its *native* class, which a param declared as a base class (Form/ObjectReference)
		// rejects; binding to the param's class lets the upcast bind. Native fallback otherwise.
		void PackFormToParam(BSScript::Internal::VirtualMachine* a_vm, RE::TESForm* a_form, const BSScript::TypeInfo& a_paramType, BSScript::Variable& a_out)
		{
			if (a_paramType.IsObject())
				if (auto* paramClass = a_paramType.GetTypeInfo(); paramClass)
					if (auto obj = BindFormObject(a_vm, a_form, paramClass->GetName())) {
						a_out.SetObject(obj, a_paramType.GetRawType());
						return;
					}
			BSScript::PackHandle(&a_out, a_form, static_cast<RE::VMTypeID>(a_form->GetFormType()));
		}

		// Build a typed Papyrus Array from a JSON array of scalars. The element type comes from
		// the contents (a typed Papyrus array can't be inferred from an empty list, so empty is
		// rejected) and every element must share one kind — a mixed array would otherwise be packed
		// as the first element's type and mis-typed silently. Int promotes to Float if any element
		// is fractional.
		BSScript::Variable JsonArrayToVariable(BSScript::Internal::VirtualMachine* a_vm, const json& a_arr)
		{
			using Raw = BSScript::TypeInfo::RawType;
			// kind: bool / number / string — the homogeneity classes (int vs float both number)
			auto kind = [](const json& e) -> int { return e.is_boolean() ? 0 : e.is_number() ? 1 :
				                                                           e.is_string()     ? 2 :
				                                                                               -1; };
			if (a_arr.empty())
				throw ToolError(400, "papyrus call: empty array args can't be typed — pass a non-empty array");
			const int k0 = kind(a_arr.front());
			if (k0 < 0)
				throw ToolError(400, "papyrus call: array args support only bool/number/string elements");
			bool anyFloat = false;
			for (const auto& e : a_arr) {
				if (kind(e) != k0)
					throw ToolError(400, "papyrus call: array args must be homogeneous (all bool, all number, or all string)");
				anyFloat = anyFloat || e.is_number_float();
			}
			const Raw elem = (k0 == 0) ? Raw::kBool : (k0 == 2) ? Raw::kString :
			                                                      (anyFloat ? Raw::kFloat : Raw::kInt);

			RE::BSTSmartPointer<BSScript::Array> array;
			if (!a_vm->CreateArray(BSScript::TypeInfo(elem), static_cast<std::uint32_t>(a_arr.size()), array) || !array)
				throw ToolError(500, "papyrus call: could not allocate a Papyrus array");
			for (std::uint32_t i = 0; i < a_arr.size(); ++i)
				(*array)[i] = JsonToVariable(a_vm, a_arr[i]);

			BSScript::Variable v;
			v.SetArray(std::move(array));
			return v;
		}

		// One JSON arg → one Variable. Scalars are direct; { "form": "0x14" | "EditorID" } resolves
		// a TESForm and packs it to the declared param type (a_paramType) so base-typed params bind,
		// else to its native type; a JSON array becomes a typed Papyrus array. Touches the VM (form
		// handles / array alloc) — call on the main thread.
		BSScript::Variable JsonToVariable(BSScript::Internal::VirtualMachine* a_vm, const json& a_arg, const BSScript::TypeInfo* a_paramType)
		{
			BSScript::Variable v;
			if (a_arg.is_null()) {
				if (!a_paramType || (!a_paramType->IsObject() && !a_paramType->IsArray()))
					throw ToolError(400, "papyrus call: explicit null (None) is only valid for an object or array parameter");
			} else if (a_arg.is_boolean()) {
				v.SetBool(a_arg.get<bool>());
			} else if (a_arg.is_number_float()) {
				v.SetFloat(static_cast<float>(a_arg.get<double>()));
			} else if (a_arg.is_number_integer() || a_arg.is_number_unsigned()) {
				v.SetSInt(a_arg.get<std::int32_t>());
			} else if (a_arg.is_string()) {
				v.SetString(a_arg.get<std::string>());
			} else if (a_arg.is_array()) {
				return JsonArrayToVariable(a_vm, a_arg);
			} else if (a_arg.is_object() && a_arg.contains("form")) {
				const std::string ref = a_arg["form"].get<std::string>();
				RE::TESForm*      form = LookupForm(ref);
				if (!form)
					throw ToolError(400, std::format("papyrus call: form '{}' not found (formId hex or EditorID)", ref));
				if (a_paramType)
					PackFormToParam(a_vm, form, *a_paramType, v);
				else
					BSScript::PackHandle(&v, form, static_cast<RE::VMTypeID>(form->GetFormType()));
			} else {
				throw ToolError(400, "papyrus call: each arg must be a bool, number, string, array, or { \"form\": \"0x.. | EditorID\" }");
			}
			return v;
		}

		// Find a function by name on a type, walking the parent chain for member functions
		// (globals/statics are not inherited). Case-insensitive (Papyrus names are). For resolving
		// param types so form args can be packed to the declared (possibly base) param class.
		const BSScript::IFunction* FindFunction(BSScript::ObjectTypeInfo* a_type, std::string_view a_name, bool a_global)
		{
			const std::string needle = Lower(std::string(a_name));
			for (auto* t = a_type; t; t = t->GetParent()) {
				const std::uint32_t n = a_global ? t->GetNumGlobalFuncs() : t->GetNumMemberFuncs();
				for (std::uint32_t i = 0; i < n; ++i) {
					const auto& f = a_global ? t->GetGlobalFuncIter()[i].func : t->GetMemberFuncIter()[i].func;
					if (f && f->GetName().c_str() && Lower(std::string(f->GetName().c_str())) == needle)
						return f.get();
				}
				if (a_global)
					break;
			}
			return nullptr;
		}

		json DescribeFunction(const BSScript::IFunction* a_fn)
		{
			json params = json::array();
			for (std::uint32_t p = 0; p < a_fn->GetParamCount(); ++p) {
				RE::BSFixedString  name;
				BSScript::TypeInfo type;
				a_fn->GetParam(p, name, type);
				params.push_back(json{ { "name", Str(name.c_str()) }, { "type", type.TypeAsString() } });
			}
			return json{
				{ "name", Str(a_fn->GetName().c_str()) },
				{ "declaringScript", Str(a_fn->GetObjectTypeName().c_str()) },
				{ "returnType", a_fn->GetReturnType().TypeAsString() },
				{ "params", std::move(params) },
				{ "native", a_fn->GetIsNative() },
				{ "argumentPolicy", "explicit_exact_count" },
			};
		}

		// IFunctionArguments built from a runtime list — MakeFunctionArguments is compile-time
		// only, so 'call' needs its own that copies pre-built Variables into the VM's scrap array.
		class RuntimeArgs : public BSScript::IFunctionArguments
		{
		public:
			std::vector<BSScript::Variable> args;

			bool operator()(RE::BSScrapArray<BSScript::Variable>& a_dst) const override
			{
				using size_type = RE::BSScrapArray<BSScript::Variable>::size_type;
				a_dst.resize(static_cast<size_type>(args.size()));
				for (size_type i = 0; i < a_dst.size(); ++i)
					a_dst[i] = args[i];
				return true;
			}
		};

		// Shared completion state. Held by a shared_ptr so a late VM callback (after a timeout
		// already returned) writes into live memory, never a freed functor's fields.
		struct CallState
		{
			explicit CallState(json request, PapyrusCallPolicy::Lifecycle::Clock::time_point deadline) :
				lifecycle(std::move(request), deadline) {}
			PapyrusCallPolicy::Lifecycle lifecycle;  // guarded by m, including response snapshots
			std::mutex              m;
			std::condition_variable cv;
			bool                    done = false;
			int                     status = 400;  // HTTP status to surface when `error` is set
			std::string             error;         // non-empty → arg-build / bind / dispatch failed
			BSScript::Variable      result;
		};

		class CallFunctor : public BSScript::IStackCallbackFunctor
		{
		public:
			explicit CallFunctor(std::shared_ptr<CallState> a_state) :
				_state(std::move(a_state)) {}

			void operator()(BSScript::Variable a_result) override
			{
				std::lock_guard<std::mutex> lk(_state->m);
				_state->result = a_result;
				_state->done = true;
				_state->lifecycle.Completed();
				_state->cv.notify_all();
			}

			bool CanSave() const override { return false; }
			void SetObject(const RE::BSTSmartPointer<BSScript::Object>&) override {}

		private:
			std::shared_ptr<CallState> _state;
		};

		json HandleList(const json& a_args)
		{
			const std::string filter = a_args.value("filter", std::string{});
			const int         limit = a_args.value("limit", 200);
			return MainThread::RunAndWait([filter, limit]() -> json {
				auto*                    vm = GetVM();
				const std::string        needle = Lower(filter);
				std::vector<std::string> names;
				int                      total = 0;
				{
					RE::BSSpinLockGuard lock(vm->typeInfoLock);
					for (auto& entry : vm->objectTypeMap) {
						std::string name = Str(entry.first.c_str());
						if (name.empty())
							continue;
						if (!needle.empty() && Lower(name).find(needle) == std::string::npos)
							continue;
						++total;
						if (static_cast<int>(names.size()) < limit)
							names.push_back(std::move(name));
					}
				}
				std::sort(names.begin(), names.end());
				return json{
					{ "total", total },
					{ "returned", static_cast<int>(names.size()) },
					{ "truncated", total > static_cast<int>(names.size()) },
					{ "scripts", std::move(names) },
				};
			});
		}

		json HandleDescribe(const json& a_args)
		{
			const std::string script = a_args.value("script", std::string{});
			if (script.empty())
				throw ToolError(400, "papyrus describe requires 'script' (class name)");
			return MainThread::RunAndWait([script]() -> json {
				auto*                                         vm = GetVM();
				RE::BSTSmartPointer<BSScript::ObjectTypeInfo> type;
				if (!vm->GetScriptObjectType1(script.c_str(), type) || !type)
					throw ToolError(404, std::format("unknown script class '{}'", script));

				json globals = json::array();
				for (std::uint32_t i = 0; i < type->GetNumGlobalFuncs(); ++i)
					if (auto& fn = type->GetGlobalFuncIter()[i].func)
						globals.push_back(DescribeFunction(fn.get()));

				json members = json::array();
				for (std::uint32_t i = 0; i < type->GetNumMemberFuncs(); ++i)
					if (auto& fn = type->GetMemberFuncIter()[i].func)
						members.push_back(DescribeFunction(fn.get()));

				json props = json::array();
				for (std::uint32_t i = 0; i < type->GetNumProperties(); ++i)
					props.push_back(Str(type->GetPropertyIter()[i].name.c_str()));

				const auto* parent = type->GetParent();
				json        out{
					{ "name", Str(type->GetName()) },
					{ "globalFunctions", std::move(globals) },
					{ "memberFunctions", std::move(members) },
					{ "properties", std::move(props) },
				};
				out["parent"] = parent ? json(Str(parent->GetName())) : json(nullptr);
				return out;
			});
		}

		// Resolve a 'self' selector to a *bound* script Object for member dispatch. "selected"
		// uses the console-selected ref (set by `prid`, the crosshair, or a click); otherwise a
		// { "form": ".." } / formId / EditorID targets any form. A member call needs the form
		// bound to a script object of `a_className` first — find the existing binding or create +
		// bind one (the same FindBoundObject → CreateObject → BindID dance the engine itself runs
		// for every form it passes into Papyrus; see CommonLib PackUnpack). Skipping the bind and
		// dispatching on a bare handle is what CTD'd. Main thread only.
		bool ResolveSelf(BSScript::Internal::VirtualMachine* a_vm, const json& a_self, const RE::BSFixedString& a_className,
			RE::BSTSmartPointer<BSScript::Object>& a_out, std::string& a_err)
		{
			RE::TESForm* form = nullptr;
			if (a_self.is_string() && a_self.get<std::string>() == "selected") {
				if (auto sel = RE::Console::GetSelectedRef())
					form = sel.get();
				if (!form) {
					a_err = "self='selected' but no console-selected ref (use `prid <id>`, the crosshair, or click one)";
					return false;
				}
			} else {
				const std::string ref =
					a_self.is_string() ? a_self.get<std::string>() : (a_self.is_object() && a_self.contains("form") ? a_self["form"].get<std::string>() : std::string{});
				if (ref.empty()) {
					a_err = "self must be \"selected\" or { \"form\": \"0x.. | EditorID\" }";
					return false;
				}
				form = LookupForm(ref);
				if (!form) {
					a_err = std::format("self form '{}' not found", ref);
					return false;
				}
			}

			// Prefer the caller's `script` when the form actually IS that type — an attached
			// custom script, or the form's exact native type — so a member call on an attached
			// script resolves there. Only when it isn't (e.g. `script` is a parent class like
			// "ObjectReference" for an Actor, where BindID's exact HandleIsType would fail silently)
			// fall back to the form's native type, whose hierarchy still satisfies the function
			// (an Actor has ObjectReference.GetDistance).
			RE::BSFixedString bindClass = a_className;
			auto*             policy = a_vm->GetObjectHandlePolicy();
			RE::VMTypeID      reqTypeID{};
			const auto        handle = policy ? policy->GetHandleForObject(static_cast<RE::VMTypeID>(form->GetFormType()), form) : RE::VMHandle{};
			const bool        isRequestedType = policy && a_vm->GetTypeIDForScriptObject(a_className, reqTypeID) && policy->HandleIsType(reqTypeID, handle);
			if (!isRequestedType) {
				RE::BSTSmartPointer<BSScript::ObjectTypeInfo> nativeType;
				if (a_vm->GetScriptObjectType(static_cast<RE::VMTypeID>(form->GetFormType()), nativeType) && nativeType && nativeType->GetName())
					bindClass = nativeType->GetName();
			}

			a_out = BindFormObject(a_vm, form, bindClass.c_str());
			if (!a_out) {
				a_err = std::format("could not bind form 0x{:08X} to a '{}' object", form->GetFormID(), bindClass.c_str());
				return false;
			}
			return true;
		}

		json HandleCall(const json& a_args, bool a_waitForResult)
		{
			const std::string script = a_args.value("script", std::string{});
			const std::string function = a_args.value("function", std::string{});
			if (script.empty() || function.empty())
				throw ToolError(400, "papyrus call requires 'script' and 'function'");
			const json timeout = a_args.contains("timeoutMs") ? a_args["timeoutMs"] : json(a_waitForResult ? 3000 : 5000);
			if ((!timeout.is_number_integer() && !timeout.is_number_unsigned()) ||
				(timeout.is_number_unsigned() ? timeout.get<std::uint64_t>() > PapyrusCallPolicy::kMaximumTimeoutMs :
					timeout.get<std::int64_t>() > PapyrusCallPolicy::kMaximumTimeoutMs || timeout.get<std::int64_t>() < 1) ||
				timeout == 0)
				throw ToolError(400, "papyrus call: 'timeoutMs' must be an integer in 1..60000");
			const int timeoutMs = timeout.get<int>();
			const json argsJson = a_args.contains("args") ? a_args["args"] : json::array();
			if (!argsJson.is_array())
				throw ToolError(400, "papyrus call: 'args' must be an array");
			const json selfJson = a_args.contains("self") ? a_args["self"] : json();
			const bool hasSelf = !selfJson.is_null();

			auto* task = SKSE::GetTaskInterface();
			if (!task)
				throw ToolError(500, "SKSE TaskInterface unavailable");
			using Clock = PapyrusCallPolicy::Lifecycle::Clock;
			static std::atomic<std::uint64_t> nextCall{ 0 };
			const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
			auto state = std::make_shared<CallState>(json{
				{ "callId", std::format("papyrus-{}-{}", GetCurrentProcessId(), ++nextCall) },
				{ "script", script }, { "function", function }, { "suppliedArgs", argsJson },
				{ "suppliedSelf", selfJson }, { "timeoutMs", timeoutMs },
			}, deadline);
			auto receipt = [state] {
				std::lock_guard lock(state->m);
				return json{ { "callReceipt", state->lifecycle.Snapshot() } };
			};

			const RE::BSFixedString cls(script.c_str());
			const RE::BSFixedString fn(function.c_str());
			auto dispatch = [cls, fn, argsJson, selfJson, hasSelf, state](RE::BSTSmartPointer<BSScript::IStackCallbackFunctor> callback) {
				{
					std::lock_guard lock(state->m);
					if (!state->lifecycle.BeginPreparation(Clock::now()))
						throw ToolError(504, "papyrus call expired before preparation; no function dispatch");
				}
				auto* vm = GetVM();
				// Keep the established object binding and actual resolved-function lookup.
				// Binding/argument preparation may touch VM objects, but no target function
				// is dispatched until exact count and the final deadline gate succeed.
				RE::BSTSmartPointer<BSScript::Object> selfObj;
				if (hasSelf) {
					std::string err;
					if (!ResolveSelf(vm, selfJson, cls, selfObj, err))
						throw ToolError(400, err);
				}
				RE::BSTSmartPointer<BSScript::ObjectTypeInfo> scriptType;
				BSScript::ObjectTypeInfo* fnType = nullptr;
				if (hasSelf)
					fnType = selfObj->GetTypeInfo();
				else if (vm->GetScriptObjectType(cls, scriptType) && scriptType)
					fnType = scriptType.get();
				const BSScript::IFunction* ifn = fnType ? FindFunction(fnType, std::string_view(fn.c_str() ? fn.c_str() : ""), !hasSelf) : nullptr;
				if (!ifn)
					throw ToolError(404, std::format("no such {} function '{}' on script '{}'", hasSelf ? "member" : "global/native", fn.c_str(), cls.c_str()));
				{
					std::lock_guard lock(state->m);
					state->lifecycle.Resolved(DescribeFunction(ifn),
						hasSelf ? json(std::format("0x{:016X}", selfObj->GetHandle())) : json(nullptr));
				}
				if (!PapyrusCallPolicy::ExactArgumentCount(argsJson.size(), ifn->GetParamCount()))
					throw ToolError(400, std::format("papyrus call: expected exactly {} explicit arguments, got {}; loaded metadata does not prove optional defaults — use describe and supply every parameter", ifn->GetParamCount(), argsJson.size()));

				// RAII until VM entry; preserve the original raw-pointer handoff at
				// Dispatch*Call. Do not introduce a competing post-entry deletion.
				auto rawArgs = std::make_unique<RuntimeArgs>();
				for (std::uint32_t p = 0; p < ifn->GetParamCount(); ++p) {
					RE::BSFixedString name;
					BSScript::TypeInfo type;
					ifn->GetParam(p, name, type);
					rawArgs->args.push_back(JsonToVariable(vm, argsJson[p], &type));
				}
				{
					std::lock_guard lock(state->m);
					if (!state->lifecycle.BeginDispatch(Clock::now()))
						throw ToolError(504, "papyrus call expired before dispatch; no function dispatch");
				}
				// Never hold the state mutex across VM dispatch: a synchronous callback
				// acquires it. A timeout at/after this boundary is conservatively uncertain.
				auto* handedOff = rawArgs.release();
				const bool ok = hasSelf ? vm->DispatchMethodCall(selfObj, fn, handedOff, callback) : vm->DispatchStaticCall(cls, fn, handedOff, callback);
				if (!ok)
					throw ToolError(400, "Papyrus VM refused dispatch; receipt conservatively retains VM-entry uncertainty, do not automatically replay");
				std::lock_guard lock(state->m);
				state->lifecycle.Accepted();
			};

			auto fail = [state](int status, const std::string& message) {
				std::lock_guard lock(state->m);
				if (state->done)
					return;
				state->lifecycle.Failed();
				state->status = status;
				state->error = message;
				state->done = true;
				state->cv.notify_all();
			};
			if (!a_waitForResult) {
				try {
					return MainThread::RunAndWait([dispatch, receipt] {
						dispatch(nullptr);
						return json{ { "queued", true }, { "callReceipt", receipt()["callReceipt"] } };
					}, std::chrono::milliseconds(timeoutMs));
				} catch (const MainThread::TaskTimeout& e) {
					std::lock_guard lock(state->m);
					state->lifecycle.TimedOut();
					throw ToolError(e.code, e.what(), json{ { "callReceipt", state->lifecycle.Snapshot() } });
				} catch (const ToolError& e) {
					fail(e.code, e.what());
					throw ToolError(e.code, e.what(), receipt());
				} catch (const std::exception& e) {
					fail(500, e.what());
					throw ToolError(500, e.what(), receipt());
				}
			}

			try {
				task->AddTask([dispatch, state, fail] {
					try {
						dispatch(RE::BSTSmartPointer<BSScript::IStackCallbackFunctor>(new CallFunctor(state)));
					} catch (const ToolError& e) {
						fail(e.code, e.what());
					} catch (const json::type_error& e) {
						fail(400, e.what());
					} catch (const std::exception& e) {
						fail(500, e.what());
					} catch (...) {
						fail(500, "unknown exception before Papyrus completion");
					}
				});
			} catch (const std::exception& e) {
				fail(500, e.what());
				throw ToolError(500, e.what(), receipt());
			}
			std::unique_lock lock(state->m);
			if (!state->cv.wait_until(lock, deadline, [&] { return state->done; })) {
				state->lifecycle.TimedOut();
				throw ToolError(504, "papyrus call timed out; inspect callReceipt before any manual recovery — timeout does not cancel VM-entered calls or permit automatic retry",
					json{ { "callReceipt", state->lifecycle.Snapshot() } });
			}
			if (!state->error.empty())
				throw ToolError(state->status, state->error, json{ { "callReceipt", state->lifecycle.Snapshot() } });
			BSScript::Variable result = state->result;
			const json completedReceipt = state->lifecycle.Snapshot();
			lock.unlock();
			try {
				return MainThread::RunAndWait([result, completedReceipt] {
					auto* vm = GetVM();
					return json{ { "called", true }, { "returned", VariableToJson(vm, result) },
						{ "returnedType", result.GetType().TypeAsString() }, { "callReceipt", completedReceipt } };
				});
			} catch (const ToolError& e) {
				throw ToolError(e.code, e.what(), json{ { "callReceipt", completedReceipt } });
			} catch (const std::exception& e) {
				throw ToolError(500, e.what(), json{ { "callReceipt", completedReceipt } });
			}
		}
	}

	json QueueCall(const json& a_args)
	{
		return HandleCall(a_args, false);
	}

	json Handle(const json& a_args, const ToolContext&)
	{
		const std::string action = a_args.value("action", std::string("list"));
		if (action == "list")
			return HandleList(a_args);
		if (action == "describe")
			return HandleDescribe(a_args);
		if (action == "call")
			return HandleCall(a_args, true);
		throw ToolError(400, std::format("unknown action '{}' (list|describe|call)", action));
	}
}
