#include "RecordingActivity.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace dvb::Recording
{
	namespace
	{
		bool IsKeyboardTransition(const json& a_event)
		{
			if (a_event.value("kind", std::string{}) != "input" ||
				a_event.value("eventType", std::string{}) != "button" ||
				a_event.value("device", std::string{}) != "keyboard")
				return false;
			const std::string state = a_event.value("state", std::string{});
			const int         id = a_event.value("idCode", 0);
			return (state == "down" || state == "up") && id > 0 && id <= 255;
		}

		json InputStep(const json& a_event, const std::string& a_owner)
		{
			json args{
				{ "action", a_event.value("state", std::string{}) },
				{ "device", "keyboard" },
				{ "key", a_event.value("idCode", 0) },
				{ "owner", a_owner },
			};
			if (args["action"] == "down")
				args["maxHoldMs"] = 60000;
			return json{ { "tool", "input" }, { "args", std::move(args) },
				{ "label", "recorded keyboard input" } };
		}

		void AppendWait(json& a_steps, std::int64_t a_ms)
		{
			if (a_ms <= 0)
				return;
			if (!a_steps.empty() && a_steps.back().is_object() && a_steps.back().size() == 1 &&
				a_steps.back().contains("wait")) {
				a_steps.back()["wait"] = a_steps.back()["wait"].get<std::int64_t>() + a_ms;
			} else {
				a_steps.push_back(json{ { "wait", a_ms } });
			}
		}
	}

	json ActivityCaptureContract()
	{
		return json{
			{ "name", "devbench.recording.activity" },
			{ "version", json{ { "major", 1 }, { "minor", 0 } } },
			{ "clock", "steadyMillisecondsFromRecordStart" },
			{ "ordering", "seq" },
			{ "inputLayer", "Skyrim.BSInputDeviceManager" },
			{ "normalized", true },
			{ "eventKinds", json::array({ "input", "menu", "lifecycle", "cell", "console" }) },
			{ "inputEventTypes", json::array({ "button", "mouseMove", "char", "thumbstick",
				"deviceConnect", "kinect", "vrTouchpadPosition", "vrTouchpadSwipe", "unknown" }) },
			{ "replay", json{
				{ "keyboardButtonTransitions", true },
				{ "controllerButtonTransitions", false },
				{ "analogAndPointerEvents", false },
				{ "observationalEvents", false },
				{ "policy", "capture-all-replay-only-declared" },
			} },
		};
	}

	json SummarizeActivity(const json& a_events)
	{
		json counts{
			{ "total", 0 }, { "input", 0 }, { "menu", 0 }, { "lifecycle", 0 }, { "cell", 0 },
			{ "console", 0 },
			{ "keyboardTransitions", 0 }, { "unsupportedInput", 0 },
		};
		if (!a_events.is_array())
			return counts;
		counts["total"] = a_events.size();
		for (const auto& event : a_events) {
			const std::string kind = event.value("kind", std::string{});
			if (counts.contains(kind))
				counts[kind] = counts[kind].get<std::size_t>() + 1;
			if (kind == "input") {
				if (IsKeyboardTransition(event))
					counts["keyboardTransitions"] = counts["keyboardTransitions"].get<std::size_t>() + 1;
				else
					counts["unsupportedInput"] = counts["unsupportedInput"].get<std::size_t>() + 1;
			}
		}
		return counts;
	}

	json InterleaveReplayableActivity(const json& a_steps, const json& a_events,
		const std::string& a_inputOwner, bool a_replayInputs)
	{
		json report = SummarizeActivity(a_events);
		report["enabled"] = a_replayInputs;
		report["replayedKeyboardTransitions"] = 0;
		report["skippedInput"] = report.value("input", 0);

		if (!a_replayInputs || !a_events.is_array())
			return json{ { "steps", a_steps }, { "report", std::move(report) }, { "inputOwner", "" } };

		std::vector<json> replayable;
		for (const auto& event : a_events)
			if (IsKeyboardTransition(event))
				replayable.push_back(event);
		std::stable_sort(replayable.begin(), replayable.end(), [](const json& a, const json& b) {
			const auto at = a.value("tMs", static_cast<std::int64_t>(0));
			const auto bt = b.value("tMs", static_cast<std::int64_t>(0));
			return at != bt ? at < bt : a.value("seq", 0ull) < b.value("seq", 0ull);
		});

		json        steps = json::array();
		std::size_t eventIndex = 0;
		std::int64_t clockMs = 0;
		const auto emitThrough = [&](std::int64_t a_endMs, json& a_out, std::int64_t& a_clock,
			std::size_t& a_index) {
			while (a_index < replayable.size() &&
				replayable[a_index].value("tMs", static_cast<std::int64_t>(0)) <= a_endMs) {
				const auto eventMs = std::max(a_clock,
					replayable[a_index].value("tMs", static_cast<std::int64_t>(0)));
				AppendWait(a_out, eventMs - a_clock);
				a_clock = eventMs;
				a_out.push_back(InputStep(replayable[a_index], a_inputOwner));
				++a_index;
			}
		};

		if (a_steps.is_array()) {
			for (const auto& original : a_steps) {
				emitThrough(clockMs, steps, clockMs, eventIndex);
				json step = original;
				const auto waitMs = std::max<std::int64_t>(0, step.value("wait", static_cast<std::int64_t>(0)));
				step.erase("wait");
				// atMs is archival ground truth for later planners; the current scenario DSL uses
				// the established wait clock and must not receive a metadata-only step.
				step.erase("atMs");
				if (!step.empty())
					steps.push_back(std::move(step));
				const auto endMs = clockMs + waitMs;
				emitThrough(endMs, steps, clockMs, eventIndex);
				AppendWait(steps, endMs - clockMs);
				clockMs = endMs;
			}
		}
		while (eventIndex < replayable.size()) {
			const auto eventMs = std::max(clockMs,
				replayable[eventIndex].value("tMs", static_cast<std::int64_t>(0)));
			AppendWait(steps, eventMs - clockMs);
			clockMs = eventMs;
			steps.push_back(InputStep(replayable[eventIndex++], a_inputOwner));
		}

		report["replayedKeyboardTransitions"] = replayable.size();
		report["skippedInput"] = report.value("input", 0) - static_cast<int>(replayable.size());
		report["partial"] = report.value("skippedInput", 0) > 0;
		return json{ { "steps", std::move(steps) }, { "report", std::move(report) },
			{ "inputOwner", replayable.empty() ? "" : a_inputOwner } };
	}
}
