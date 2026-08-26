#include "test_framework.h"

#include "RecordingActivity.h"

using dvb::json;
using dvb::Recording::ActivityCaptureContract;
using dvb::Recording::InterleaveReplayableActivity;
using dvb::Recording::SummarizeActivity;

namespace
{
	json Button(std::int64_t a_ms, std::uint64_t a_seq, const char* a_device,
		const char* a_state, int a_id)
	{
		return json{ { "kind", "input" }, { "eventType", "button" },
			{ "device", a_device }, { "state", a_state }, { "idCode", a_id },
			{ "tMs", a_ms }, { "seq", a_seq } };
	}
}

TEST_CASE("activity contract declares capture-all and partial replay honestly")
{
	const json contract = ActivityCaptureContract();
	CHECK(contract["version"]["major"] == 1);
	CHECK(contract["inputLayer"] == "Skyrim.BSInputDeviceManager");
	CHECK(contract["replay"]["keyboardButtonTransitions"] == true);
	CHECK(contract["replay"]["controllerButtonTransitions"] == false);
}

TEST_CASE("activity summary separates replayable keyboard transitions from preserved input")
{
	const json events = json::array({
		Button(10, 1, "keyboard", "down", 17),
		Button(20, 2, "keyboard", "held", 17),
		Button(30, 3, "oculusSecondary", "down", 33),
		json{ { "kind", "menu" }, { "name", "InventoryMenu" }, { "opening", true } },
	});
	const json summary = SummarizeActivity(events);
	CHECK(summary["total"] == 4);
	CHECK(summary["input"] == 3);
	CHECK(summary["menu"] == 1);
	CHECK(summary["keyboardTransitions"] == 1);
	CHECK(summary["unsupportedInput"] == 2);
}

TEST_CASE("keyboard transitions interleave without changing the original trajectory clock")
{
	const json steps = json::array({
		json{ { "pose", json::array({ 1, 2, 3, 4, 5 }) }, { "wait", 100 } },
	});
	const json events = json::array({
		Button(25, 1, "keyboard", "down", 17),
		Button(50, 2, "keyboard", "held", 17),
		Button(60, 3, "oculusSecondary", "down", 33),
		Button(75, 4, "keyboard", "up", 17),
	});
	const json plan = InterleaveReplayableActivity(steps, events, "recording:test", true);
	CHECK(plan["report"]["replayedKeyboardTransitions"] == 2);
	CHECK(plan["report"]["skippedInput"] == 2);
	CHECK(plan["report"]["partial"] == true);

	std::int64_t totalWait = 0;
	json         transitions = json::array();
	for (const auto& step : plan["steps"]) {
		if (step.contains("wait"))
			totalWait += step["wait"].get<std::int64_t>();
		if (step.value("tool", std::string{}) == "input")
			transitions.push_back(step["args"]["action"]);
	}
	CHECK(totalWait == 100);
	CHECK(transitions == json::array({ "down", "up" }));
	CHECK(plan["inputOwner"] == "recording:test");
}

TEST_CASE("input replay can be disabled without altering legacy steps")
{
	const json steps = json::array({ json{ { "wait", 40 } } });
	const json events = json::array({ Button(10, 1, "keyboard", "down", 17) });
	const json plan = InterleaveReplayableActivity(steps, events, "recording:test", false);
	CHECK(plan["steps"] == steps);
	CHECK(plan["report"]["enabled"] == false);
	CHECK(plan["report"]["replayedKeyboardTransitions"] == 0);
	CHECK(plan["inputOwner"] == "");
}
