#include "test_framework.h"
#include "HttpListenerStartup.h"

#include <stdexcept>
#include <thread>

namespace
{
	const auto Now = [] { return std::chrono::steady_clock::now(); };
}

TEST_CASE("listener startup needs explicit queue readiness, not a running flag")
{
	dvb::HttpListenerStartup startup;
	CHECK(!startup.Ready());
	CHECK(!startup.PublishReady(startup.CurrentToken()));
	const auto token = startup.Begin();
	CHECK(!startup.Ready());
	CHECK(!startup.WaitUntil(token, Now()));
	CHECK(startup.PublishReady(token));
	CHECK(startup.WaitUntil(token, Now()));
	CHECK_THROWS(startup.Begin());
}

TEST_CASE("queue construction exception publishes startup failure")
{
	dvb::HttpListenerStartup startup;
	const auto token = startup.Begin();
	const auto construct = [&] {
		dvb::HttpListenerStartupExit exit(startup, token);
		throw std::runtime_error("queue constructor failure");
	};
	CHECK_THROWS(construct());
	CHECK(!startup.WaitUntil(token, Now()));
	CHECK(!startup.PublishReady(token));
}

TEST_CASE("stop before queue readiness cannot be overwritten by late publication")
{
	dvb::HttpListenerStartup startup;
	const auto token = startup.Begin();
	startup.Cancel();
	CHECK(!startup.PublishReady(token));
	CHECK(!startup.WaitUntil(token, Now()));
}

TEST_CASE("listener exit after readiness revokes the ready result")
{
	dvb::HttpListenerStartup startup;
	const auto token = startup.Begin();
	{
		dvb::HttpListenerStartupExit exit(startup, token);
		CHECK(startup.PublishReady(token));
		CHECK(startup.Ready());
	}
	CHECK(!startup.Ready());
	CHECK(!startup.WaitUntil(token, Now()));
}

TEST_CASE("stale listener signals cannot change a new startup epoch")
{
	dvb::HttpListenerStartup startup;
	const auto oldToken = startup.Begin();
	startup.Finish(oldToken);
	const auto newToken = startup.Begin();
	CHECK(newToken != oldToken);
	CHECK(!startup.PublishReady(oldToken));
	CHECK(startup.PublishReady(newToken));
	startup.Finish(oldToken);
	CHECK(startup.Ready());
	CHECK(!startup.WaitUntil(oldToken, Now()));
	startup.Finish(newToken);
}

TEST_CASE("listener startup waiter receives queue-ready publication")
{
	dvb::HttpListenerStartup startup;
	const auto token = startup.Begin();
	bool published = false;
	std::thread worker([&] { published = startup.PublishReady(token); });
	const bool ready = startup.WaitUntil(token, Now() + std::chrono::seconds(2));
	worker.join();
	CHECK(published);
	CHECK(ready);
	startup.Finish(token);
}
