#pragma once
#ifndef MCP_LOGGER_H
#define MCP_LOGGER_H

// Server-only exact-pinned logger replacement, selected by the build-tree mirror.
// Neither vendored source nor process/CRT stderr is modified.
#include "McpLog.h"
namespace mcp
{
	enum class log_level { debug, info, warning, error };
	class logger
	{
	public:
		static logger& instance() { static logger value; return value; }
		void set_level(log_level level) noexcept { dvb::McpLog::SetLevel(static_cast<dvb::McpLogLevel>(level)); }
		template<class... Args> void debug(Args&&... args) noexcept { dvb::McpLog::Write(dvb::McpLogLevel::debug, std::forward<Args>(args)...); }
		template<class... Args> void info(Args&&... args) noexcept { dvb::McpLog::Write(dvb::McpLogLevel::info, std::forward<Args>(args)...); }
		template<class... Args> void warning(Args&&... args) noexcept { dvb::McpLog::Write(dvb::McpLogLevel::warn, std::forward<Args>(args)...); }
		template<class... Args> void error(Args&&... args) noexcept { dvb::McpLog::Write(dvb::McpLogLevel::error, std::forward<Args>(args)...); }
	};
	inline void set_log_level(log_level level) noexcept { logger::instance().set_level(level); }
}
#define LOG_DEBUG(...) mcp::logger::instance().debug(__VA_ARGS__)
#define LOG_INFO(...) mcp::logger::instance().info(__VA_ARGS__)
#define LOG_WARNING(...) mcp::logger::instance().warning(__VA_ARGS__)
#define LOG_ERROR(...) mcp::logger::instance().error(__VA_ARGS__)
#endif
