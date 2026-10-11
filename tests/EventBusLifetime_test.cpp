#include "test_framework.h"
#include "EventBus.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>

namespace
{
	using namespace std::chrono_literals;

	struct CopyFailureState
	{
		std::atomic<bool> failCopies{ false };
		std::atomic<int> calls{ 0 };
	};

	// std::function copies this actual production subscriber during the bus snapshot.
	struct ThrowingCopySubscriber
	{
		std::shared_ptr<CopyFailureState> state;
		explicit ThrowingCopySubscriber(std::shared_ptr<CopyFailureState> a_state) : state(std::move(a_state)) {}
		ThrowingCopySubscriber(ThrowingCopySubscriber&&) noexcept = default;
		ThrowingCopySubscriber(const ThrowingCopySubscriber& a_other) : state(a_other.state)
		{
			if (state->failCopies.load())
				throw std::runtime_error("injected subscriber snapshot failure");
		}
		void operator()(const dvb::EventBus::Event&) const { ++state->calls; }
	};
}

TEST_CASE("actual EventBus isolates subscriber exceptions across subscribers and events")
{
	dvb::EventBus bus;
	std::promise<void> secondEvent;
	auto secondObserved = secondEvent.get_future();
	std::atomic<int> delivered{ 0 };
	const auto throwing = bus.Subscribe([](const auto&) { throw std::runtime_error("injected delivery failure"); });
	const auto observer = bus.Subscribe([&](const auto& ev) {
		++delivered;
		if (ev.seq == 2)
			secondEvent.set_value();
	});
	bus.Publish("first", dvb::json::object());
	bus.Publish("second", dvb::json::object());
	const auto result = secondObserved.wait_for(2s);
	bus.Unsubscribe(throwing);
	bus.Unsubscribe(observer);
	CHECK(result == std::future_status::ready);
	CHECK(delivered.load() == 2);
	CHECK(bus.SubscriberFailures() == 2);
	CHECK(!bus.DeliveryStopped());
}

TEST_CASE("actual EventBus unsubscribe drains an admitted callback and prevents later invocation")
{
	dvb::EventBus bus;
	std::promise<void> entered;
	auto enteredObserved = entered.get_future();
	std::promise<void> release;
	auto permission = release.get_future().share();
	std::atomic<int> calls{ 0 };
	const auto owner = bus.Subscribe([&](const auto&) {
		entered.set_value();
		// Bounded even if an assertion/fixture fails before its normal release.
		permission.wait_for(2s);
		++calls;
	});
	bus.Publish("admitted", dvb::json::object());
	const auto admission = enteredObserved.wait_for(2s);
	std::promise<void> retirementStarted;
	auto retirementObserved = retirementStarted.get_future();
	auto retirement = std::async(std::launch::async, [&] {
		retirementStarted.set_value();
		bus.Unsubscribe(owner);
	});
	const auto started = retirementObserved.wait_for(2s);
	const auto whileAdmitted = retirement.wait_for(20ms);
	release.set_value();
	retirement.get();
	// All test-owned waiters are released before reporting failed assertions.
	CHECK(admission == std::future_status::ready);
	CHECK(started == std::future_status::ready);
	CHECK(whileAdmitted == std::future_status::timeout);

	std::promise<void> nextEvent;
	auto nextObserved = nextEvent.get_future();
	const auto observer = bus.Subscribe([&](const auto& ev) {
		if (ev.seq == 2)
			nextEvent.set_value();
	});
	bus.Publish("after_retirement", dvb::json::object());
	const auto next = nextObserved.wait_for(2s);
	bus.Unsubscribe(observer);
	CHECK(next == std::future_status::ready);
	CHECK(calls.load() == 1);
	CHECK(bus.SubscriberFailures() == 0);
	CHECK(!bus.DeliveryStopped());
}

TEST_CASE("actual EventBus snapshot failure stops delivery without terminating its thread owner")
{
	dvb::EventBus bus;
	auto state = std::make_shared<CopyFailureState>();
	const auto owner = bus.Subscribe(ThrowingCopySubscriber(state));
	state->failCopies.store(true);
	bus.Publish("snapshot_failure", dvb::json::object());
	const auto deadline = std::chrono::steady_clock::now() + 2s;
	while (!bus.DeliveryStopped() && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(1ms);
	bus.Unsubscribe(owner);
	CHECK(bus.DeliveryStopped());
	CHECK(state->calls.load() == 0);
	CHECK(bus.SubscriberFailures() == 0);  // copy failure is not a callback invocation
	bus.Publish("ring_after_delivery_failure", dvb::json::object());
	CHECK(bus.HeadSeq() == 2);
	CHECK(bus.Since(0).size() == 2);
}
