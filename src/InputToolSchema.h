#pragma once

#include "Json.h"
#include <cstdint>

namespace dvb
{
	// Exact schema used by native input registration. Keep host-independent so
	// registry listing coverage can verify the public contract without hooks/game.
	// This union does not make every action valid for every device; handlers retain
	// device-specific checks. In particular observe is read-only vrTrackedSet only.
	inline json InputToolSchema(std::uint32_t maximumHoldMs,
		std::uint32_t maximumSequenceEvents, std::uint32_t maximumVRTrackedFrames)
	{
		return json{
			{ "type", "object" },
			{ "properties", json{
								{ "action", json{ { "type", "string" }, { "enum", json::array({ "capabilities", "status", "observe", "down", "up", "tap", "sequence", "stop", "releaseAll" }) } } },
								{ "device", json{ { "type", "string" }, { "enum", json::array({ "keyboard", "vrTrackedSet" }) }, { "description", "observation/mutation/status device; observe requires vrTrackedSet; omit for capabilities" } } },
								{ "key", json{ { "oneOf", json::array({ json{ { "type", "string" } }, json{ { "type", "integer" }, { "minimum", 1 }, { "maximum", 258 } } }) }, { "description", "down/up/tap: documented key name or raw DirectInput scancode; keyboard button events have no VR wand association; mouseLeft / mouseRight / mouseMiddle (256-258) are mouse buttons" } } },
								{ "owner", json{ { "type", "string" }, { "minLength", 1 }, { "maxLength", 128 }, { "description", "stable task/session owner; defaults to MCP session id or rest:anonymous" } } },
								{ "durationMs", json{ { "type", "integer" }, { "minimum", 10 }, { "maximum", 5000 }, { "description", "tap duration (default 50); sequence tap/wait event duration" } } },
								{ "maxHoldMs", json{ { "type", "integer" }, { "minimum", 100 }, { "maximum", maximumHoldMs }, { "description", "down safety lease (default 5000); automatic up at expiry" } } },
								{ "force", json{ { "type", "boolean" }, { "description", "internal cleanup only; external true is rejected" } } },
								{ "all", json{ { "type", "boolean" }, { "description", "internal cleanup only; external true is rejected" } } },
								{ "events", json{ { "type", "array" }, { "minItems", 1 }, { "maxItems", maximumSequenceEvents }, { "description", "sequence: balanced [{action:tap|down|up|wait,key?,durationMs?,afterMs?}]" }, { "items", json{ { "type", "object" } } } } },
								{ "frames", json{ { "type", "array" }, { "minItems", 1 }, { "maxItems", maximumVRTrackedFrames }, { "description", "vrTrackedSet sequence: monotonic atomic frames; each requires tMs, originCode, hmd, left, right; controller objects carry packetNumber/pressed/touched/five axes" }, { "items", json{ { "type", "object" } } } } },
								{ "tailMs", json{ { "type", "integer" }, { "minimum", 10 }, { "maximum", 1000 }, { "description", "vrTrackedSet sequence: final-state visibility before automatic release (default 50ms)" } } },
								{ "controlToken", json{ { "type", "string" }, { "description", "vrTrackedSet stop/releaseAll: opaque token returned by sequence start" } } },
								{ "surviveLifecycle", json{ { "type", "boolean" }, { "description", "internal recording replay only; external true is rejected" } } },
							} },
		};
	}
}
