#include "test_framework.h"

#include "MainThreadTask.h"
#include "NewGameRequestLedger.h"

#include <latch>
#include <stdexcept>
#include <thread>

using dvb::NewGameControl::RequestLedger;
using dvb::MainThread::QueuedTask;
using namespace std::chrono_literals;

TEST_CASE("New Game selection failure keeps a barrier before pending movie cleanup")
{
	RequestLedger receipts;
	receipts.BeginRequest("selection-failed", 1);
	// The selection entered GFx, then failed before NEW was called.
	receipts.Invalidate("selection-failed", "expired");
	CHECK(receipts.Snapshot("selection-failed")["phase"] == "dispatchUncertain");
	CHECK(receipts.UnresolvedID() == "selection-failed");
	CHECK(!receipts.CanRequest());
	CHECK_THROWS(receipts.BeginRequest("fresh", 1));
}

TEST_CASE("New Game partial confirmation mutation followed by exception stays uncertain")
{
	RequestLedger receipts;
	receipts.BeginRequest("partial", 1);
	CHECK(receipts.CompleteRequest("partial", "MainConfirm", true));
	bool mutated = false;
	const auto confirm = [&]() {
		receipts.BeginConfirmation("partial");
		mutated = true;  // Stand-in for onAcceptPress starting the normal fade.
		throw std::runtime_error("GFx callback failed after mutation");
	};
	CHECK_THROWS_AS(confirm(), std::runtime_error);
	CHECK(mutated);
	receipts.Invalidate("partial", "menuInterrupted");
	CHECK(receipts.Snapshot("partial")["phase"] == "dispatchUncertain");
	CHECK(!receipts.CanRequest());
	CHECK_THROWS(receipts.BeginConfirmation("partial"));
	CHECK_THROWS(receipts.BeginRequest("fresh", 1));
}

TEST_CASE("New Game failed post-dispatch verification cannot acknowledge confirmation")
{
	RequestLedger receipts;
	receipts.BeginRequest("verify-failed", 1);
	CHECK(receipts.CompleteRequest("verify-failed", "MainConfirm", true));
	receipts.BeginConfirmation("verify-failed");
	// An unreadable or non-StartNewGame fade callback never calls completion.
	CHECK(receipts.Snapshot("verify-failed")["accepted"] == false);
	for (const auto* reason : { "menuInterrupted", "menuReplaced", "expired" }) {
		receipts.Invalidate("verify-failed", reason);
		const auto snapshot = receipts.Snapshot("verify-failed");
		CHECK(snapshot["phase"] == "dispatchUncertain");
		CHECK(snapshot["invalidationReason"] == reason);
		CHECK(snapshot["unresolvedDispatch"] == true);
		CHECK(!receipts.CanRequest());
	}
	CHECK_THROWS(receipts.BeginRequest("fresh", 1));
}

TEST_CASE("New Game known receipt inspection is immutable and preserves uncertainty")
{
	RequestLedger receipts;
	receipts.BeginRequest("inspect", 0);
	receipts.Invalidate("inspect", "menuReplaced");
	auto copy = receipts.Snapshot("inspect");
	copy["phase"] = "expired";
	CHECK(receipts.Snapshot("inspect")["phase"] == "dispatchUncertain");
	CHECK(receipts.Snapshot("inspect")["completed"] == false);
	CHECK(receipts.UnresolvedID() == "inspect");
	CHECK_THROWS(receipts.BeginRequest("inspect", 0));
	CHECK_THROWS(receipts.BeginRequest("fresh", 0));
}

TEST_CASE("New Game started queue timeout cannot abandon a partially mutating confirmation")
{
	RequestLedger receipts;
	receipts.BeginRequest("started-timeout", 1);
	CHECK(receipts.CompleteRequest("started-timeout", "MainConfirm", true));
	std::latch started(1);
	std::latch finish(1);
	QueuedTask task([&]() -> dvb::json {
		receipts.BeginConfirmation("started-timeout");
		started.count_down();
		finish.wait();
		throw std::runtime_error("partial GFx mutation then failure");
	}, QueuedTask::Clock::now() + 1min);
	auto future = task.GetFuture();
	std::jthread gameThread([&]() { task.Run(); });
	started.wait();
	// Same production cancellation primitive used by RunAndWait's timeout path.
	CHECK(future.wait_for(0ms) == std::future_status::timeout);
	CHECK(!task.Abandon());
	finish.count_down();
	gameThread.join();
	CHECK_THROWS_AS(future.get(), std::runtime_error);
	receipts.Invalidate("started-timeout", "menuInterrupted");
	CHECK(receipts.Snapshot("started-timeout")["phase"] == "dispatchUncertain");
	CHECK_THROWS(receipts.BeginRequest("fresh", 1));
}

TEST_CASE("New Game definitive late completion resolves only its own started operation")
{
	RequestLedger receipts;
	receipts.BeginRequest("late", 1);
	CHECK_THROWS(receipts.CompleteConfirmation("late"));
	CHECK(receipts.CompleteRequest("late", "MainConfirm", true));
	receipts.BeginConfirmation("late");
	receipts.Invalidate("late", "menuInterrupted");
	CHECK(!receipts.CanRequest());
	// The same running callback eventually verified StartNewGame scheduling.
	receipts.CompleteConfirmation("late");
	CHECK(receipts.Snapshot("late")["phase"] == "dispatched");
	CHECK(receipts.Snapshot("late")["accepted"] == true);
	CHECK(receipts.Snapshot("late")["completed"] == false);
	CHECK(receipts.UnresolvedID().empty());
	CHECK_THROWS(receipts.BeginConfirmation("late"));
	receipts.Invalidate("late", "menuReplaced");
	CHECK(receipts.Snapshot("late")["phase"] == "dispatched");
}

TEST_CASE("New Game merely requested confirmation may expire before dispatch")
{
	RequestLedger receipts;
	receipts.BeginRequest("not-confirmed", 1);
	CHECK(receipts.CompleteRequest("not-confirmed", "MainConfirm", true));
	receipts.Invalidate("not-confirmed", "expired");
	CHECK(receipts.Snapshot("not-confirmed")["phase"] == "expired");
	CHECK(receipts.Snapshot("not-confirmed")["unresolvedDispatch"] == false);
	CHECK(!receipts.IsRequested("not-confirmed"));
	CHECK(receipts.CanRequest());
	CHECK_THROWS(receipts.BeginRequest("not-confirmed", 1));
	CHECK_NOTHROW(receipts.BeginRequest("fresh", 1));
}

TEST_CASE("New Game bounded history never forgets IDs to admit fresh requests")
{
	RequestLedger receipts;
	for (std::size_t i = 0; i < RequestLedger::kMaximumRequests; ++i) {
		const auto id = std::to_string(i);
		receipts.BeginRequest(id, 1);
		CHECK(receipts.CompleteRequest(id, "MainConfirm", true));
		receipts.Invalidate(id, "expired");
	}
	CHECK(!receipts.CanRequest());
	CHECK(receipts.Contains("0"));
	CHECK_THROWS(receipts.BeginRequest("overflow", 1));
}

namespace
{
	void CheckUnverifiedRequest(const char* a_state, bool a_selectedNew)
	{
		RequestLedger receipts;
		receipts.BeginRequest("unverified", 1);
		CHECK(!receipts.CompleteRequest("unverified", a_state, a_selectedNew));
		CHECK(!receipts.IsRequested("unverified"));
		CHECK(receipts.Snapshot("unverified")["state"] == a_state);
		CHECK(receipts.Snapshot("unverified")["selectedNew"] == a_selectedNew);
		for (const auto* reason : { "menuClosed", "menuReplaced", "menuInterrupted", "expired" }) {
			receipts.Invalidate("unverified", reason);
			const auto snapshot = receipts.Snapshot("unverified");
			CHECK(snapshot["phase"] == "dispatchUncertain");
			CHECK(snapshot["invalidationReason"] == reason);
			CHECK(snapshot["unresolvedDispatch"] == true);
			CHECK(snapshot["accepted"] == false);
			CHECK(snapshot["completed"] == false);
			auto inspection = snapshot;
			inspection["phase"] = "requested";
			CHECK(receipts.Snapshot("unverified")["phase"] == "dispatchUncertain");
			CHECK(receipts.UnresolvedID() == "unverified");
			CHECK(!receipts.CanRequest());
			CHECK_THROWS(receipts.BeginRequest("fresh", 1));
			CHECK_THROWS(receipts.BeginRequest("unverified", 1));
			CHECK_THROWS(receipts.BeginConfirmation("unverified"));
		}
	}
}

TEST_CASE("New Game readable Main after NEW cannot resolve request uncertainty")
{
	CheckUnverifiedRequest("Main", true);
}

TEST_CASE("New Game other readable post-NEW states cannot resolve request uncertainty")
{
	for (const auto* state : { "Transition", "StartNewGame", "", "mainconfirm" })
		CheckUnverifiedRequest(state, true);
}

TEST_CASE("New Game MainConfirm without the selected semantic New remains uncertain")
{
	CheckUnverifiedRequest("MainConfirm", false);
}

TEST_CASE("New Game started request timeout cannot acknowledge an unsupported postcondition")
{
	RequestLedger receipts;
	std::latch started(1);
	std::latch finish(1);
	QueuedTask task([&]() -> dvb::json {
		receipts.BeginRequest("request-timeout", 1);
		started.count_down();
		finish.wait();
		if (!receipts.CompleteRequest("request-timeout", "Main", true))
			throw std::runtime_error("NEW returned without supported confirmation");
		return receipts.Snapshot("request-timeout");
	}, QueuedTask::Clock::now() + 1min);
	auto future = task.GetFuture();
	std::jthread gameThread([&]() { task.Run(); });
	started.wait();
	CHECK(future.wait_for(0ms) == std::future_status::timeout);
	CHECK(!task.Abandon());
	finish.count_down();
	gameThread.join();
	CHECK_THROWS_AS(future.get(), std::runtime_error);
	receipts.Invalidate("request-timeout", "expired");
	CHECK(receipts.Snapshot("request-timeout")["phase"] == "dispatchUncertain");
	CHECK_THROWS(receipts.BeginRequest("fresh", 1));
}
