#include "Autorun.h"
#include "CameraOrbit.h"
#include "Capture.h"
#include "Config.h"
#include "ConsoleHook.h"
#include "ConsoleLogCapture.h"
#include "FreeCamera.h"
#include "GameEvents.h"
#include "GameState.h"
#include "HostApi.h"
#include "InputHotkeys.h"
#include "KeyboardInput.h"
#include "PostLoadStartup.h"
#include "Recording.h"
#include "RecordingsMenu.h"
#include "Server.h"
#include "StallWatchdog.h"
#include "Tools.h"
#include "VRInput.h"
#include "Version.h"

#include <cstring>
#include <memory>
#include <stdexcept>
#include <spdlog/sinks/basic_file_sink.h>

namespace
{
	// Process-lifetime: DLL detach runs after Windows terminates the server's workers,
	// so destroying it there would wait for threads that can no longer finish.
	dvb::Server* g_server = nullptr;
	dvb::Config  g_config;  // captured at kPostLoad; used at kInputLoaded
	dvb::PostLoadStartup g_startup;

	void InitLogging()
	{
		auto path = SKSE::log::log_directory();
		if (!path)
			return;
		*path /= std::format("{}.log", SKSE::PluginDeclaration::GetSingleton()->GetName());

		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S.%e] [%^%L%$] %v");
	}

	// Apply the config's log level (default info). Lowering to debug/trace surfaces
	// per-tool-invocation logging for diagnosing client/agent interactions.
	void ApplyLogLevel(const std::string& a_level)
	{
		const auto lvl = spdlog::level::from_str(a_level);  // unknown string → 'off'; guard that
		const auto resolved = (lvl == spdlog::level::off && a_level != "off") ? spdlog::level::info : lvl;
		if (resolved == spdlog::level::off && a_level != "off")
			logs::warn("devbench: unknown logLevel '{}' — keeping info", a_level);
		spdlog::set_level(resolved);
		spdlog::flush_on(resolved);
	}

	// Listener for messages from ANY plugin (registered with a nullptr sender). The
	// default OnMessage listener only receives SKSE-sender messages, so a consumer mod's
	// interface request would never reach us without this. Only acts on the request.
	void OnInterfaceMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (g_startup.Ready() && g_server && g_server->Ready())
			dvb::HostApi::OnInterfaceRequest(a_msg);
	}

	struct PostLoadOperations
	{
		dvb::Config config;

		bool Configure()
		{
			config = dvb::LoadConfig();
			ApplyLogLevel(config.logLevel);
			return config.enabled;
		}

		void Construct()
		{
			g_config = config;
			// Retain this process-lifetime owner BEFORE any registry/bus address
			// escapes. A later failure never deletes it or repeats irreversible work.
			g_server = new dvb::Server("127.0.0.1", config.port);
		}

		void Prepare()
		{
			g_server->Events().SetFrameProvider(&dvb::game::CurrentFrame);
			dvb::RegisterCoreTools(g_server->Tools(), g_server->Events());
			dvb::Recording::SetLoadSettleMs(config.loadSettleMs);
			dvb::Recording::SetDefaultIntervalMs(config.recordIntervalMs);
			dvb::Recording::SetCoupling(config.couplingAnchorMs, config.couplingCellMs, config.cleanTransition, config.cleanTransitionCell);
			dvb::Recording::SetCaptureDefaults(config.captureSettleMs);
			dvb::Capture::SetEvents(&g_server->Events());
			dvb::Capture::SetDefaults(config);
			dvb::ArmAutoRun(g_server->Tools(), config.autoRunPath, config.autoRunRestoreScene);
			dvb::HostApi::Init(g_server->Tools(), g_server->Events());
		}

		bool Listen() { return g_server->Start(); }

		void Publish()
		{
			// Required listener stage must succeed BEFORE irreversible hook/sink
			// installs. No hook rollback API is presumed if this phase later fails.
			dvb::InstallGameEvents(g_server->Events());
			dvb::StallWatchdog::Start(g_server->Events(), config.stallWatchdogMs);
			dvb::ConsoleHook::Install(g_server->Events());
			dvb::ConsoleLogCapture::InstallPrintHook();
			dvb::CameraOrbit::Install();
			auto* messaging = SKSE::GetMessagingInterface();
			if (!messaging || !messaging->RegisterListener(nullptr, OnInterfaceMessage))
				throw std::runtime_error("devbench interface listener registration failed");
			g_server->SetReady(true);
		}

		void Quarantine() noexcept
		{
			if (g_server)
				g_server->SetReady(false);
			// Do NOT run pinned stop/destruction here until its cleanup is proven.
			// This is denied admission and retained ownership, not stopped transport.
		}

		void Report(dvb::PostLoadStartup::Outcome a_outcome, dvb::PostLoadStartup::Phase a_phase, std::exception_ptr a_error)
		{
			if (a_outcome == dvb::PostLoadStartup::Outcome::Disabled) {
				logs::info("devbench: server disabled via config; not starting");
			} else if (a_outcome == dvb::PostLoadStartup::Outcome::Ready) {
				g_server->ReportDiscovery();
			} else {
				try {
					if (a_error)
						std::rethrow_exception(a_error);
				} catch (const std::exception& e) {
					logs::error("devbench: startup phase {} failed: {}; owner retained, dependent stages disabled",
						static_cast<int>(a_phase), e.what());
					return;
				} catch (...) {}
				logs::error("devbench: startup phase {} failed; owner retained, dependent stages disabled", static_cast<int>(a_phase));
			}
		}
	};

	void StartPostLoad() noexcept
	{
		// Even construction of the operation/config object belongs to the SKSE
		// boundary; no diagnostic failure may escape to another plugin's listener.
		try {
			PostLoadOperations operations;
			g_startup.Run(operations);
		} catch (...) {
			try { logs::error("devbench: could not construct startup operations"); } catch (...) {}
		}
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg)
			return;
		if (a_msg->type == SKSE::MessagingInterface::kPostLoad) {
			StartPostLoad();
			return;
		}
		if (!g_startup.Ready() || !g_server || !g_server->Ready())
			return;
		if (a_msg->type == SKSE::MessagingInterface::kPreLoadGame || a_msg->type == SKSE::MessagingInterface::kNewGame)
			dvb::CameraOrbit::Stop();  // an orbit never carries into another game
		if (a_msg->type == SKSE::MessagingInterface::kPreLoadGame)
			dvb::FreeCamera::BeginLoad();
		else if (a_msg->type == SKSE::MessagingInterface::kNewGame || a_msg->type == SKSE::MessagingInterface::kPostLoadGame)
			dvb::FreeCamera::EndLoad();
		// Register input hotkeys at kInputLoaded — BSInputDeviceManager is null at kPostLoad,
		// so registering then silently no-ops. kInputLoaded fires once the input subsystem is up.
		if (a_msg->type == SKSE::MessagingInterface::kInputLoaded && g_server) {
			dvb::MarkKeyboardInputReady();
			dvb::MarkVRInputReady(g_server->Events());
			dvb::InstallInputHotkeys(g_server->Tools(), g_config);
		}

		// Register the optional in-game menus at kDataLoaded (the frameworks are up by then). Each
		// is inert if its framework isn't installed; both drive devbench via dvb::RunTool, so they
		// need committed server readiness. SMF and FUCK are independent — host either, both, or neither.
		if (a_msg->type == SKSE::MessagingInterface::kDataLoaded && g_server) {
			dvb::UI::Register();
			dvb::UI::RegisterFuck();
		}

		if (g_server) {
			// Never carry a synthetic held key across a scene reset. This callback is on Skyrim's
			// main thread, so the keyboard layer hands cleanup to a worker and returns immediately.
			if (a_msg->type == SKSE::MessagingInterface::kPreLoadGame) {
				dvb::ReleaseKeyboardInputForLifecycle("preLoadGame");
				dvb::ReleaseVRInputForLifecycle("preLoadGame");
			} else if (a_msg->type == SKSE::MessagingInterface::kNewGame) {
				dvb::ReleaseKeyboardInputForLifecycle("newGame");
				dvb::ReleaseVRInputForLifecycle("newGame");
			}

			// Publish lifecycle events (dataLoaded and later load/save/new-game).
			dvb::OnSKSEMessage(a_msg->type);
			// Remember the save the player loaded or just wrote, so a recording started
			// afterward can stamp a reproducible entry point — even when the user loaded via
			// the in-game menu (devbench didn't broker it). kPreLoadGame / kSaveGame carry the
			// save's base name as a null-terminated string in data.
			//
			// Read it with a BOUNDED strnlen — do NOT use a_msg->dataLen. For kSaveGame dataLen is
			// not the string length, and std::string(data, dataLen) with a bogus length allocates
			// huge and HANGS the main thread (this handler runs there). Verified live: that hang
			// is exactly what froze the game on a console `save`.
			if ((a_msg->type == SKSE::MessagingInterface::kPreLoadGame ||
					a_msg->type == SKSE::MessagingInterface::kSaveGame) &&
				a_msg->data) {
				const char* raw = static_cast<const char*>(a_msg->data);
				std::string name(raw, strnlen(raw, 255));  // capped; never trust dataLen
				if (name.size() > 4 && name.compare(name.size() - 4, 4, ".ess") == 0)
					name.resize(name.size() - 4);  // the load tool takes the stem, not the filename
				if (!name.empty())
					dvb::Recording::NoteLoadEntry(name);
			}
			// Fire the one-shot autorun benchmark once a game has loaded.
			if (a_msg->type == SKSE::MessagingInterface::kPostLoadGame)
				dvb::OnPostLoadGame();
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	InitLogging();
	SKSE::AllocTrampoline(1 << 10);
	logs::info("devbench {} loaded", DEVBENCH_VERSION_STRING);

	if (auto* messaging = SKSE::GetMessagingInterface())
		messaging->RegisterListener(OnMessage);

	return true;
}
