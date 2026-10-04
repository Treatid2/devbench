#pragma once

#include "Json.h"

#include <cstddef>
#include <utility>

namespace dvb
{
	inline const char* ConventionalHeldNodeName(bool a_left)
	{
		return a_left ? "SHIELD" : "WEAPON";
	}

	// Pure response policy shared by the live observer and host regression tests.
	// Keep view occurrences: neither native pointers nor equipped-item identities
	// establish one logical light. Scene-list membership does not prove visibility.
	class HeldLightObservation
	{
	public:
		void AddView(const char* a_view, const char* a_nodeName, bool a_rootAvailable,
			bool a_nodeFound, json a_lights, json a_coverage = nullptr)
		{
			const bool found = a_rootAvailable && a_nodeFound;
			json view{
				{ "view", a_view }, { "rootAvailable", a_rootAvailable },
				{ "searchedNode", a_nodeName }, { "nodeFound", found },
				{ "coverage", "conventional-first-named-node-only" },
				{ "traversalCoverage", "depth-limited-no-completeness-proof" },
				{ "maxTraversalDepth", 256 }
			};
			if (!a_coverage.is_null()) {
				view["traversalCoverage"] = "bounded-unique-pointer-subgraph";
				view["graphCoverage"] = std::move(a_coverage);
			}
			_views.push_back(std::move(view));
			if (!found)
				return;
			for (auto& light : a_lights) {
				light["view"] = a_view;
				const auto membership = light.find("inScene");
				if (membership != light.end() && membership->is_string() &&
					(*membership == "active" || *membership == "shadow"))
					++_entriesInScene;
				_lights.push_back(std::move(light));
			}
		}

		void WriteTo(json& a_hand) &&
		{
			a_hand["heldLightObservationVersion"] = 2;
			a_hand["heldLights"] = std::move(_lights);
			a_hand["heldLightEntriesInScene"] = _entriesInScene;
			a_hand["heldLightCoverage"] = std::move(_views);
			a_hand["visibleIlluminationProven"] = false;
			// Compatibility only. The name historically overclaimed what was measured.
			a_hand["heldLightsRendered"] = _entriesInScene;
			a_hand["heldLightsRenderedSemantics"] =
				"deprecated-alias-of-heldLightEntriesInScene-not-visible-or-unique";
		}

	private:
		json        _lights = json::array();
		json        _views = json::array();
		std::size_t _entriesInScene = 0;
	};
}
