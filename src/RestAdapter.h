#pragma once

#include <cstdint>
#include <memory>
#include "CallbackLease.h"
#include <mutex>
#include <optional>
#include <string>

namespace httplib
{
	class Server;
}

namespace dvb
{
	class ToolRegistry;
	class EventBus;

	/// Mounts a plain-HTTP REST facade (/api/*) onto an existing httplib server,
	/// sharing the MCP port. Generic — reflects the registry, no tool names. Lets
	/// non-MCP clients (curl, scripts, CI) reach the same tools without the JSON-RPC
	/// handshake. Mount before server.start().
	///
	///   GET  /api/tools            → registry descriptors (discovery + docs)
	///   POST /api/tool/<name>      → invoke; body is the arguments object
	///   GET  /api/health           → {ok, lastLifecycle, frame, lastTaskFrame, pendingTasks,
	///                                 pid, port, exe, vr} — off-thread liveness + identity
	///   GET  /api/events?since=N   → recent event ring (poll)
	class RestAdapter
	{
	public:
		RestAdapter(ToolRegistry& a_registry, EventBus& a_events);
		// Revoke copied bus/HTTP callbacks and drain admitted bodies. The shared
		// lease survives in routes; they return 503 after retirement without this.
		~RestAdapter();

		void Mount(httplib::Server& a_http);

	private:
		ToolRegistry&              m_registry;
		EventBus&                  m_events;
		uint64_t                   m_lifecycleSub = 0;
		std::shared_ptr<CallbackLease<RestAdapter, CallbackMode::Concurrent>> m_callbacks;
		mutable std::mutex         m_lifecycleMtx;
		std::optional<std::string> m_lastLifecycle;
	};
}
