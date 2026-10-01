#pragma once

#include "Json.h"

namespace dvb::NewGameControl
{
	/// Inspect, request, or confirm the VR main menu's normal New Game flow.
	/// Mutation receipts prove dispatch only; callers must verify world entry.
	json Handle(const json& a_args);
}
