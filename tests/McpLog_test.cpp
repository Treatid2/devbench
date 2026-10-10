#include "test_framework.h"
#include "PinnedMcpLogger.h"

#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
	std::mutex captureMutex;
	std::vector<std::pair<dvb::McpLogLevel, std::string>> captured;
	void Capture(dvb::McpLogLevel level, std::string_view text)
	{
		std::lock_guard lock(captureMutex);
		captured.emplace_back(level, text);
	}
	void FailedSink(dvb::McpLogLevel, std::string_view) { throw std::runtime_error("sink unavailable"); }
	struct Fixture
	{
		Fixture() { captured.clear(); dvb::McpLog::SetLevel(dvb::McpLogLevel::warn); dvb::McpLog::SetSink(Capture); }
		~Fixture() { dvb::McpLog::SetSink(nullptr); dvb::McpLog::SetLevel(dvb::McpLogLevel::warn); }
	};
	struct FormatProbe { int* count; bool fail{ false }; };
	std::ostream& operator<<(std::ostream& stream, const FormatProbe& probe)
	{
		++*probe.count;
		if (probe.fail) throw std::runtime_error("format failed");
		return stream << "probe";
	}
}

TEST_CASE("MCP levels parse independently and unknown values fall back to warn")
{
	dvb::McpLogLevel level;
	for (const auto* name : { "debug", "info", "warn", "error", "off" }) CHECK(dvb::ParseMcpLogLevel(name, level));
	for (const auto* name : { "trace", "INFO", "", "warning" }) {
		CHECK(!dvb::ParseMcpLogLevel(name, level));
		CHECK(level == dvb::McpLogLevel::warn);
	}
}

TEST_CASE("pinned MCP facade retains severity and filters before formatting")
{
	Fixture fixture;
	int formatted = 0;
	LOG_DEBUG(FormatProbe{ &formatted });
	LOG_INFO(FormatProbe{ &formatted });
	CHECK(formatted == 0);
	LOG_WARNING("count=", 10, " limit=", 10);
	LOG_ERROR("cleanup failed");
	CHECK(captured.size() == 2);
	CHECK(captured.at(0).first == dvb::McpLogLevel::warn);
	CHECK(captured.at(0).second == "count=10 limit=10");
	CHECK(captured.at(1).first == dvb::McpLogLevel::error);
	mcp::set_log_level(mcp::log_level::debug);
	LOG_DEBUG(FormatProbe{ &formatted });
	CHECK(formatted == 1);
	dvb::McpLog::SetLevel(dvb::McpLogLevel::off);
	LOG_ERROR(FormatProbe{ &formatted });
	CHECK(formatted == 1);
}

TEST_CASE("MCP output redacts session handles and cannot inject extra lines or ANSI")
{
	Fixture fixture;
	LOG_WARNING("session=12345678-1234-abcd-ABCD-123456789abc\r\n\x1b[31m");
	const auto& text = captured.at(0).second;
	CHECK(text.find("12345678") == std::string::npos);
	CHECK(text.find(std::string(36, '*')) != std::string::npos);
	CHECK(text.find('\r') == std::string::npos);
	CHECK(text.find('\n') == std::string::npos);
	CHECK(text.find('\x1b') == std::string::npos);
}

TEST_CASE("MCP log messages are capped including truncated session handle prefixes")
{
	Fixture fixture;
	const std::string prefix(dvb::McpLog::MaxMessageBytes - 20, 'x');
	LOG_WARNING(prefix, "12345678-1234-abcd-ABCD-123456789abc", std::string(10000, 'z'));
	const auto& text = captured.at(0).second;
	CHECK(text.size() == dvb::McpLog::MaxMessageBytes);
	CHECK(text.ends_with(" [truncated]"));
	CHECK(text.find("12345678") == std::string::npos);
}

TEST_CASE("MCP sink and formatting failures never escape and remain counted")
{
	Fixture fixture;
	auto before = dvb::McpLog::Failures();
	dvb::McpLog::SetSink(FailedSink);
	LOG_ERROR("disk unavailable");
	CHECK(dvb::McpLog::Failures() == before + 1);
	dvb::McpLog::SetSink(Capture);
	int formatted = 0;
	LOG_ERROR(FormatProbe{ &formatted, true });
	CHECK(formatted == 1);
	CHECK(dvb::McpLog::Failures() == before + 2);
	CHECK(captured.empty());
	dvb::McpLog::SetSink(nullptr);
	LOG_ERROR("no retained sink");
	CHECK(dvb::McpLog::Failures() == before + 3);
}

TEST_CASE("MCP messages from concurrent producers remain separate")
{
	Fixture fixture;
	std::thread first([] { for (int i = 0; i < 20; ++i) LOG_WARNING("first ", i); });
	try {
		std::thread second([] { for (int i = 0; i < 20; ++i) LOG_WARNING("second ", i); });
		first.join();
		second.join();
	} catch (...) { if (first.joinable()) first.join(); throw; }
	CHECK(captured.size() == 40);
	for (const auto& entry : captured) CHECK(entry.second.starts_with("first ") || entry.second.starts_with("second "));
}
