#pragma once

#include <cstdint>

namespace dvb
{
	class ToolRegistry;
}

namespace dvb::CalendarControl
{
	void Register(ToolRegistry& a_registry);
	void Reconcile();  // main thread; wall-time cleanup is independent of the engine frame counter
	void OnLifecycle(std::uint32_t a_type);  // main thread; never restore into a newer load generation
	void OnLoadingMenu();  // main thread; release before a scene transition when the old globals remain valid
	void RequestStop();  // listener-safe; cleanup runs on the engaged main-thread pump
	bool Outstanding();  // listener-safe; used to refuse saves/calendar jumps during a hold
}
