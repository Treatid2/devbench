#include "test_framework.h"
#include "SessionInitializationState.h"

#include <atomic>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <thread>

namespace
{
	struct InitializationDispatcher
	{
		std::atomic<bool> closed{ false };
		bool is_closed() const noexcept { return closed.load(); }
	};
	using Dispatchers = std::map<std::string, std::shared_ptr<InitializationDispatcher>>;
	using Initialized = std::map<std::string, bool>;
}

TEST_CASE("initialization refuses missing and null dispatchers without creating flags")
{
	Dispatchers dispatchers{ { "null", nullptr } };
	Initialized initialized;
	std::mutex mutex;
	CHECK(!dvb::SetRegisteredSessionInitialized(dispatchers, initialized, mutex, "missing", true));
	CHECK(!dvb::SetRegisteredSessionInitialized(dispatchers, initialized, mutex, "null", true));
	CHECK(initialized.empty());
	CHECK(dispatchers.size() == 1);
}

TEST_CASE("initialization updates only a registered open dispatcher")
{
	auto dispatcher = std::make_shared<InitializationDispatcher>();
	Dispatchers dispatchers{ { "owned", dispatcher } };
	Initialized initialized{ { "other", true } };
	std::mutex mutex;
	CHECK(dvb::SetRegisteredSessionInitialized(dispatchers, initialized, mutex, "owned", false));
	CHECK(!initialized.at("owned"));
	CHECK(dvb::SetRegisteredSessionInitialized(dispatchers, initialized, mutex, "owned", true));
	CHECK(initialized.at("owned"));
	CHECK(initialized.at("other"));
	CHECK(dispatchers.at("owned") == dispatcher);
}

TEST_CASE("late initialization refuses a closed dispatcher and a retired registration")
{
	auto dispatcher = std::make_shared<InitializationDispatcher>();
	Dispatchers dispatchers{ { "owned", dispatcher } };
	Initialized initialized;
	std::mutex mutex;
	dispatcher->closed.store(true);
	CHECK(!dvb::SetRegisteredSessionInitialized(dispatchers, initialized, mutex, "owned", true));
	CHECK(initialized.empty());
	{
		std::lock_guard lock(mutex);
		dispatchers.erase("owned");
		initialized.erase("owned");
	}
	CHECK(!dvb::SetRegisteredSessionInitialized(dispatchers, initialized, mutex, "owned", true));
	CHECK(initialized.empty());
}

TEST_CASE("initialization racing retirement leaves no orphan initialization flag")
{
	Dispatchers dispatchers{ { "owned", std::make_shared<InitializationDispatcher>() } };
	Initialized initialized;
	std::mutex mutex;
	std::promise<void> ready;
	auto start = ready.get_future().share();
	std::thread notifier([&] {
		start.wait();
		(void)dvb::SetRegisteredSessionInitialized(dispatchers, initialized, mutex, "owned", true);
	});
	std::thread retirement;
	try {
		retirement = std::thread([&] {
			start.wait();
			std::lock_guard lock(mutex);
			dispatchers.erase("owned");
			initialized.erase("owned");
		});
		ready.set_value();
		notifier.join();
		retirement.join();
	} catch (...) {
		try { ready.set_value(); } catch (...) {}
		if (notifier.joinable()) notifier.join();
		if (retirement.joinable()) retirement.join();
		throw;
	}
	CHECK(dispatchers.empty());
	CHECK(initialized.empty());
}
