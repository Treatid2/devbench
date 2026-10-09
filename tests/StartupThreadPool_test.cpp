#include "test_framework.h"
#include "PinnedMcpThreadPool.h"

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>

namespace
{
	using namespace std::chrono_literals;
}

TEST_CASE("production pinned pool overlay joins earlier workers when constructor creation fails")
{
	std::atomic<int> creations{ 0 };
	std::atomic<int> exits{ 0 };
	auto factory = [&](std::function<void()> fn) -> std::thread {
		if (++creations == 2)
			throw std::runtime_error("injected second worker creation failure");
		return std::thread([&, fn = std::move(fn)] {
			fn();
			++exits;
		});
	};
	CHECK_THROWS_AS((mcp::thread_pool(2, 2, 60000, factory)), std::runtime_error);
	// The throw cannot escape construction until the first actual thread exits.
	CHECK(creations.load() == 2);
	CHECK(exits.load() == 1);
}

TEST_CASE("production pinned pool overlay does not accept a task after failed required growth")
{
	std::promise<void> releaseCore;
	auto corePermission = releaseCore.get_future().share();
	std::atomic<bool> failCreation{ false };
	std::atomic<int> calls{ 0 };
	auto factory = [&](std::function<void()> fn) -> std::thread {
		if (failCreation.load())
			throw std::runtime_error("injected growth failure");
		return std::thread([fn = std::move(fn), corePermission] {
			// Keep the core out of its idle wait, forcing the real enqueue growth
			// path deterministically. The fixture releases even on failed assertions.
			corePermission.wait_for(2s);
			fn();
		});
	};
	mcp::thread_pool pool(1, 2, 60000, factory);
	failCreation.store(true);
	CHECK_THROWS_AS(pool.enqueue([&] { ++calls; }), std::runtime_error);
	releaseCore.set_value();
	pool.shutdown();
	CHECK(calls.load() == 0);  // no orphaned accepted task behind the thrown enqueue
	CHECK(pool.get_active_threads() == 0);
}

TEST_CASE("production pinned pool overlay drains futures rejects stopped admission and restarts explicitly")
{
	mcp::thread_pool pool(2, 2);
	auto result = pool.enqueue([] { return 42; });
	auto failure = pool.enqueue([]() -> int { throw std::runtime_error("injected application failure"); });
	pool.shutdown();
	const auto resultReady = result.wait_for(2s);
	const auto failureReady = failure.wait_for(2s);
	CHECK(resultReady == std::future_status::ready);
	if (resultReady == std::future_status::ready)
		CHECK(result.get() == 42);
	CHECK(failureReady == std::future_status::ready);
	if (failureReady == std::future_status::ready)
		CHECK_THROWS_AS(failure.get(), std::runtime_error);
	CHECK(pool.worker_failures() == 0);  // packaged_task handles application failure
	CHECK(pool.get_active_threads() == 0);
	CHECK_THROWS_AS(pool.enqueue([] {}), std::runtime_error);
	CHECK_NOTHROW(pool.shutdown());
	pool.restart();
	auto after = pool.enqueue([] { return 7; });
	pool.shutdown();
	const auto afterReady = after.wait_for(2s);
	CHECK(afterReady == std::future_status::ready);
	if (afterReady == std::future_status::ready)
		CHECK(after.get() == 7);
	CHECK(pool.get_active_threads() == 0);
}

TEST_CASE("production pinned pool overlay closes admission before waiting for admitted tasks")
{
	std::promise<void> entered;
	std::promise<void> release;
	auto permission = release.get_future().share();
	auto started = entered.get_future();
	mcp::thread_pool pool(1, 1);
	auto held = pool.enqueue([&entered, permission] {
		entered.set_value();
		// Bound the fixture's own wait even if an assertion prevents its intended
		// release. This is not a production task-cancellation mechanism.
		permission.wait_for(5s);
		return 17;
	});
	const auto startedStatus = started.wait_for(2s);
	auto queued = pool.enqueue([] { return 23; });
	pool.close_admission();
	pool.close_admission();  // no join or duplicate admission transition
	CHECK_THROWS_AS(pool.enqueue([] {}), std::runtime_error);
	if (startedStatus == std::future_status::ready) {
		CHECK(held.wait_for(0ms) == std::future_status::timeout);
		CHECK(queued.wait_for(0ms) == std::future_status::timeout);
		CHECK(pool.get_active_threads() == 1);
	}
	release.set_value();
	pool.shutdown();
	CHECK(startedStatus == std::future_status::ready);
	const auto heldStatus = held.wait_for(2s);
	const auto queuedStatus = queued.wait_for(2s);
	CHECK(heldStatus == std::future_status::ready);
	CHECK(queuedStatus == std::future_status::ready);
	if (heldStatus == std::future_status::ready)
		CHECK(held.get() == 17);
	if (queuedStatus == std::future_status::ready)
		CHECK(queued.get() == 23);
	CHECK(pool.get_active_threads() == 0);
}
