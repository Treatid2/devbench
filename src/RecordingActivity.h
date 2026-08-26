#pragma once

#include "Json.h"

#include <string>

namespace dvb::Recording
{
	// Host-independent contract and replay planner for the activity stream stored beside a
	// trajectory. The game-facing recorder creates the events; this module decides which of
	// them can be reproduced by the currently advertised input contract and interleaves those
	// transitions without changing the trajectory's recorded clock.
	json ActivityCaptureContract();
	json SummarizeActivity(const json& a_events);

	// Returns { steps, report, inputOwner }. Only keyboard button down/up transitions are
	// currently replayable. All other captured input remains in the recording and is counted
	// explicitly in report.skippedInput; it is never silently approximated.
	json InterleaveReplayableActivity(const json& a_steps, const json& a_events,
		const std::string& a_inputOwner, bool a_replayInputs);
}
