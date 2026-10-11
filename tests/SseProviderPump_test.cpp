#include "test_framework.h"
#include "SseProviderPump.h"

#include <stdexcept>
#include <vector>

TEST_CASE("SSE provider publishes endpoint once before waiting for events")
{
	dvb::SseProviderPump pump("event: endpoint\r\ndata: /message?session_id=owned\r\n\r\n");
	std::vector<std::string> writes;
	int waits = 0;
	auto write = [&](const std::string& value) { writes.push_back(value); return true; };
	auto wait = [&] { ++waits; return dvb::SseWaitOutcome::Event; };
	CHECK(pump.Pump([] { return true; }, write, wait));
	CHECK(writes.size() == 1);
	CHECK(waits == 0);
	CHECK(pump.Pump([] { return true; }, write, wait));
	CHECK(writes.size() == 1);
	CHECK(waits == 1);
}

TEST_CASE("SSE provider generates numbered heartbeats only on genuine timeout")
{
	dvb::SseProviderPump pump("endpoint");
	std::vector<std::string> writes;
	auto write = [&](const std::string& value) { writes.push_back(value); return true; };
	auto wait = [] { return dvb::SseWaitOutcome::Timeout; };
	CHECK(pump.Pump([] { return true; }, write, wait));
	CHECK(pump.Pump([] { return true; }, write, wait));
	CHECK(pump.Pump([] { return true; }, write, wait));
	CHECK(writes.size() == 3);
	CHECK(writes.at(1) == "event: heartbeat\r\ndata: 0\r\n\r\n");
	CHECK(writes.at(2) == "event: heartbeat\r\ndata: 1\r\n\r\n");
}

TEST_CASE("SSE provider refuses closed sessions before publication and after wait")
{
	dvb::SseProviderPump pump("endpoint");
	int writes = 0;
	bool ready = false;
	auto write = [&](const std::string&) { ++writes; return true; };
	CHECK(!pump.Pump([&] { return ready; }, write, [] { return dvb::SseWaitOutcome::Timeout; }));
	CHECK(writes == 0);
	ready = true;
	CHECK(pump.Pump([&] { return ready; }, write, [] { return dvb::SseWaitOutcome::Event; }));
	CHECK(!pump.Pump([&] { return ready; }, write, [&] { ready = false; return dvb::SseWaitOutcome::Timeout; }));
	CHECK(writes == 1);
}

TEST_CASE("SSE provider treats closed wait and failed write as terminal not heartbeat")
{
	dvb::SseProviderPump pump("endpoint");
	int writes = 0;
	auto write = [&](const std::string&) { ++writes; return true; };
	CHECK(pump.Pump([] { return true; }, write, [] { return dvb::SseWaitOutcome::Event; }));
	CHECK(!pump.Pump([] { return true; }, write, [] { return dvb::SseWaitOutcome::Closed; }));
	CHECK(writes == 1);
	CHECK(!pump.Pump([] { return true; }, [](const std::string&) { return false; }, [] { return dvb::SseWaitOutcome::Timeout; }));
}

TEST_CASE("SSE provider contains endpoint wait and heartbeat exceptions")
{
	dvb::SseProviderPump pump("endpoint");
	auto throwing_write = [](const std::string&) -> bool { throw std::runtime_error("sink"); };
	CHECK(!pump.Pump([] { return true; }, throwing_write, [] { return dvb::SseWaitOutcome::Event; }));
	CHECK(pump.Pump([] { return true; }, [](const std::string&) { return true; }, [] { return dvb::SseWaitOutcome::Event; }));
	CHECK(!pump.Pump([] { return true; }, [](const std::string&) { return true; }, []() -> dvb::SseWaitOutcome { throw std::runtime_error("wait"); }));
	CHECK(!pump.Pump([] { return true; }, throwing_write, [] { return dvb::SseWaitOutcome::Timeout; }));
}
