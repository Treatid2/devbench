#pragma once

namespace dvb
{
	class EventBus;
	class ToolRegistry;

	// Register the versioned `input` tool. The keyboard capability is advertised from
	// kPostLoad, but mutation reports 503 until MarkKeyboardInputReady() is called at
	// kInputLoaded and Skyrim's native input queue exists.
	void RegisterInputTool(ToolRegistry& a_registry, EventBus& a_events);
	void MarkKeyboardInputReady();

	// Called from lifecycle messages already running on Skyrim's main thread. It schedules an
	// off-thread release of only DevBench-owned synthetic keys; it must never wait/lock there.
	void ReleaseKeyboardInputForLifecycle(const char* a_reason);
}
