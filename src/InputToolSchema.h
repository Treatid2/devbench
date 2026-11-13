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
								{ "action", json{ { "type", "string" }, { "enum", json::array({ "capabilities", "status", "observe", "down", "up", "tap", "click", "move", "sequence", "stop", "releaseAll", "reach", "reachStatus", "reachStop" }) } } },
								{ "device", json{ { "type", "string" }, { "enum", json::array({ "keyboard", "mouse", "vrTrackedSet" }) }, { "description", "observe requires vrTrackedSet; omit device for capabilities" } } },
								{ "x", json{ { "type", "number" }, { "description", "mouse move: client-pixel x" } } },
								{ "y", json{ { "type", "number" }, { "description", "mouse move: client-pixel y" } } },
								{ "button", json{ { "type", "string" }, { "enum", json::array({ "left", "right", "middle" }) }, { "description", "mouse down/up/tap/click: button (default left)" } } },
								{ "focusWindow", json{ { "type", "boolean" }, { "description", "mouse move: bring the game window to the foreground and set the OS cursor too (default false)" } } },
								{ "key", json{ { "oneOf", json::array({ json{ { "type", "string" } }, json{ { "type", "integer" }, { "minimum", 1 }, { "maximum", 258 } } }) }, { "description", "down/up/tap: documented key name, raw DirectInput scancode (1-255), or mouse button code (256-258 = mouseLeft / mouseRight / mouseMiddle)" } } },
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
								{ "hands", json{ { "type", "object" }, { "description", "reach: {left?:{node,front?,offset?:[x,y,z]}, right?:{...}} - put that hand node-relative: next to a node of the player, front world units in front of it along the facing, plus a world offset; at least one hand" } } },
								{ "head", json{ { "type", "object" }, { "description", "reach: {lookAt:'hands'|<node name>, back?:0.5, up?:0.3, pos?:[x,y,z], eyeHeight?} - aim the HMD at the hands or a node from back meters behind and up meters above it (tracking space), or from pos; eyeHeight (0..3) sets the head's height in tracking meters, e.g. 1.6 to frame low targets from standing height" } } },
								{ "holdMs", json{ { "type", "integer" }, { "minimum", 100 }, { "maximum", 1798000 }, { "description", "reach: how long to hold the final pose (default 10000)" } } },
								{ "iterations", json{ { "type", "integer" }, { "minimum", 1 }, { "maximum", 10 }, { "description", "reach: correction passes (default 4)" } } },
								{ "toleranceUnits", json{ { "type", "number" }, { "minimum", 0.05 }, { "maximum", 50 }, { "description", "reach: converged when each hand is within this many world units of its target (default 1)" } } },
								{ "approachUnits", json{ { "type", "number" }, { "minimum", 0 }, { "maximum", 200 }, { "description", "reach: world units behind the target, along the facing, the first approach starts from (default 15)" } } },
								{ "runId", json{ { "type", "integer" }, { "minimum", 1 }, { "description", "reachStatus: the run (default the latest)" } } },
							} },
		};
	}
}
