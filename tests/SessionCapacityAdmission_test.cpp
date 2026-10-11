#include "test_framework.h"
#include "SessionCapacityAdmission.h"

#include <atomic>
#include <future>
#include <map>
#include <string>
#include <thread>

TEST_CASE("session admission refuses the capacity boundary without replacing holders")
{
	std::map<std::string, int> sessions;
	std::mutex mutex;
	CHECK(dvb::AdmitSession(sessions, mutex, 1, "owned", 7));
	CHECK(!dvb::AdmitSession(sessions, mutex, 1, "new", 8));
	CHECK(sessions.size() == 1);
	CHECK(sessions.at("owned") == 7);
}

TEST_CASE("session admission preserves unlimited mode and rejects ID replacement")
{
	std::map<std::string, int> sessions;
	std::mutex mutex;
	CHECK(dvb::AdmitSession(sessions, mutex, 0, "one", 1));
	CHECK(dvb::AdmitSession(sessions, mutex, 0, "two", 2));
	CHECK_THROWS(dvb::AdmitSession(sessions, mutex, 0, "one", 99));
	CHECK(sessions.size() == 2);
	CHECK(sessions.at("one") == 1);
}

TEST_CASE("stopped session admission refuses even with capacity available")
{
	std::map<std::string, int> sessions;
	std::mutex mutex;
	bool open = false;
	bool ownerClosed = false;
	CHECK(!dvb::AdmitSession(sessions, mutex, 10, "new", 1, &open, &ownerClosed));
	CHECK(ownerClosed);
	CHECK(!dvb::AdmitSession(sessions, mutex, 0, "unlimited", 2, &open));
	CHECK(sessions.empty());
	{
		std::lock_guard lock(mutex);
		open = true;
	}
	CHECK(dvb::AdmitSession(sessions, mutex, 1, "owned", 1, &open, &ownerClosed));
	CHECK(!ownerClosed);
	CHECK(!dvb::AdmitSession(sessions, mutex, 1, "capacity", 2, &open, &ownerClosed));
	CHECK(!ownerClosed);
}

TEST_CASE("session admission after owner closure preserves existing holders")
{
	std::map<std::string, int> sessions;
	std::mutex mutex;
	bool open = true;
	CHECK(dvb::AdmitSession(sessions, mutex, 10, "owned", 1, &open));
	{
		std::lock_guard lock(mutex);
		open = false;
	}
	CHECK(!dvb::AdmitSession(sessions, mutex, 10, "new", 2, &open));
	CHECK(sessions.size() == 1);
	CHECK(sessions.at("owned") == 1);
}

TEST_CASE("concurrent session admission shares one remaining slot")
{
	std::map<std::string, int> sessions;
	std::mutex mutex;
	std::promise<void> ready;
	auto start = ready.get_future().share();
	std::atomic<int> admitted{ 0 };
	std::thread first([&] { start.wait(); if (dvb::AdmitSession(sessions, mutex, 1, "one", 1)) ++admitted; });
	// Joining the first worker on second-worker construction failure is required
	// even in this source fixture; no detached owner references.
	try {
		std::thread second([&] { start.wait(); if (dvb::AdmitSession(sessions, mutex, 1, "two", 2)) ++admitted; });
		ready.set_value();
		first.join();
		second.join();
	} catch (...) {
		try { ready.set_value(); } catch (...) {}
		if (first.joinable()) first.join();
		throw;
	}
	CHECK(admitted.load() == 1);
	CHECK(sessions.size() == 1);
}

TEST_CASE("session insertion rechecks the retained startup token under the owner lock")
{
	std::map<std::string, int> sessions;
	std::mutex mutex;
	bool open = true;
	bool ownerClosed = false;
	std::atomic<bool> committed{ false };
	const std::function<bool()> admission = [&]() noexcept { return committed.load(); };
	CHECK(!dvb::AdmitSession(sessions, mutex, 1, "premature", 1, &open, &ownerClosed, &admission));
	CHECK(ownerClosed);
	CHECK(sessions.empty());
	committed.store(true);
	CHECK(dvb::AdmitSession(sessions, mutex, 1, "owned", 2, &open, &ownerClosed, &admission));
	CHECK(!ownerClosed);
	committed.store(false);
	CHECK(!dvb::AdmitSession(sessions, mutex, 0, "quarantined", 3, &open, &ownerClosed, &admission));
	CHECK(ownerClosed);
	CHECK(sessions.size() == 1);
	CHECK(sessions.at("owned") == 2);
}

TEST_CASE("throwing startup admission fails closed without consuming a session slot")
{
	std::map<std::string, int> sessions;
	std::mutex mutex;
	bool ownerClosed = false;
	const std::function<bool()> admission = []() -> bool { throw std::runtime_error("injected admission failure"); };
	CHECK(!dvb::AdmitSession(sessions, mutex, 1, "premature", 1, nullptr, &ownerClosed, &admission));
	CHECK(ownerClosed);
	CHECK(sessions.empty());
}
