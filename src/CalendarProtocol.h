#pragma once

#include "CalendarLease.h"
#include "ToolRegistry.h"

#include <format>

namespace dvb::CalendarControl
{
	struct Request
	{
		std::string action, owner, command, lease;
		std::int64_t duration = 0;
		json binding = json::object();
	};

	inline Request ParseRequest(const json& a_args)
	{
		if (!a_args.is_object() || !a_args.contains("action") || !a_args["action"].is_string())
			throw ToolError(400, "calendar requires a typed action");
		Request r;
		r.action = a_args["action"].get<std::string>();
		if (r.action != "status" && r.action != "hold" && r.action != "release")
			throw ToolError(400, "calendar action must be status, hold or release");
		for (const auto& [key, value] : a_args.items()) {
			const bool allowed = key == "action" || (r.action != "status" &&
				(key == "owner" || key == "commandId" || key == "binding" ||
				 (r.action == "hold" ? key == "holdMs" : key == "leaseId")));
			if (!allowed)
				throw ToolError(400, std::format("calendar does not accept '{}' for {}", key, r.action));
		}
		if (r.action == "status")
			return r;
		for (const auto* field : { "owner", "commandId" })
			if (!a_args.contains(field) || !a_args[field].is_string() || a_args[field].get_ref<const std::string&>().empty() ||
			    a_args[field].get_ref<const std::string&>().size() > 128)
				throw ToolError(400, std::format("calendar requires {} (1..128 characters)", field));
		if (!a_args.contains("binding") || !a_args["binding"].is_object())
			throw ToolError(400, "calendar requires the exact status (hold) or retained lease (release) binding object");
		r.binding = a_args["binding"];
		if (r.binding.size() != 5 || !r.binding.contains("processSession") || !r.binding["processSession"].is_string() ||
		    r.binding["processSession"].get_ref<const std::string&>().empty() ||
		    r.binding["processSession"].get_ref<const std::string&>().size() > 64 ||
		    !r.binding.contains("pid") || !r.binding["pid"].is_number_integer() ||
		    !r.binding.contains("loadGeneration") || !r.binding["loadGeneration"].is_number_integer() ||
		    !r.binding.contains("cellFormId") || !r.binding["cellFormId"].is_number_integer() ||
		    !r.binding.contains("globalFormIds") || !r.binding["globalFormIds"].is_array() || r.binding["globalFormIds"].size() != 6)
			throw ToolError(400, "calendar binding has invalid typed identity fields");
		if (r.binding["pid"] <= 0 || r.binding["loadGeneration"] <= 0 || r.binding["cellFormId"] < 0)
			throw ToolError(400, "calendar binding has invalid identity ranges");
		for (const auto& id : r.binding["globalFormIds"])
			if (!id.is_number_integer() || id <= 0)
				throw ToolError(400, "calendar global identities must be positive integers");
		r.owner = a_args["owner"].get<std::string>();
		r.command = a_args["commandId"].get<std::string>();
		if (r.action == "hold") {
			if (!a_args.contains("holdMs") || !a_args["holdMs"].is_number_integer())
				throw ToolError(400, "calendar holdMs must be an integer");
			r.duration = a_args["holdMs"].get<std::int64_t>();
			if (r.duration < 1 || r.duration > kMaximumHoldMs)
				throw ToolError(400, "calendar holdMs must be 1..300000");
		} else {
			if (!a_args.contains("leaseId") || !a_args["leaseId"].is_string() ||
			    a_args["leaseId"].get_ref<const std::string&>().empty() || a_args["leaseId"].get_ref<const std::string&>().size() > 128)
				throw ToolError(400, "calendar release requires leaseId (1..128 characters)");
			r.lease = a_args["leaseId"].get<std::string>();
		}
		return r;
	}

	inline json SourceBinding(const Source& a_source, std::uint32_t a_pid)
	{
		return json{ { "processSession", a_source.processSession }, { "pid", a_pid },
			{ "loadGeneration", a_source.generation }, { "cellFormId", a_source.cell },
			{ "globalFormIds", a_source.forms } };
	}

	// Shared native/host admission policy. A release validates retained custody,
	// not the current scene. Controller::Restore independently re-reads SameStorage
	// and the owned-zero rate before writing; scene drift never relaxes those guards.
	inline Outcome ExecuteRequest(const Request& a_request, Controller& a_controller,
		const Snapshot& a_current, std::uint32_t a_pid, const std::string& a_connection,
		std::int64_t a_now, std::int64_t a_applyDeadline, bool a_stopping)
	{
		if (a_request.action == "status")
			return { true, "observed", false };
		if (a_stopping)
			return { false, "service_stopping", false };
		if (a_request.action == "hold") {
			if (a_request.binding != SourceBinding(a_current.source, a_pid))
				return { false, "binding_mismatch", false };
			return a_controller.Hold(a_current.source, a_request.owner, a_connection,
				a_request.command, a_now, a_request.duration, a_applyDeadline);
		}
		const auto& selected = a_controller.Current() ? a_controller.Current() : a_controller.Last();
		if (!selected)
			return { false, "wrong_owner_or_lease", false };
		if (a_request.binding != SourceBinding(selected->baseline.source, a_pid))
			return { false, "binding_mismatch", false };
		return a_controller.Release(selected->baseline.source, a_request.owner,
			a_connection, a_request.lease);
	}

	inline ToolDescriptor Descriptor()
	{
		ToolDescriptor tool;
		tool.name = "calendar";
		tool.description = "Read full calendar state or hold only its observed progression rate at zero. Does not set hour/date or change engine speed. "
			"hold/release require explicit owner and commandId. hold requires exact current status binding; release requires exact retained lease binding and leaseId, "
			"even after a cell change, with independently verified same storage/generation and owned zero before restoring. Holds last 1..300000 wall milliseconds, "
			"then restore captured rate on the main thread, without rewinding dates. MCP ownership also binds its session; REST ownership is cooperative, "
			"not authentication. Disconnect recovery is expiry-bounded, not immediate. Never save during a hold. Read receipts for partial/invalidated cleanup; "
			"a stalled main thread cannot restore at its deadline. No weather, exposure, physics or rendering control.";
		tool.inputSchema = json{
			{ "type", "object" }, { "required", json::array({ "action" }) }, { "additionalProperties", false },
			{ "properties", {
				{ "action", { { "type", "string" }, { "enum", json::array({ "status", "hold", "release" }) } } },
				{ "owner", { { "type", "string" }, { "minLength", 1 }, { "maxLength", 128 } } },
				{ "commandId", { { "type", "string" }, { "minLength", 1 }, { "maxLength", 128 } } },
				{ "binding", {
					{ "type", "object" }, { "additionalProperties", false },
					{ "required", json::array({ "processSession", "pid", "loadGeneration", "cellFormId", "globalFormIds" }) },
					{ "properties", {
						{ "processSession", { { "type", "string" }, { "minLength", 1 }, { "maxLength", 64 } } },
						{ "pid", { { "type", "integer" }, { "minimum", 1 } } },
						{ "loadGeneration", { { "type", "integer" }, { "minimum", 1 } } },
						{ "cellFormId", { { "type", "integer" }, { "minimum", 0 } } },
						{ "globalFormIds", { { "type", "array" }, { "minItems", 6 }, { "maxItems", 6 }, { "items", { { "type", "integer" }, { "minimum", 1 } } } } }
					} }
				} },
				{ "holdMs", { { "type", "integer" }, { "minimum", 1 }, { "maximum", kMaximumHoldMs } } },
				{ "leaseId", { { "type", "string" }, { "minLength", 1 }, { "maxLength", 128 } } }
			} },
			{ "oneOf", json::array({
				json{ { "properties", { { "action", { { "const", "status" } } } } }, { "maxProperties", 1 } },
				json{ { "properties", { { "action", { { "const", "hold" } } } } }, { "required", json::array({ "owner", "commandId", "binding", "holdMs" }) }, { "maxProperties", 5 } },
				json{ { "properties", { { "action", { { "const", "release" } } } } }, { "required", json::array({ "owner", "commandId", "binding", "leaseId" }) }, { "maxProperties", 5 } }
			}) }
		};
		return tool;
	}
}
