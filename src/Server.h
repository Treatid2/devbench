#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "EventBus.h"
#include "Json.h"
#include "ToolRegistry.h"

namespace mcp
{
	class server;
}

namespace dvb
{
	class McpAdapter;
	class RestAdapter;

	/// Owns the registry, event bus, and the cpp-mcp server, and mounts both the MCP
	/// and REST adapters on the one httplib instance (one localhost port). Register
	/// tools via Tools() before Start().
	class Server
	{
	public:
		explicit Server(std::string a_host = "127.0.0.1", int a_port = 8920);
		~Server();

		Server(const Server&) = delete;
		Server& operator=(const Server&) = delete;

		ToolRegistry& Tools() { return m_registry; }
		EventBus&     Events() { return m_events; }

		/// Owner-only lifecycle; returns after actual bind/queue readiness, not
		/// worker construction. Never call Start/Stop from a transport callback.
		bool Start();
		void Stop();
		bool Running() const;

		// Start builds the transport; PostLoad publishes only after required wiring.
		// False revokes new tool/REST admission without destroying escaped owners.
		void SetReady(bool a_ready) noexcept;
		bool Ready() const noexcept { return m_ready.load(); }
		// Optional discovery/reporting, outside the required startup transaction.
		void ReportDiscovery();

	private:
		void StopLocked();  // requires m_lifecycleMutex; drains before adapter reset
		std::mutex                   m_lifecycleMutex;
		std::string                  m_host;
		int                          m_port;
		ToolRegistry                 m_registry;
		EventBus                     m_events;
		std::atomic<bool>            m_ready{ false };
		// Atomic shared snapshots let Running() inspect without taking the
		// lifecycle mutex that Stop holds while joining transport callbacks.
		std::shared_ptr<mcp::server> m_mcp;
		std::unique_ptr<McpAdapter>  m_mcpAdapter;
		std::unique_ptr<RestAdapter> m_restAdapter;
	};

	/// The port actually bound by the (single, process-wide) Server instance — may differ
	/// from the configured port if it was busy, since Start() auto-iterates. 0 before a
	/// Server has started. Exposed so inspect{kind:"state"} can report it: with multiple
	/// game instances each on their own port, a caller can tell which one it's actually
	/// talking to instead of silently misattaching (devbench#16).
	int BoundPort();

	/// Basename of the running executable (SkyrimSE.exe / SkyrimVR.exe). Process-constant.
	std::string ExecutableName();

	/// { pid, port, exe, vr } identifying the instance that answered — computed off the main
	/// thread (pid/exe/vr cached once, port read live) so /api/health and inspect{kind:"state"}
	/// share one identity source and a caller can tell which game instance replied (devbench#16).
	json InstanceIdentity();

	/// { exePath, args, mcpJsonSnippet, installCommand } for the devbench-bridge companion
	/// (bridge/README.md) that survives this process restarting, unlike a client connected
	/// directly to this DLL's own /mcp endpoint. Surfaced at the top level of GET /api/tools,
	/// not behind a separate endpoint or inside a tool description.
	json BridgeDiscoveryInfo();

	/// Point RunTool at the process registry while a Server is up (set by Start, cleared by Stop),
	/// so the in-game menu — a separate render-thread TU with no Server reference — can invoke tools.
	void SetProcessRegistry(ToolRegistry* a_registry);

	/// Invoke a devbench tool by name from anywhere (e.g. the SMF menu). Returns the tool's value,
	/// or { error, code } on failure / when no Server is up. Only call fast, non-blocking tools from
	/// the render thread (recordings; record replay is async by default) — a RunAndWait-backed tool
	/// would stall the caller.
	json RunTool(const std::string& a_name, const json& a_args);

	/// The `recordings` list, cached and refreshed at most ~once/second. A menu redraws every frame,
	/// and the underlying list fully parses every recording file — calling it per frame would stall
	/// the render thread. Call InvalidateRecordingsCache() after a validate/delete to force a refresh.
	json ListRecordingsCached();
	void InvalidateRecordingsCache();

	/// Open the recordings directory in Explorer (menu "Open folder"); OpenRecordingFile selects one
	/// file. No-op if the directory is unknown. Not an ImGui call, so it lives in the main TU.
	void OpenRecordingsFolder();
	void OpenRecordingFile(const std::string& a_file);
}
