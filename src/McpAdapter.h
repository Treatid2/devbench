#pragma once

#include <cstdint>
#include <memory>
#include "CallbackLease.h"

namespace mcp
{
	class server;
}

namespace dvb
{
	class ToolRegistry;
	class EventBus;

	/// Reflects a ToolRegistry onto an mcp::server — registers each tool as an MCP
	/// tool and forwards EventBus events as MCP notifications. Contains no individual
	/// tool name. Call Wire() after tools are registered and before server.start().
	class McpAdapter
	{
	public:
		McpAdapter(ToolRegistry& a_registry, EventBus& a_events, mcp::server& a_server);

		// Revoke copied registry/bus callbacks before removing registrations. The
		// transport must outlive this retirement, including any admitted callback.
		~McpAdapter();

		void Wire();

	private:
		ToolRegistry& m_registry;
		EventBus&     m_events;
		mcp::server&  m_server;
		uint64_t      m_sub = 0;
		std::shared_ptr<CallbackLease<McpAdapter>> m_callbacks;
		// Initial wiring registers every existing tool; only AFTER that do (re)registrations mean a
		// real change worth a notifications/tools/list_changed broadcast. Set true at the end of Wire().
		bool m_announceChanges = false;
	};
}
