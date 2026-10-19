#pragma once

#include "MenuInventory.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dvb
{
	class EventBus;

	/// Main-thread initial UI snapshot followed by menu events. Readers never touch
	/// engine UI. Before a successful seed, absent names are unknown, not closed.
	MenuInventorySnapshot GetMenuInventory();

	/// Best-effort names for recording/watchdog context. Use GetMenuInventory for
	/// negative assertions: an empty vector alone does not prove an empty UI.
	std::vector<std::string> GetOpenMenus();

	/// Install game event sources that publish into `a_bus`:
	///  - a MenuOpenCloseEvent sink → "menu" events { name, opening } (loading screens,
	///    main menu, new-game message boxes, etc.).
	/// SKSE-messaging lifecycle events are forwarded separately via OnSKSEMessage.
	/// Call once on the deferred main-thread startup task after the EventBus exists.
	/// Subscribe then seed from the current UI; do not synthesize open events.
	void InstallGameEvents(EventBus& a_bus);

	/// Forward an SKSE MessagingInterface message type; publishes a "lifecycle" event
	/// (dataLoaded / newGame / preLoadGame / postLoadGame / saveGame / deleteGame).
	/// No-op until InstallGameEvents has run.
	void OnSKSEMessage(std::uint32_t a_type);
}
