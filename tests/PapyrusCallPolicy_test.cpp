#include "test_framework.h"
#include "PapyrusCallPolicy.h"

using dvb::json;
using dvb::PapyrusCallPolicy::Lifecycle;
using Clock = Lifecycle::Clock;

TEST_CASE("Papyrus exact argument admission never guesses omissions or accepts extras")
{
	for (std::size_t n = 0; n < 16; ++n) {
		CHECK(dvb::PapyrusCallPolicy::ExactArgumentCount(n, n));
		CHECK(!dvb::PapyrusCallPolicy::ExactArgumentCount(n, n + 1));
		CHECK(!dvb::PapyrusCallPolicy::ExactArgumentCount(n + 1, n));
	}
}

TEST_CASE("Papyrus queued timeout prevents even late preparation")
{
	const auto now = Clock::time_point{};
	Lifecycle state(json{ { "suppliedArgs", json::array({ 7 }) } }, now + std::chrono::seconds(1));
	state.TimedOut();
	const auto snapshot = state.Snapshot();
	CHECK(snapshot["phase"] == "abandoned_before_dispatch");
	CHECK(!snapshot["mayStillExecute"].get<bool>());
	CHECK(!state.BeginPreparation(now));
	CHECK(!state.BeginDispatch(now));
}

TEST_CASE("Papyrus queued and preparing deadlines fail closed at the exact boundary")
{
	const auto deadline = Clock::time_point{} + std::chrono::seconds(1);
	Lifecycle queued(json::object(), deadline);
	CHECK(!queued.BeginPreparation(deadline));
	Lifecycle preparing(json::object(), deadline);
	CHECK(preparing.BeginPreparation(deadline - std::chrono::milliseconds(1)));
	CHECK(!preparing.BeginDispatch(deadline));
	CHECK(!preparing.Snapshot()["dispatchEntered"].get<bool>());
}

TEST_CASE("Papyrus timeout during preparation fences subsequent VM entry")
{
	const auto now = Clock::time_point{};
	Lifecycle state(json::object(), now + std::chrono::seconds(1));
	CHECK(state.BeginPreparation(now));
	state.TimedOut();
	CHECK(!state.BeginDispatch(now));
	state.Failed();
	CHECK(state.Snapshot()["phase"] == "abandoned_before_dispatch");
}

TEST_CASE("Papyrus VM entry timeout is uncertain and never retry authority")
{
	const auto now = Clock::time_point{};
	const json request{ { "callId", "test-1" }, { "suppliedArgs", json::array({ 7, true }) } };
	Lifecycle state(request, now + std::chrono::seconds(1));
	CHECK(state.BeginPreparation(now));
	state.Resolved(json{ { "name", "DoThing" } }, nullptr);
	CHECK(state.BeginDispatch(now));
	state.TimedOut();
	const json timeoutReceipt = state.Snapshot();
	CHECK(timeoutReceipt["phase"] == "dispatching");
	CHECK(timeoutReceipt["mayStillExecute"].get<bool>());
	CHECK(timeoutReceipt["executionMayHaveOccurred"].get<bool>());
	CHECK(!timeoutReceipt["automaticRetryAllowed"].get<bool>());
	CHECK(timeoutReceipt["request"] == request);
	CHECK(timeoutReceipt["filledArgs"].empty());
	state.Accepted();
	CHECK(state.Snapshot()["phase"] == "dispatched");
	state.Completed();
	CHECK(!state.Snapshot()["mayStillExecute"].get<bool>());
	CHECK(timeoutReceipt["phase"] == "dispatching");
}

TEST_CASE("Papyrus synchronous callback completion survives dispatch acceptance")
{
	const auto now = Clock::time_point{};
	Lifecycle state(json::object(), now + std::chrono::seconds(1));
	CHECK(state.BeginPreparation(now));
	CHECK(state.BeginDispatch(now));
	state.Completed();
	state.Accepted();
	state.Failed();
	CHECK(state.Snapshot()["phase"] == "completed");
	CHECK(state.Snapshot()["dispatchEntered"].get<bool>());
}

TEST_CASE("Papyrus pre-entry rejection and VM-entry exception are distinct")
{
	const auto now = Clock::time_point{};
	Lifecycle rejected(json::object(), now + std::chrono::seconds(1));
	CHECK(rejected.BeginPreparation(now));
	rejected.Failed();
	CHECK(rejected.Snapshot()["phase"] == "rejected_before_dispatch");
	CHECK(!rejected.Snapshot()["mayStillExecute"].get<bool>());
	Lifecycle uncertain(json::object(), now + std::chrono::seconds(1));
	CHECK(uncertain.BeginPreparation(now));
	CHECK(uncertain.BeginDispatch(now));
	uncertain.Failed();
	CHECK(uncertain.Snapshot()["phase"] == "dispatch_uncertain");
	CHECK(uncertain.Snapshot()["mayStillExecute"].get<bool>());
}
