#include "test_framework.h"
#include "HttpTaskQueueOwnership.h"

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>

namespace
{
	using namespace std::chrono_literals;
}

TEST_CASE("production HTTP queue rolls back earlier threads on constructor failure")
{
	std::atomic<int> creations{ 0 };
	std::atomic<int> exits{ 0 };
	auto factory = [&](std::function<void()> fn) -> std::thread {
		if (++creations == 2)
			throw std::runtime_error("injected HTTP worker creation failure");
		return std::thread([&, fn = std::move(fn)] { fn(); ++exits; });
	};
	CHECK_THROWS_AS((dvb::HttpTaskQueueOwnership(2, 0, factory)), std::runtime_error);
	CHECK(creations.load() == 2);
	CHECK(exits.load() == 1);
}

TEST_CASE("production HTTP queue preserves waiting job limit and refuses after drain")
{
	std::promise<void> entered;
	auto started = entered.get_future();
	std::promise<void> release;
	auto permission = release.get_future().share();
	std::atomic<int> queuedCalls{ 0 };
	dvb::HttpTaskQueueOwnership queue(1, 1);
	const bool first = queue.enqueue([&entered, permission] {
		entered.set_value();
		permission.wait_for(5s);  // bounded fixture wait, not production cancellation
	});
	const auto startedStatus = started.wait_for(2s);
	bool admitted = false;
	bool refused = false;
	if (startedStatus == std::future_status::ready) {
		admitted = queue.enqueue([&] { ++queuedCalls; });
		refused = !queue.enqueue([&] { ++queuedCalls; });
	}
	release.set_value();
	queue.shutdown();
	CHECK(first);
	CHECK(startedStatus == std::future_status::ready);
	CHECK(admitted);
	CHECK(refused);
	CHECK(queuedCalls.load() == 1);
	CHECK(!queue.enqueue([] {}));
	CHECK_NOTHROW(queue.shutdown());
}

TEST_CASE("production HTTP queue owns destructor drain and contains handler exceptions")
{
	std::atomic<int> calls{ 0 };
	{
		dvb::HttpTaskQueueOwnership queue(1);
		CHECK(queue.enqueue([&] { ++calls; }));
	}  // the actual last-declared owned pool joins before queue state destruction
	CHECK(calls.load() == 1);
	dvb::HttpTaskQueueOwnership queue(1);
	CHECK(queue.enqueue([] { throw std::runtime_error("injected HTTP handler failure"); }));
	CHECK(queue.enqueue([&] { ++calls; }));
	queue.shutdown();
	CHECK(queue.failed_requests() == 1);
	CHECK(calls.load() == 2);
	CHECK(!queue.enqueue(std::function<void()>{}));
}
