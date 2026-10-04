#pragma once

#include "ToolRegistry.h"

namespace dvb::CalendarControl
{
	// Shared production/test seam. Outstanding includes unverified cleanup, not
	// only an unexpired hold. Check before queueing and again on the main thread.
	inline void RequireIdle(bool a_outstanding)
	{
		if (a_outstanding)
			throw ToolError(409, "calendar custody is outstanding; release and verify restoration before wait/sleep");
	}
}
