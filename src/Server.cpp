#include "Server.h"

#include "CalendarControl.h"

#include "McpAdapter.h"
#include "RestAdapter.h"
#include "PathUtf8.h"
#include "Version.h"

#include <RE/Skyrim.h>  // REL::Module::IsVR
#include <httplib.h>
#include <mcp_server.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <shellapi.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace
{
	std::atomic<int> g_boundPort{ 0 };

	// Process-wide registry pointer so dvb::RunTool can invoke tools from the in-game menu (a
	// separate render-thread TU) without a Server reference. Atomic: written on the SKSE thread
	// (Start/Stop), read on the render thread (RunTool). Set/cleared by Server::Start/Stop.
	std::atomic<dvb::ToolRegistry*> g_registry{ nullptr };

	// Throttle for ListRecordingsCached: a menu redraws every frame, and listing fully parses every
	// recording file, so cache the result and refresh at most ~once/second (or on invalidation).
	std::mutex                            g_recCacheMtx;
	dvb::json                             g_recCache;
	std::chrono::steady_clock::time_point g_recCacheAt{};
	bool                                  g_recCacheValid = false;


	// %LOCALAPPDATA% survives a VFS mod manager's Data virtualization (MO2's overwrite folder
	// swallows writes to Data/SKSE/Plugins/devbench); per-game subfolder since SE and VR run
	// concurrently on separate ports.
	std::optional<std::filesystem::path> ExternalStateDir()
	{
		wchar_t    buf[MAX_PATH]{};
		const auto n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buf, static_cast<DWORD>(std::size(buf)));
		if (n == 0 || n >= std::size(buf))
			return std::nullopt;
		return std::filesystem::path(buf) / "devbench" / (REL::Module::IsVR() ? "vr" : "se");
	}

	// Publish the actually-bound port so fixed-URL clients can discover a non-default
	// choice (when auto-iteration moved off the configured port).
	void WriteRuntimeInfo(int a_port)
	{
		const std::string payload = "{\"port\":" + std::to_string(a_port) + "}\n";
		std::error_code   ec;
		std::filesystem::create_directories("Data/SKSE/Plugins/devbench", ec);
		if (std::ofstream f("Data/SKSE/Plugins/devbench/runtime.json", std::ios::trunc); f)
			f << payload;
		if (auto dir = ExternalStateDir()) {
			std::filesystem::create_directories(*dir, ec);
			if (std::ofstream f(*dir / "runtime.json", std::ios::trunc); f)
				f << payload;
		}
	}

	// Mirror of GET /api/tools's mcp_bridge block onto disk, for a caller who found devbench
	// via its files rather than a live REST call (e.g. reading the install directory directly).
	void WriteBridgeInfo()
	{
		const std::string payload = dvb::BridgeDiscoveryInfo().dump(2) + "\n";
		if (std::ofstream f("Data/SKSE/Plugins/devbench/mcp-bridge.json", std::ios::trunc); f)
			f << payload;
		if (auto dir = ExternalStateDir()) {
			std::error_code ec;
			std::filesystem::create_directories(*dir, ec);
			if (std::ofstream f(*dir / "mcp-bridge.json", std::ios::trunc); f)
				f << payload;
		}
	}
}

namespace dvb
{
	Server::Server(std::string a_host, int a_port) :
		m_host(std::move(a_host)), m_port(a_port)
	{}

	Server::~Server()
	{
		Stop();
	}

	bool Server::Start()
	{
		std::lock_guard lifecycle(m_lifecycleMutex);
		if (const auto current = std::atomic_load(&m_mcp); current && current->is_running())
			return true;
		StopLocked();  // a failed retained owner must not masquerade as success
		constexpr int kMaxTries = 16;
		for (int offset = 0; offset < kMaxTries; ++offset) {
			// Avoid overflow/out-of-range ports before creating any transport owner.
			if (m_port < 1 || m_port > 65535 - offset)
				return false;
			const int chosen = m_port + offset;
			try {
				mcp::server::configuration cfg;
				cfg.host = m_host;
				cfg.port = chosen;
				cfg.name = "devbench";
				cfg.version = DEVBENCH_VERSION_STRING;
				// Immutable transport policy references the same retained PostLoad token.
				// Bind/queue readiness alone must not expose MCP sessions or the catalogue.
				cfg.request_admission = [ready = &m_ready]() noexcept { return ready->load(); };
				auto candidate = std::make_shared<mcp::server>(cfg);
				std::atomic_store(&m_mcp, candidate);
				candidate->set_server_info(cfg.name, cfg.version);
				candidate->set_capabilities(json{ { "tools", json{ { "listChanged", true } } }, { "logging", json::object() } });

				m_mcpAdapter = std::make_unique<McpAdapter>(m_registry, m_events, *candidate, m_ready);
				m_mcpAdapter->Wire();
				m_restAdapter = std::make_unique<RestAdapter>(m_registry, m_events, m_ready);
				auto* http = candidate->http();
				if (!http)
					throw std::runtime_error("DevBench HTTP owner unavailable");
				http->set_read_timeout(2, 0);
				m_restAdapter->Mount(*http);
				if (!candidate->start(false)) {
					const bool bindFailed = candidate->last_start_bind_failed();
					StopLocked();
					// Only an actual bind refusal advances the existing finite port
					// selection. Worker/route/readiness failures are not replayed.
					if (bindFailed) continue;
					return false;
				}
				// Publish only the actual held, prepared endpoint. Tool/menu
				// admission still requires PostLoad SetReady(true).
				g_boundPort.store(chosen);
				try { logs::info("devbench: server on {}:{} — listening (mcp + rest)", m_host, chosen); }
				catch (...) {} // optional reporting cannot turn success into a retry
				return true;
			} catch (...) {
				StopLocked();
				try { logs::error("devbench: server startup failed"); } catch (...) {}
				return false;
			}
		}
		return false;
	}

	void Server::Stop()
	{
		std::lock_guard lifecycle(m_lifecycleMutex);
		StopLocked();
	}

	void Server::StopLocked()
	{
		SetReady(false);
		CalendarControl::RequestStop();
		const auto transport = std::atomic_load(&m_mcp);
		if (transport)
			transport->stop();
		// Every HTTP/core handler is drained before destroying its callback
		// owners. Concurrent Running() retains only a read-only transport snapshot,
		// never this lifecycle lock or a new request/admission lease.
		m_restAdapter.reset();
		m_mcpAdapter.reset();
		std::atomic_store(&m_mcp, std::shared_ptr<mcp::server>{});
		g_boundPort.store(0);
		g_registry.store(nullptr);
	}

	bool Server::Running() const
	{
		const auto transport = std::atomic_load(&m_mcp);
		return transport && transport->is_running();
	}

	void Server::SetReady(bool a_ready) noexcept
	{
		if (a_ready) {
			g_registry.store(&m_registry);
			m_ready.store(true);
		} else {
			m_ready.store(false);
			g_registry.store(nullptr);
		}
	}

	void Server::ReportDiscovery()
	{
		const int chosen = BoundPort();
		WriteRuntimeInfo(chosen);
		WriteBridgeInfo();
		if (chosen != m_port)
			logs::info("devbench: configured port {} busy → bound {}", m_port, chosen);
	}

	int BoundPort()
	{
		return g_boundPort.load();
	}

	void SetProcessRegistry(ToolRegistry* a_registry)
	{
		g_registry.store(a_registry);
	}

	json RunTool(const std::string& a_name, const json& a_args)
	{
		// Load once: a concurrent Stop() must not null the pointer between the check and the call.
		ToolRegistry* registry = g_registry.load();
		if (!registry)
			return json{ { "error", "devbench server not running" }, { "code", 503 } };
		ToolContext ctx{ "ui" };
		ctx.internal = true;  // menu-driven; don't log each call
		const ToolResult r = registry->Invoke(a_name, a_args, ctx);
		return r.ok ? r.value : json{ { "error", r.errorMessage }, { "code", r.errorCode } };
	}

	json ListRecordingsCached()
	{
		using namespace std::chrono;
		std::lock_guard lock(g_recCacheMtx);
		if (const auto now = steady_clock::now(); !g_recCacheValid || now - g_recCacheAt > seconds(1)) {
			g_recCache = RunTool("recordings", json{ { "action", "list" } });
			g_recCacheAt = now;
			g_recCacheValid = true;
		}
		return g_recCache;
	}

	void InvalidateRecordingsCache()
	{
		std::lock_guard lock(g_recCacheMtx);
		g_recCacheValid = false;  // next ListRecordingsCached() re-parses
	}

	void OpenRecordingsFolder()
	{
		const std::string dir = ListRecordingsCached().value("dir", std::string{});
		if (dir.empty())
			return;
		std::error_code             ec;
		const std::filesystem::path abs = std::filesystem::absolute(dir, ec);
		::ShellExecuteW(nullptr, L"open", abs.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	}

	void OpenRecordingFile(const std::string& a_file)
	{
		const std::string dir = ListRecordingsCached().value("dir", std::string{});
		if (dir.empty() || a_file.empty())
			return;
		std::error_code             ec;
		const std::filesystem::path abs = std::filesystem::absolute(std::filesystem::path(dir) / a_file, ec);
		const std::wstring          args = L"/select,\"" + abs.wstring() + L"\"";
		::ShellExecuteW(nullptr, nullptr, L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
	}

	std::string ExecutableName()
	{
		char        path[MAX_PATH]{};
		const DWORD len = ::GetModuleFileNameA(nullptr, path, MAX_PATH);
		if (len == 0 || len == MAX_PATH)
			return {};
		return std::filesystem::path(path).filename().string();
	}

	json InstanceIdentity()
	{
		// pid/exe/vr are constant for the process lifetime — compute once. /api/health is
		// polled frequently, so avoid a GetModuleFileNameA + path + string alloc per call;
		// only the bound port (an atomic) is read live.
		static const int         pid = static_cast<int>(::GetCurrentProcessId());
		static const std::string exe = ExecutableName();
		static const bool        vr = REL::Module::IsVR();
		return json{ { "pid", pid }, { "port", BoundPort() }, { "exe", exe }, { "vr", vr } };
	}

	json BridgeDiscoveryInfo()
	{
		const bool        vr = REL::Module::IsVR();
		const std::string game = vr ? "vr" : "se";
		std::error_code   ec;
		const std::string exePath = PathUtf8(std::filesystem::absolute("Data/SKSE/Plugins/devbench/devbench-bridge.exe", ec));
		const std::string name = "devbench-" + game;
		json              result{
			{ "exePath", exePath },
			{ "args", json::array({ "--game", game }) },
			{ "mcpJsonSnippet",
				json{ { "mcpServers", json{ { name, json{ { "command", exePath }, { "args", json::array({ "--game", game }) } } } } } } },
			{ "installCommand", std::format("\"{}\" setup --game {}", exePath, game) },
			{ "note",
				"Add mcpJsonSnippet to your MCP client's config (e.g. .mcp.json), or run installCommand "
				"to print the same thing — devbench never edits your client config itself. Under a VFS mod "
				"manager (MO2, …), exePath is only reachable by processes launched through that manager's "
				"virtual filesystem -- an MCP client spawning the bridge directly needs a real, on-disk copy "
				"instead (extract devbench-bridge.exe from the release archive). runtime.json and this file "
				"are also mirrored to externalStateDir, which isn't virtualized, for exactly that reason." },
		};
		if (auto dir = ExternalStateDir())
			result["externalStateDir"] = PathUtf8(*dir);
		return result;
	}
}
