#pragma once

#include "Json.h"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace dvb
{
	struct MenuInventorySnapshot
	{
		std::vector<std::string> openMenus;
		bool initialized{ false };

		bool KnownAbsent(std::string_view a_name) const
		{
			return initialized && std::find(openMenus.begin(), openMenus.end(), a_name) == openMenus.end();
		}

		bool KnownEmpty() const { return initialized && openMenus.empty(); }

		json Describe(const json& a_registered) const
		{
			const bool modal = std::find(openMenus.begin(), openMenus.end(), "MessageBoxMenu") != openMenus.end();
			return json{
				{ "openMenus", openMenus },
				{ "inventoryInitialized", initialized },
				{ "inventorySource", initialized ? "initial-ui-snapshot-and-events" : "events-only-incomplete" },
				// A positive observation remains useful; absence is unknown until seeded.
				{ "messageBoxOpen", (modal || initialized) ? json(modal) : json(nullptr) },
				{ "registered", a_registered },
			};
		}
	};

	// Production event inventory. UI enumeration is supplied only by the owning
	// main-thread install, never performed by listener/watchdog readers.
	class MenuInventory
	{
	public:
		std::uint64_t BeginSeed() const
		{
			std::lock_guard lock(mutex_);
			return revision_;
		}

		bool Seed(std::uint64_t a_revision, const std::vector<std::string>& a_names)
		{
			std::unordered_set<std::string> names(a_names.begin(), a_names.end());
			std::lock_guard lock(mutex_);
			// Never overwrite an event delivered during collection with a stale seed.
			if (revision_ != a_revision)
				return false;
			openMenus_.swap(names);
			initialized_ = true;
			++revision_;
			return true;
		}

		void OnEvent(const std::string& a_name, bool a_opening)
		{
			std::lock_guard lock(mutex_);
			if (a_opening)
				openMenus_.insert(a_name);
			else
				openMenus_.erase(a_name);
			++revision_;
		}

		MenuInventorySnapshot Snapshot() const
		{
			std::lock_guard lock(mutex_);
			MenuInventorySnapshot out{ { openMenus_.begin(), openMenus_.end() }, initialized_ };
			std::sort(out.openMenus.begin(), out.openMenus.end());
			return out;
		}

	private:
		mutable std::mutex mutex_;
		std::unordered_set<std::string> openMenus_;
		std::uint64_t revision_{ 0 };
		bool initialized_{ false };
	};
}
