#include "KeyboardInput.h"

#include "EventBus.h"
#include "GameState.h"
#include "KeyboardInputState.h"
#include "MainThread.h"
#include "ToolRegistry.h"

#include <RE/B/BSInputEventQueue.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace dvb
{
	namespace
	{
		using namespace std::chrono;

		constexpr int         kKeyboardContractVersion = 1;
		constexpr int         kDefaultTapMs = 50;
		constexpr int         kDefaultMaxHoldMs = 5000;
		constexpr int         kMaximumMaxHoldMs = 60000;
		constexpr std::size_t kMaximumHeldKeys = 8;
		constexpr std::size_t kMaximumSequenceEvents = 128;
		constexpr int         kMaximumSequenceMs = 30000;

		std::atomic<bool> g_inputReady{ false };

		std::int64_t NowMs()
		{
			return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
		}

		int BoundedInteger(const json& a_object, const char* a_name, int a_default, int a_min, int a_max)
		{
			if (!a_object.contains(a_name))
				return a_default;
			if (!a_object[a_name].is_number_integer())
				throw ToolError(400, std::format("'{}' must be an integer", a_name));
			const int value = a_object[a_name].get<int>();
			if (value < a_min || value > a_max)
				throw ToolError(400, std::format("'{}' must be between {} and {}", a_name, a_min, a_max));
			return value;
		}

		KeyboardKey ParseKey(const json& a_args)
		{
			if (!a_args.contains("key"))
				throw ToolError(400, "keyboard action requires 'key' (a documented name or DirectInput scancode)");
			std::optional<KeyboardKey> key;
			if (a_args["key"].is_string())
				key = ResolveKeyboardKey(a_args["key"].get<std::string>());
			else if (a_args["key"].is_number_integer())
				key = ResolveKeyboardKey(a_args["key"].get<int>());
			else
				throw ToolError(400, "'key' must be a string name or integer DirectInput scancode");
			if (!key)
				throw ToolError(400, std::format("unknown/invalid keyboard key {} — use action='capabilities' for names and scan codes", a_args["key"].dump()));
			return *key;
		}

		std::string ResolveOwner(const json& a_args, const ToolContext& a_ctx)
		{
			std::string owner;
			if (a_args.contains("owner")) {
				if (!a_args["owner"].is_string())
					throw ToolError(400, "'owner' must be a string");
				owner = a_args["owner"].get<std::string>();
			} else if (!a_ctx.clientId.empty()) {
				owner = "mcp:" + a_ctx.clientId;
			} else {
				owner = "rest:anonymous";
			}
			if (owner.empty() || owner.size() > 128)
				throw ToolError(400, "'owner' must contain 1..128 characters");
			return owner;
		}

		json KeyJson(const KeyboardKey& a_key)
		{
			return json{ { "key", a_key.name }, { "scancode", a_key.scancode } };
		}

		json ContractJson()
		{
			return json{ { "name", "devbench.input" }, { "version", kKeyboardContractVersion } };
		}

		struct QueueResult
		{
			bool pending = false;
			int  frame = -1;
		};

		class KeyboardManager
		{
		public:
			static KeyboardManager& Get()
			{
				// Process-lifetime singleton: detached bounded lease timers may still reference it
				// during DLL/process teardown, so deliberately do not run a static destructor.
				static auto* instance = new KeyboardManager();
				return *instance;
			}

			void SetEvents(EventBus& a_events)
			{
				std::lock_guard lock(m_mutex);
				m_events = &a_events;
			}

			json Capabilities()
			{
				json keys = json::array();
				for (const auto& key : KeyboardKeyCatalog())
					keys.push_back(KeyJson(key));
				return json{
					{ "contract", ContractJson() },
					{ "capabilities", json{
						{ "keyboard", json{
							{ "version", kKeyboardContractVersion },
							{ "available", true },
							{ "ready", g_inputReady.load(std::memory_order_relaxed) },
							{ "injection", "Skyrim.BSInputEventQueue" },
							{ "encoding", "DirectInputScanCode" },
							{ "actions", json::array({ "status", "down", "up", "tap", "sequence", "releaseAll" }) },
							{ "defaultTapMs", kDefaultTapMs },
							{ "defaultMaxHoldMs", kDefaultMaxHoldMs },
							{ "maximumMaxHoldMs", kMaximumMaxHoldMs },
							{ "maximumHeldKeys", kMaximumHeldKeys },
							{ "maximumSequenceEvents", kMaximumSequenceEvents },
							{ "maximumSequenceMs", kMaximumSequenceMs },
							{ "keys", std::move(keys) },
						} },
					} },
				};
			}

			json Status()
			{
				const auto now = NowMs();
				json       held = json::array();
				{
					std::lock_guard lock(m_mutex);
					for (const auto& lease : m_leases.Snapshot()) {
						json item = KeyJson(lease.key);
						item["owner"] = lease.owner;
						item["generation"] = lease.generation;
						item["heldForMs"] = std::max<std::int64_t>(0, now - lease.pressedAtMs);
						item["remainingMs"] = std::max<std::int64_t>(0, lease.expiresAtMs - now);
						held.push_back(std::move(item));
					}
				}
				return json{
					{ "contract", ContractJson() },
					{ "device", "keyboard" },
					{ "ready", g_inputReady.load(std::memory_order_relaxed) },
					{ "held", std::move(held) },
				};
			}

			json Down(const KeyboardKey& a_key, const std::string& a_owner, int a_maxHoldMs,
				bool a_requireFresh = false)
			{
				RequireReady();
				KeyboardAcquireResult acquired;
				QueueResult           queued;
				{
					std::lock_guard lock(m_mutex);
					if (m_leases.Size() >= kMaximumHeldKeys && !m_leases.Find(a_key.scancode))
						throw ToolError(409, std::format("keyboard synthetic hold limit reached ({}) — release a key or call releaseAll", kMaximumHeldKeys));
					acquired = m_leases.Acquire(a_key, a_owner, NowMs(), a_maxHoldMs);
					if (acquired.status == KeyboardAcquireStatus::kConflict)
						throw ToolError(409, std::format("key '{}' is held by owner '{}'", a_key.name, acquired.lease.owner));
					if (acquired.status == KeyboardAcquireStatus::kAlreadyOwned) {
						if (a_requireFresh)
							throw ToolError(409, std::format("key '{}' is already held by this owner; a tap/sequence will not release an earlier hold", a_key.name));
						json result = EventResult("down", acquired.lease, false, false);
						result["alreadyHeld"] = true;
						return result;
					}
					try {
						queued = QueueButton(a_key, true, 0.0F);
					} catch (...) {
						m_leases.RemoveExact(a_key.scancode, acquired.lease.generation);
						throw;
					}
				}

				Publish("down", acquired.lease, "request", queued.pending);
				ScheduleExpiry(acquired.lease);
				return EventResult("down", acquired.lease, queued.pending, false, queued.frame);
			}

			json Up(const KeyboardKey& a_key, const std::string& a_owner, bool a_force = false,
				std::string_view a_reason = "request")
			{
				RequireReady();
				std::lock_guard lock(m_mutex);
				const auto current = m_leases.Find(a_key.scancode);
				if (!current) {
					json result{ { "contract", ContractJson() }, { "device", "keyboard" },
						{ "action", "up" }, { "released", false }, { "notHeld", true } };
					result.update(KeyJson(a_key));
					return result;
				}
				if (!a_force && current->owner != a_owner)
					throw ToolError(409, std::format("key '{}' is held by owner '{}' (not '{}')", a_key.name, current->owner, a_owner));
				const float heldSecs = std::max(0.001F,
					static_cast<float>(NowMs() - current->pressedAtMs) / 1000.0F);
				const QueueResult queued = QueueButton(a_key, false, heldSecs);
				m_leases.RemoveExact(a_key.scancode, current->generation);
				Publish("up", *current, a_reason, queued.pending);
				return EventResult("up", *current, queued.pending, true, queued.frame);
			}

			json Tap(const KeyboardKey& a_key, const std::string& a_owner, int a_durationMs)
			{
				json down = Down(a_key, a_owner, std::min(kMaximumMaxHoldMs, a_durationMs + 2000), true);
				std::this_thread::sleep_for(milliseconds(a_durationMs));
				json up;
				try {
					up = Up(a_key, a_owner);
				} catch (...) {
					// The bounded lease remains and its watchdog will make a second release attempt.
					throw;
				}
				json result{
					{ "contract", ContractJson() }, { "device", "keyboard" }, { "action", "tap" },
					{ "durationMs", a_durationMs }, { "down", std::move(down) }, { "up", std::move(up) },
				};
				result.update(KeyJson(a_key));
				return result;
			}

			json Sequence(const json& a_args, const std::string& a_owner)
			{
				RequireReady();
				if (!a_args.contains("events") || !a_args["events"].is_array())
					throw ToolError(400, "action='sequence' requires an 'events' array");
				const auto& events = a_args["events"];
				if (events.empty() || events.size() > kMaximumSequenceEvents)
					throw ToolError(400, std::format("'events' must contain 1..{} entries", kMaximumSequenceEvents));

				// Validate the complete sequence before injecting anything. Persistent holds belong to
				// action='down'; a sequence is balanced by contract and cannot strand a modifier/key.
				std::unordered_set<std::uint16_t> balanced;
				int                               totalMs = 0;
				for (const auto& event : events) {
					if (!event.is_object())
						throw ToolError(400, "each sequence event must be an object");
					const std::string action = event.value("action", std::string("tap"));
					const int afterMs = BoundedInteger(event, "afterMs", 0, 0, 10000);
					totalMs += afterMs;
					if (action == "wait") {
						totalMs += BoundedInteger(event, "durationMs", 0, 0, 10000);
						continue;
					}
					const auto key = ParseKey(event);
					if (action == "tap") {
						totalMs += BoundedInteger(event, "durationMs", kDefaultTapMs, 10, 5000);
					} else if (action == "down") {
						if (!balanced.insert(key.scancode).second)
							throw ToolError(400, std::format("sequence presses key '{}' twice without an intervening up", key.name));
					} else if (action == "up") {
						if (!balanced.erase(key.scancode))
							throw ToolError(400, std::format("sequence releases key '{}' without a matching sequence down", key.name));
					} else {
						throw ToolError(400, std::format("unknown sequence event action '{}' (tap|down|up|wait)", action));
					}
				}
				if (!balanced.empty())
					throw ToolError(400, "sequence contains an unbalanced down; add a matching up or use action='down' for a bounded persistent hold");
				if (totalMs > kMaximumSequenceMs)
					throw ToolError(400, std::format("sequence duration {}ms exceeds the {}ms limit", totalMs, kMaximumSequenceMs));

				json results = json::array();
				std::vector<KeyboardLease> opened;
				try {
					for (const auto& event : events) {
						const std::string action = event.value("action", std::string("tap"));
						json result;
						if (action == "wait") {
							const int durationMs = BoundedInteger(event, "durationMs", 0, 0, 10000);
							std::this_thread::sleep_for(milliseconds(durationMs));
							result = json{ { "action", "wait" }, { "durationMs", durationMs } };
						} else {
							const auto key = ParseKey(event);
							if (action == "tap") {
								result = Tap(key, a_owner, BoundedInteger(event, "durationMs", kDefaultTapMs, 10, 5000));
							} else if (action == "down") {
								result = Down(key, a_owner, std::min(kMaximumMaxHoldMs, totalMs + 2000), true);
								opened.push_back(KeyboardLease{ key, a_owner, result.value("generation", 0ull), 0, 0 });
							} else {
								result = Up(key, a_owner);
								std::erase_if(opened, [&](const KeyboardLease& a_lease) { return a_lease.key.scancode == key.scancode; });
							}
						}
						results.push_back(std::move(result));
						const int afterMs = BoundedInteger(event, "afterMs", 0, 0, 10000);
						if (afterMs)
							std::this_thread::sleep_for(milliseconds(afterMs));
					}
				} catch (...) {
					for (const auto& lease : opened)
						ReleaseGeneration(lease.key.scancode, lease.generation, "sequenceFailure");
					throw;
				}

				return json{
					{ "contract", ContractJson() }, { "device", "keyboard" }, { "action", "sequence" },
					{ "owner", a_owner }, { "durationMs", totalMs }, { "eventsRun", results.size() },
					{ "results", std::move(results) },
				};
			}

			json ReleaseAll(const std::string& a_owner, bool a_all, std::string_view a_reason = "request")
			{
				RequireReady();
				std::vector<KeyboardLease> selected;
				{
					std::lock_guard lock(m_mutex);
					for (const auto& lease : m_leases.Snapshot())
						if (a_all || lease.owner == a_owner)
							selected.push_back(lease);
				}
				json released = json::array();
				json failed = json::array();
				for (const auto& lease : selected) {
					try {
						if (ReleaseGeneration(lease.key.scancode, lease.generation, a_reason))
							released.push_back(KeyJson(lease.key));
					} catch (const std::exception& e) {
						json item = KeyJson(lease.key);
						item["error"] = e.what();
						failed.push_back(std::move(item));
					}
				}
				return json{
					{ "contract", ContractJson() }, { "device", "keyboard" }, { "action", "releaseAll" },
					{ "owner", a_all ? "*" : a_owner }, { "released", std::move(released) },
					{ "failed", std::move(failed) },
				};
			}

		private:
			void RequireReady() const
			{
				if (!g_inputReady.load(std::memory_order_relaxed))
					throw ToolError(503, "keyboard input is advertised but not ready — SKSE kInputLoaded has not completed; inspect input capabilities/status and retry");
			}

			QueueResult QueueButton(const KeyboardKey& a_key, bool a_down, float a_heldSecs)
			{
				try {
					const json result = MainThread::RunAndWait([=]() -> json {
						auto* queue = RE::BSInputEventQueue::GetSingleton();
						if (!queue)
							throw ToolError(503, "Skyrim BSInputEventQueue unavailable");
						if (queue->buttonEventCount >= RE::BSInputEventQueue::MAX_BUTTON_EVENTS)
							throw ToolError(503, "Skyrim keyboard input queue is full for this frame — retry after the next frame");
						queue->AddButtonEvent(RE::INPUT_DEVICE::kKeyboard, 0, a_key.scancode,
							a_down ? 1.0F : 0.0F, a_down ? 0.0F : a_heldSecs);
						return json{ { "frame", game::CurrentFrame() } };
					});
					return { false, result.value("frame", -1) };
				} catch (const ToolError& e) {
					// RunAndWait's 504 means the task is already queued and will execute if the main
					// thread resumes. Treat that as accepted/pending so a down retains its lease and
					// watchdog, and an up can safely retire it without a retry racing the queued event.
					if (e.code == 504)
						return { true, -1 };
					throw;
				}
			}

			json EventResult(std::string_view a_action, const KeyboardLease& a_lease,
				bool a_pending, bool a_released, int a_frame = -1) const
			{
				json result{
					{ "contract", ContractJson() }, { "device", "keyboard" },
					{ "action", a_action }, { "owner", a_lease.owner },
					{ "generation", a_lease.generation }, { "accepted", true },
					{ "pending", a_pending }, { "released", a_released }, { "frame", a_frame },
				};
				result.update(KeyJson(a_lease.key));
				return result;
			}

			void Publish(std::string_view a_action, const KeyboardLease& a_lease,
				std::string_view a_reason, bool a_pending)
			{
				if (!m_events)
					return;
				json payload{
					{ "contractVersion", kKeyboardContractVersion }, { "device", "keyboard" },
					{ "action", a_action }, { "owner", a_lease.owner },
					{ "generation", a_lease.generation }, { "reason", a_reason }, { "pending", a_pending },
				};
				payload.update(KeyJson(a_lease.key));
				m_events->Publish("input.keyboard", std::move(payload));
			}

			void ScheduleExpiry(KeyboardLease a_lease)
			{
				const auto delay = milliseconds(std::max<std::int64_t>(0, a_lease.expiresAtMs - NowMs()));
				std::thread([lease = std::move(a_lease), delay]() {
					std::this_thread::sleep_for(delay);
					try {
						KeyboardManager::Get().ReleaseGeneration(lease.key.scancode, lease.generation, "leaseExpired");
					} catch (const std::exception& e) {
						logs::warn("devbench: automatic keyboard release failed for {} (generation {}): {}",
							lease.key.name, lease.generation, e.what());
					}
				}).detach();
			}

			bool ReleaseGeneration(std::uint16_t a_scancode, std::uint64_t a_generation,
				std::string_view a_reason)
			{
				std::lock_guard lock(m_mutex);
				const auto current = m_leases.Find(a_scancode);
				if (!current || current->generation != a_generation)
					return false;
				const float heldSecs = std::max(0.001F,
					static_cast<float>(NowMs() - current->pressedAtMs) / 1000.0F);
				const QueueResult queued = QueueButton(current->key, false, heldSecs);
				m_leases.RemoveExact(a_scancode, a_generation);
				Publish("up", *current, a_reason, queued.pending);
				return true;
			}

			std::mutex         m_mutex;
			KeyboardLeaseTable m_leases;
			EventBus*          m_events = nullptr;
		};

		json HandleInput(const json& a_args, const ToolContext& a_ctx)
		{
			if (!a_args.is_object())
				throw ToolError(400, "input arguments must be an object");
			const std::string action = a_args.value("action", std::string("capabilities"));
			if (action == "capabilities")
				return KeyboardManager::Get().Capabilities();
			if (a_args.contains("device") && (!a_args["device"].is_string() || a_args["device"] != "keyboard"))
				throw ToolError(400, "only device='keyboard' is implemented in input contract v1; inspect action='capabilities' before using later devices");
			if (action == "status")
				return KeyboardManager::Get().Status();

			const std::string owner = ResolveOwner(a_args, a_ctx);
			if (action == "down")
				return KeyboardManager::Get().Down(ParseKey(a_args), owner,
					BoundedInteger(a_args, "maxHoldMs", kDefaultMaxHoldMs, 100, kMaximumMaxHoldMs));
			if (action == "up")
				return KeyboardManager::Get().Up(ParseKey(a_args), owner, a_args.value("force", false));
			if (action == "tap")
				return KeyboardManager::Get().Tap(ParseKey(a_args), owner,
					BoundedInteger(a_args, "durationMs", kDefaultTapMs, 10, 5000));
			if (action == "sequence")
				return KeyboardManager::Get().Sequence(a_args, owner);
			if (action == "releaseAll")
				return KeyboardManager::Get().ReleaseAll(owner, a_args.value("all", false));
			throw ToolError(400, std::format("unknown input action '{}' (capabilities|status|down|up|tap|sequence|releaseAll)", action));
		}
	}

	void RegisterInputTool(ToolRegistry& a_registry, EventBus& a_events)
	{
		KeyboardManager::Get().SetEvents(a_events);
		ToolDescriptor input;
		input.name = "input";
		input.description =
			"Versioned synthetic input interface. action='capabilities' (default) returns the exact "
			"contract/version, readiness, limits, injection path, supported actions, and complete "
			"keyboard name→DirectInput-scan-code catalog; clients MUST capability-negotiate rather "
			"than assuming this tool exists. Contract v1 implements device='keyboard' using Skyrim's "
			"own BSInputEventQueue (not Windows SendInput, so window focus is irrelevant). 'status' "
			"reports readiness and every synthetic held key with owner/lease timing. 'down' starts a "
			"bounded owned hold (default maxHoldMs 5000; automatic up on expiry); repeated down by the "
			"same owner is idempotent and another owner gets 409. 'up' releases that owner's key "
			"(force=true overrides ownership); releasing a key not held by DevBench is an idempotent "
			"no-op. 'tap' emits down, waits durationMs (default 50), then up. 'sequence' executes a "
			"prevalidated balanced array of tap/down/up/wait events (optional afterMs); it rejects an "
			"unbalanced hold before injecting anything and releases keys acquired by the sequence on "
			"failure. 'releaseAll' releases this owner only, or all=true releases every DevBench-owned "
			"synthetic key. owner defaults to the MCP session id or rest:anonymous; automation should "
			"supply a stable task owner. DevBench also releases owned keys on load/new-game and emits "
			"input.keyboard events for accepted down/up transitions. This input namespace is designed "
			"to add a coherent HMD+left/right-controller capability in a later contract version.";
		input.inputSchema = json{
			{ "type", "object" },
			{ "properties", json{
				{ "action", json{ { "type", "string" }, { "enum", json::array({ "capabilities", "status", "down", "up", "tap", "sequence", "releaseAll" }) } } },
				{ "device", json{ { "type", "string" }, { "enum", json::array({ "keyboard" }) }, { "description", "v1 mutation/status device; omit for capabilities" } } },
				{ "key", json{ { "oneOf", json::array({ json{ { "type", "string" } }, json{ { "type", "integer" }, { "minimum", 1 }, { "maximum", 255 } } }) }, { "description", "down/up/tap: documented key name or raw DirectInput scancode" } } },
				{ "owner", json{ { "type", "string" }, { "minLength", 1 }, { "maxLength", 128 }, { "description", "stable task/session owner; defaults to MCP session id or rest:anonymous" } } },
				{ "durationMs", json{ { "type", "integer" }, { "minimum", 10 }, { "maximum", 5000 }, { "description", "tap duration (default 50); sequence tap/wait event duration" } } },
				{ "maxHoldMs", json{ { "type", "integer" }, { "minimum", 100 }, { "maximum", kMaximumMaxHoldMs }, { "description", "down safety lease (default 5000); automatic up at expiry" } } },
				{ "force", json{ { "type", "boolean" }, { "description", "up: override owner mismatch (default false)" } } },
				{ "all", json{ { "type", "boolean" }, { "description", "releaseAll: release all owners, not only this caller (default false)" } } },
				{ "events", json{ { "type", "array" }, { "minItems", 1 }, { "maxItems", kMaximumSequenceEvents }, { "description", "sequence: balanced [{action:tap|down|up|wait,key?,durationMs?,afterMs?}]" }, { "items", json{ { "type", "object" } } } } },
			} },
		};
		a_registry.Register(std::move(input), &HandleInput);
	}

	void MarkKeyboardInputReady()
	{
		g_inputReady.store(true, std::memory_order_relaxed);
		logs::info("devbench: keyboard input API ready (contract v{}, Skyrim BSInputEventQueue)", kKeyboardContractVersion);
	}

	void ReleaseKeyboardInputForLifecycle(const char* a_reason)
	{
		if (!g_inputReady.load(std::memory_order_relaxed))
			return;
		// OnMessage is a main-thread callback. Never acquire KeyboardManager::m_mutex there:
		// an input request may hold it while RunAndWait is waiting for this same main thread.
		// Hand the cleanup to a worker, which queues ordinary up events once this callback exits.
		const std::string reason = a_reason ? a_reason : "lifecycle";
		std::thread([reason]() {
			try {
				KeyboardManager::Get().ReleaseAll("lifecycle", true, reason);
			} catch (const std::exception& e) {
				logs::warn("devbench: lifecycle keyboard release failed ({}): {}", reason, e.what());
			}
		}).detach();
	}
}
