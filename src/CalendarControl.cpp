#include "CalendarControl.h"

#include "CalendarProtocol.h"
#include "GameClock.h"
#include "GameState.h"
#include "MainThread.h"
#include "Server.h"
#include "ToolRegistry.h"
#include "Version.h"

#include <RE/B/BSTimer.h>
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <format>
#include <mutex>

namespace dvb::CalendarControl
{
	namespace
	{
		std::mutex    g_mutex;
		std::uint64_t g_generation = 1;
		bool          g_loading = false, g_pumping = false;
		std::atomic<bool> g_stopping{ false };

		std::int64_t Now()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		const std::string& ProcessSession()
		{
			static const std::string identity = [] {
				FILETIME creation{}, exit{}, kernel{}, user{};
				if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
					return std::string{};
				const auto started = (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
				return std::format("{}:{:016X}", GetCurrentProcessId(), started);
			}();
			return identity;
		}

		class NativeBackend final : public Backend
		{
		public:
			Snapshot Read() override
			{
				Snapshot s;
				s.source.processSession = ProcessSession();
				s.source.generation = g_generation;
				s.frame = game::CurrentFrame();
				auto* cal = RE::Calendar::GetSingleton();
				if (!cal || s.source.processSession.empty())
					return s;
				s.source.calendar = reinterpret_cast<std::uintptr_t>(cal);
				const std::array<RE::TESGlobal*, 6> globals{
					cal->gameYear, cal->gameMonth, cal->gameDay, cal->gameHour, cal->gameDaysPassed, cal->timeScale
				};
				for (std::size_t i = 0; i < globals.size(); ++i) {
					if (!globals[i] || !std::isfinite(globals[i]->value))
						return s;
					s.source.forms[i] = globals[i]->GetFormID();
					if (s.source.forms[i] == 0)
						return s;
					s.source.storage[i] = reinterpret_cast<std::uintptr_t>(globals[i]);
					if (i < s.date.size())
						s.date[i] = globals[i]->value;
				}
				s.rate = cal->timeScale->value;
				s.engineMultiplier = RE::BSTimer::QGlobalTimeMultiplier();
				s.available = std::isfinite(s.engineMultiplier);
				auto* pc = RE::PlayerCharacter::GetSingleton();
				auto* cell = pc ? pc->GetParentCell() : nullptr;
				if (cell)
					s.source.cell = cell->GetFormID();
				auto* ui = RE::UI::GetSingleton();
				s.worldLoaded = !g_loading && pc && pc->Get3D() && cell && ui &&
					!ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) && !ui->IsMenuOpen(RE::MainMenu::MENU_NAME);
				return s;
			}

			bool WriteRate(const Snapshot& a_expected, float a_rate) override
			{
				const auto current = Read();
				if (!current.available || !current.source.SameStorage(a_expected.source) || current.rate != a_expected.rate)
					return false;
				RE::Calendar::GetSingleton()->timeScale->value = a_rate;
				return true;
			}
		};

		NativeBackend g_backend;
		Controller    g_controller(g_backend);
		Snapshot      g_lastRead;

		json Binding(const Source& a_source)
		{
			return json{ { "processSession", a_source.processSession }, { "pid", GetCurrentProcessId() },
				{ "loadGeneration", a_source.generation }, { "cellFormId", a_source.cell },
				{ "globalFormIds", a_source.forms } };
		}

		json Values(const Snapshot& a_snapshot)
		{
			if (!a_snapshot.available)
				return nullptr;
			return json{ { "year", a_snapshot.date[0] }, { "month", a_snapshot.date[1] },
				{ "day", a_snapshot.date[2] }, { "gameHour", a_snapshot.date[3] },
				{ "daysPassed", a_snapshot.date[4] }, { "calendarRate", a_snapshot.rate },
				{ "engineMultiplier", a_snapshot.engineMultiplier } };
		}

		void UpdatePump()
		{
			const bool needed = g_controller.NeedsPump();
			if (needed == g_pumping)
				return;
			g_pumping = needed;
			if (needed)
				GameClock::Engage();
			else
				GameClock::Disengage();
		}

		json Receipt(const Outcome& a_result, const Snapshot& a_snapshot, bool a_fresh)
		{
			const auto now = Now();
			const auto& current = g_controller.Current();
			const auto& selected = current ? current : g_controller.Last();
			json out{ { "ok", a_result.ok }, { "status", a_result.code }, { "schemaVersion", 1 },
				{ "plugin", "devbench" }, { "version", DEVBENCH_VERSION_STRING },
				{ "binding", Binding(a_snapshot.source) }, { "frame", a_snapshot.frame },
				{ "readbackFresh", a_fresh }, { "available", a_fresh && a_snapshot.available },
				{ "worldLoaded", a_fresh && a_snapshot.worldLoaded }, { "values", a_fresh ? Values(a_snapshot) : json(nullptr) },
				{ "observedMonotonicMs", now }, { "restored", a_result.restored },
				{ "globalOrder", json::array({ "year", "month", "day", "gameHour", "daysPassed", "calendarRate" }) },
				{ "serviceStopping", g_stopping.load() }, { "outstanding", current.has_value() },
				{ "leaseActive", current && now < current->deadline && !current->cleanupAttempted },
				{ "expiryDue", current && now >= current->deadline },
				{ "cleanupPending", current && (now >= current->deadline || current->cleanupAttempted || g_stopping) },
				{ "disconnectRecovery", "expiry-bounded; no disconnect callback" } };
			if (selected)
				out["lease"] = json{ { "id", selected->id }, { "owner", selected->owner },
					{ "commandId", selected->command }, { "binding", Binding(selected->baseline.source) },
					{ "deadlineMonotonicMs", selected->deadline }, { "applied", selected->applied },
					{ "captured", Values(selected->baseline) }, { "cleanupAttempted", selected->cleanupAttempted } };
			out["holdValid"] = a_fresh && current && current->applied && !current->cleanupAttempted && now < current->deadline &&
				a_snapshot.available && a_snapshot.worldLoaded && a_snapshot.source == current->baseline.source &&
				a_snapshot.date == current->baseline.date && a_snapshot.rate == 0 &&
				a_snapshot.engineMultiplier == current->baseline.engineMultiplier;
			out["lastTransition"] = json{ { "status", g_controller.Result().code },
				{ "ok", g_controller.Result().ok }, { "restored", g_controller.Result().restored } };
			return out;
		}

		json Handle(const json& a_args, const ToolContext& a_ctx)
		{
			const auto request = ParseRequest(a_args);
			const auto& action = request.action;
			const auto& owner = request.owner;
			const auto& command = request.command;
			const auto& lease = request.lease;
			const auto duration = request.duration;
			const auto applyDeadline = Now() + 5000;
			const auto connection = a_ctx.clientId.empty() ? std::string("rest") : "mcp:" + a_ctx.clientId;
			try {
				return MainThread::RunAndWait([=]() -> json {
					std::lock_guard lock(g_mutex);
					g_controller.Tick(Now());
					const auto s = g_backend.Read();
					Outcome result{ true, "observed", false };
					if (action != "status") {
						if (g_stopping)
							result = { false, "service_stopping", false };
						else if (a_args["binding"] != Binding(s.source))
							result = { false, "binding_mismatch", false };
						else if (action == "hold")
							result = g_controller.Hold(s.source, owner, connection, command, Now(), duration, applyDeadline);
						else
							result = g_controller.Release(s.source, owner, connection, lease);
					}
					UpdatePump();
					g_lastRead = g_backend.Read();
					auto out = Receipt(result, g_lastRead, true);
					out["action"] = action;
					out["commandId"] = command;
					return out;
				});
			} catch (const MainThread::TaskTimeout& e) {
				std::unique_lock lock(g_mutex, std::try_to_lock);
				auto out = lock.owns_lock() ? Receipt({ false, "main_thread_timeout", false }, g_lastRead, false) :
					json{ { "ok", false }, { "status", "main_thread_timeout" }, { "readbackFresh", false },
						{ "available", false }, { "leaseStateKnown", false } };
				out["taskStarted"] = e.started;
				out["mayCompleteLater"] = e.started;
				out["commandId"] = command;
				out["error"] = e.what();
				return out;
			}
		}
	}

	void Register(ToolRegistry& a_registry)
	{
		a_registry.Register(Descriptor(), &Handle);
	}

	void Reconcile()
	{
		std::lock_guard lock(g_mutex);
		if (g_stopping)
			g_controller.End("service_stop", true);
		else
			g_controller.Tick(Now());
		UpdatePump();
	}

	void OnLifecycle(std::uint32_t a_type)
	{
		std::lock_guard lock(g_mutex);
		if (a_type == SKSE::MessagingInterface::kPreLoadGame) {
			g_controller.End("pre_load", true);
			++g_generation;
			g_loading = true;
		} else if (a_type == SKSE::MessagingInterface::kPostLoadGame || a_type == SKSE::MessagingInterface::kNewGame) {
			g_controller.End("new_load_generation", false);
			++g_generation;
			g_loading = false;
		} else if (a_type == SKSE::MessagingInterface::kSaveGame)
			g_controller.End("unsupported_save_event", true);
		UpdatePump();
	}

	void OnLoadingMenu()
	{
		std::lock_guard lock(g_mutex);
		g_controller.End("loading_menu", true);
		UpdatePump();
	}

	void RequestStop()
	{
		g_stopping.store(true);
	}

	bool Outstanding()
	{
		std::lock_guard lock(g_mutex);
		return g_controller.Current().has_value();
	}
}
