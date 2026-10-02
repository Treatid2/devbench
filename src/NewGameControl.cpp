#include "NewGameControl.h"

#include "MainThread.h"
#include "NewGameRequestLedger.h"
#include "ToolRegistry.h"

#include <cmath>
#include <atomic>

namespace dvb::NewGameControl
{
	namespace
	{
		constexpr auto          kMoviePath = "_root.MenuHolder.Menu_mc";
		constexpr auto          kRequestLifetime = std::chrono::seconds(60);
		constexpr std::uint32_t kMaximumEntries = 32;
		constexpr double        kNewEntryID = 1.0;

		// All movie access and request bookkeeping share the game-thread dispatch.
		RequestLedger                        g_receipts;
		std::string                           g_pending;
		RE::GPtr<RE::GFxMovieView>            g_pendingMovie;
		std::chrono::steady_clock::time_point g_expires;
		std::atomic<std::uint64_t>             g_menuEpoch{ 0 };
		std::uint64_t                          g_pendingEpoch = 0;

		RE::GFxValue Member(const RE::GFxValue& a_object, const char* a_name)
		{
			RE::GFxValue value;
			if (!a_object.IsObject() || !a_object.GetMember(a_name, &value))
				throw ToolError(422, std::format("newGame: unsupported movie member '{}'", a_name));
			return value;
		}

		std::string StringMember(const RE::GFxValue& a_object, const char* a_name)
		{
			const auto value = Member(a_object, a_name);
			if (!value.IsString())
				throw ToolError(422, std::format("newGame: '{}' is not a string", a_name));
			return value.GetString();
		}

		double NumberMember(const RE::GFxValue& a_object, const char* a_name)
		{
			const auto value = Member(a_object, a_name);
			if (!value.IsNumber() || !std::isfinite(value.GetNumber()))
				throw ToolError(422, std::format("newGame: '{}' is not a finite number", a_name));
			return value.GetNumber();
		}

		bool IsNewEntry(const RE::GFxValue& a_entry)
		{
			return NumberMember(a_entry, "index") == kNewEntryID;
		}

		bool IsEnabledNewEntry(const RE::GFxValue& a_entry)
		{
			if (!IsNewEntry(a_entry) || StringMember(a_entry, "text") != "$NEW")
				return false;
			const auto disabled = Member(a_entry, "disabled");
			return disabled.IsBool() && !disabled.GetBool();
		}

		void ClearPending(const char* a_phase)
		{
			if (!g_pending.empty())
				g_receipts.Invalidate(g_pending, a_phase);
			g_pending.clear();
			g_pendingMovie.reset();
		}

		void ValidatePending(RE::GFxMovieView* a_movie)
		{
			if (!g_pending.empty() && g_pendingEpoch != g_menuEpoch.load())
				ClearPending("menuInterrupted");
			if (!g_pending.empty() && (a_movie != g_pendingMovie.get() ||
										  std::chrono::steady_clock::now() >= g_expires))
				ClearPending(a_movie != g_pendingMovie.get() ? "menuReplaced" : "expired");
		}

		struct MenuState
		{
			RE::GPtr<RE::GFxMovieView> movie;
			RE::GPtr<RE::FxDelegate>   delegate;
			RE::GFxValue               menu;
			RE::GFxValue               list;
			std::string                state;
		};

		MenuState ReadMenu()
		{
			auto* ui = RE::UI::GetSingleton();
			auto  menu = ui ? ui->GetMenu(RE::MainMenu::MENU_NAME) : nullptr;
			if (!ui || !ui->IsMenuOpen(RE::MainMenu::MENU_NAME) || !menu || !menu->uiMovie)
				throw ToolError(409, "newGame: Main Menu is not open");
			if (ui->IsMenuOpen(RE::MessageBoxMenu::MENU_NAME))
				throw ToolError(409, "newGame: an unrelated MessageBoxMenu is blocking the main menu");
			MenuState result;
			result.movie = menu->uiMovie;
			result.delegate = menu->fxDelegate;
			if (!result.movie->GetVariable(&result.menu, kMoviePath) || !result.menu.IsObject())
				throw ToolError(422, "newGame: unsupported Main Menu movie path");
			result.list = Member(result.menu, "MainList");
			result.state = StringMember(result.menu, "currentState");
			ValidatePending(result.movie.get());
			return result;
		}

		void RequireCallback(MenuState& a_menu, const char* a_name)
		{
			if (!a_menu.delegate)
				throw ToolError(422, "newGame: Main Menu has no native delegate");
			const auto* callback = a_menu.delegate->callbacks.GetAlt(a_name);
			if (!callback || !callback->handler || !callback->callback)
				throw ToolError(422, std::format("newGame: native '{}' callback is unavailable", a_name));
		}

		json Describe(MenuState& a_menu)
		{
			json       result{ { "mainMenuOpen", true }, { "state", a_menu.state },
				{ "moviePath", kMoviePath }, { "pendingRequestId", g_pending },
				{ "unresolvedRequestId", g_receipts.UnresolvedID() },
				{ "newRequestsBlocked", !g_receipts.UnresolvedID().empty() },
				{ "readyToRequest", a_menu.state == "Main" && g_pending.empty() && g_receipts.CanRequest() },
				{ "readyToConfirm", false } };
			const auto selected = Member(a_menu.list, "selectedEntry");
			result["selectedEntryId"] = selected.IsObject() ? json(NumberMember(selected, "index")) : json(nullptr);
			result["readyToConfirm"] = a_menu.state == "MainConfirm" && g_receipts.IsRequested(g_pending) &&
			                           selected.IsObject() && IsEnabledNewEntry(selected);
			return result;
		}

		std::uint32_t NewRow(MenuState& a_menu)
		{
			const auto entries = Member(a_menu.list, "entryList");
			if (!entries.IsArray() || entries.GetArraySize() > kMaximumEntries)
				throw ToolError(422, "newGame: unsupported Main Menu entry list");
			std::optional<std::uint32_t> row;
			for (std::uint32_t i = 0; i < entries.GetArraySize(); ++i) {
				RE::GFxValue entry;
				if (!entries.GetElement(i, &entry))
					throw ToolError(422, "newGame: unreadable Main Menu entry");
				if (!IsNewEntry(entry))
					continue;
				const auto disabled = Member(entry, "disabled");
				if (!disabled.IsBool() || disabled.GetBool() || row || StringMember(entry, "text") != "$NEW")
					throw ToolError(422, "newGame: New entry is disabled or ambiguous");
				row = i;
			}
			if (!row)
				throw ToolError(422, "newGame: New entry is absent");
			return *row;
		}

		json Request(MenuState& a_menu, const std::string& a_id)
		{
			if (const auto unresolved = g_receipts.UnresolvedID(); !unresolved.empty())
				throw ToolError(409, std::format("newGame: request '{}' has unresolved dispatch; inspect that ID, never retry with a fresh ID; restart the process if it cannot resolve", unresolved));
			if (a_menu.state != "Main" || !g_pending.empty())
				throw ToolError(409, "newGame: request needs idle Main state with no pending request");
			RequireCallback(a_menu, "NEW");
			RequireCallback(a_menu, "fadeOutStarted");
			RequireCallback(a_menu, "StartNewGame");
			const auto row = NewRow(a_menu);
			if (!g_receipts.CanRequest())
				throw ToolError(409, "newGame: request history is full; restart the test session");
			g_receipts.BeginRequest(a_id, row);
			g_pending = a_id;
			g_pendingMovie = a_menu.movie;
			g_pendingEpoch = g_menuEpoch.load();
			g_expires = std::chrono::steady_clock::now() + kRequestLifetime;
			// Record uncertainty before entering GFx; a lost response must not replay NEW.
			const RE::GFxValue selection(static_cast<double>(row));
			if (!a_menu.list.Invoke("__set__selectedIndex", nullptr, &selection, 1) ||
				!IsEnabledNewEntry(Member(a_menu.list, "selectedEntry")))
				throw ToolError(422, "newGame: New selection did not read back; inspect before another request");
			// Native NEW requests the real confirmation; Callback requires a response ID.
			const RE::GFxValue responseID(0.0);
			a_menu.delegate->Callback(a_menu.movie.get(), "NEW", &responseID, 1);
			// Re-read the live menu, not just a field on the retained pre-call object.
			// Closure/replacement/modal/expiry may clean up the movie but cannot clear uncertainty.
			auto post = ReadMenu();
			if (g_pending != a_id || post.movie.get() != a_menu.movie.get() ||
				g_pendingEpoch != g_menuEpoch.load())
				throw ToolError(422, "newGame: request menu changed after NEW; do not replay");
			if (!g_receipts.CompleteRequest(a_id, post.state,
					IsEnabledNewEntry(Member(post.list, "selectedEntry"))))
				throw ToolError(422, "newGame: NEW did not reach the selected New confirmation; do not replay");
			return g_receipts.Snapshot(a_id);
		}

		json Confirm(MenuState& a_menu, const std::string& a_id)
		{
			if (g_pending != a_id || !g_receipts.IsRequested(a_id) || a_menu.state != "MainConfirm" ||
				!IsEnabledNewEntry(Member(a_menu.list, "selectedEntry")))
				throw ToolError(409, "newGame: confirm needs this request's ready New confirmation");
			RequireCallback(a_menu, "fadeOutStarted");
			RequireCallback(a_menu, "StartNewGame");
			g_receipts.BeginConfirmation(a_id);
			if (!a_menu.menu.Invoke("onAcceptPress"))
				throw ToolError(422, "newGame: confirmation invocation failed; do not replay");
			if (StringMember(a_menu.menu, "strFadeOutCallback") != "StartNewGame")
				throw ToolError(422, "newGame: confirmation did not schedule StartNewGame; do not replay");
			g_receipts.CompleteConfirmation(a_id);
			g_pending.clear();
			g_pendingMovie.reset();
			return g_receipts.Snapshot(a_id);
		}
	}

	void OnMenuEvent(const RE::MenuOpenCloseEvent& a_event)
	{
		// A close/reopen of the same retained movie, or a modal appearing and
		// disappearing between calls, must not revive a pending confirmation.
		if ((a_event.menuName == RE::MainMenu::MENU_NAME && !a_event.opening) ||
			(a_event.menuName == RE::MessageBoxMenu::MENU_NAME && a_event.opening))
			g_menuEpoch.fetch_add(1);
	}

	json Handle(const json& a_args)
	{
		for (const auto* field : { "phase", "requestId" }) {
			if (a_args.contains(field) && !a_args[field].is_string())
				throw ToolError(400, std::format("newGame: '{}' must be a string", field));
		}
		const std::string phase = a_args.value("phase", std::string("inspect"));
		const std::string id = a_args.value("requestId", std::string{});
		if (phase != "inspect" && phase != "request" && phase != "confirm")
			throw ToolError(400, "newGame: phase must be inspect, request or confirm");
		if (id.size() > 128 || (phase != "inspect" && id.empty()))
			throw ToolError(400, "newGame: request/confirm requires a unique requestId of 1..128 bytes");
		if (phase == "confirm" && (!a_args.contains("confirmNewGame") ||
									  !a_args["confirmNewGame"].is_boolean() || !a_args["confirmNewGame"].get<bool>()))
			throw ToolError(400, "newGame: confirmation requires confirmNewGame:true");
		return MainThread::RunAndWait([phase, id]() -> json {
			if (!REL::Module::IsVR())
				throw ToolError(422, "newGame: this menu adapter supports VR assets only");
			auto* ui = RE::UI::GetSingleton();
			auto  menu = ui ? ui->GetMenu(RE::MainMenu::MENU_NAME) : nullptr;
			ValidatePending(menu && ui->IsMenuOpen(RE::MainMenu::MENU_NAME) ? menu->uiMovie.get() : nullptr);
			if (g_receipts.Contains(id) && (phase != "confirm" || !g_receipts.IsRequested(id)))
				return g_receipts.Snapshot(id);
			if (phase == "confirm" && !g_receipts.Contains(id))
				throw ToolError(409, "newGame: unknown requestId");
			if (phase == "inspect" && (!ui || !ui->IsMenuOpen(RE::MainMenu::MENU_NAME)))
				return json{ { "mainMenuOpen", false }, { "readyToRequest", false }, { "readyToConfirm", false },
					{ "pendingRequestId", g_pending }, { "unresolvedRequestId", g_receipts.UnresolvedID() },
					{ "newRequestsBlocked", !g_receipts.UnresolvedID().empty() } };
			auto state = ReadMenu();
			if (phase == "inspect")
				return Describe(state);
			return phase == "request" ? Request(state, id) : Confirm(state, id);
		});
	}
}
