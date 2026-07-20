#include "MainThread.h"

#include "GameState.h"     // dvb::game::CurrentFrame
#include "ToolRegistry.h"  // dvb::ToolError

#include <future>
#include <memory>

namespace dvb::MainThread
{
	json RunAndWait(std::function<json()> a_fn, std::chrono::milliseconds a_timeout)
	{
		auto* task = SKSE::GetTaskInterface();
		if (!task)
			throw ToolError(500, "SKSE TaskInterface unavailable");

		// Shared so the promise outlives a timed-out wait: if we return before the
		// task runs, the task can still set the (now-abandoned) promise safely.
		auto promise = std::make_shared<std::promise<json>>();
		auto future = promise->get_future();

		task->AddTask([fn = std::move(a_fn), promise]() {
			try {
				promise->set_value(fn());
			} catch (...) {
				try {
					promise->set_exception(std::current_exception());
				} catch (...) {
				}
			}
		});

		// The engine's frame counter discriminates the two 504 causes: still
		// advancing = main thread busy (retry can succeed); frozen = hung OR
		// fully paused (the counter also freezes in pause menus).
		const int frameAtStart = game::CurrentFrame();
		if (future.wait_for(a_timeout) != std::future_status::ready) {
			const int frameNow = game::CurrentFrame();
			if (frameAtStart < 0 || frameNow < 0)
				throw ToolError(504, std::format("main-thread task did not run within {}ms", a_timeout.count()));
			if (frameNow == frameAtStart)
				throw ToolError(504, std::format(
										 "main-thread task did not run within {}ms and the game frame counter has not advanced -- main thread hung or the game is fully paused; if no pause menu is open, only a process restart recovers",
										 a_timeout.count()));
			throw ToolError(504, std::format(
									 "main-thread task did not run within {}ms ({} frames elapsed -- main thread busy, a retry may succeed)",
									 a_timeout.count(), frameNow - frameAtStart));
		}

		return future.get();  // rethrows the handler's exception on the listener thread
	}
}
