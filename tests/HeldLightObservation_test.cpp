#include "test_framework.h"
#include "HeldLightObservation.h"

#include <string>
#include <utility>

namespace
{
	using dvb::json;

	json Finish(dvb::HeldLightObservation&& a_observation)
	{
		json hand{ { "equipped", "preserved" }, { "caster", nullptr } };
		std::move(a_observation).WriteTo(hand);
		return hand;
	}
}

TEST_CASE("held light observation counts only active and shadow entries, not visibility")
{
	dvb::HeldLightObservation observation;
	observation.AddView("thirdPerson", "WEAPON", true, true, json::array({
		json{ { "inScene", "active" }, { "appCulled", true }, { "fade", 0 } },
		json{ { "inScene", "shadow" } }, json{ { "inScene", false } },
		json{ { "inScene", true } }, json{ { "inScene", "other" } },
		json{ { "inScene", nullptr } }, json{ { "name", "missing membership" } }
	}));
	const auto hand = Finish(std::move(observation));
	CHECK(hand["heldLightEntriesInScene"] == 2);
	CHECK(hand["heldLights"].size() == 7);
	CHECK(hand["heldLights"][0]["appCulled"] == true);
	CHECK(hand["visibleIlluminationProven"] == false);
}

TEST_CASE("held light observation retains duplicate subtree and cross-view occurrences")
{
	const json light{ { "name", "same returned light" }, { "inScene", "active" } };
	dvb::HeldLightObservation observation;
	observation.AddView("thirdPerson", "WEAPON", true, true, json::array({ light, light }));
	observation.AddView("firstPerson", "WEAPON", true, true, json::array({ light }));
	const auto hand = Finish(std::move(observation));
	CHECK(hand["heldLightEntriesInScene"] == 3);
	CHECK(hand["heldLights"].size() == 3);
	CHECK(hand["heldLights"][0]["view"] == "thirdPerson");
	CHECK(hand["heldLights"][2]["view"] == "firstPerson");
	CHECK(!hand.contains("uniqueSceneLightPointers"));
}

TEST_CASE("held light observation distinguishes missing root from missing conventional node")
{
	dvb::HeldLightObservation observation;
	observation.AddView("thirdPerson", "SHIELD", false, false, json::array());
	observation.AddView("firstPerson", "SHIELD", true, false, json::array());
	const auto hand = Finish(std::move(observation));
	CHECK(hand["heldLights"].empty());
	CHECK(hand["heldLightEntriesInScene"] == 0);
	const auto& views = hand["heldLightCoverage"];
	CHECK(views.size() == 2);
	CHECK(views[0]["rootAvailable"] == false);
	CHECK(views[1]["rootAvailable"] == true);
	CHECK(views[0]["nodeFound"] == false);
	CHECK(views[1]["nodeFound"] == false);
	CHECK(views[1]["coverage"] == "conventional-first-named-node-only");
}

TEST_CASE("held light observation reports an empty found node with bounded coverage")
{
	dvb::HeldLightObservation observation;
	observation.AddView("thirdPerson", "WEAPON", true, true, json::array());
	const auto hand = Finish(std::move(observation));
	CHECK(hand["heldLights"].empty());
	CHECK(hand["heldLightCoverage"][0]["nodeFound"] == true);
	CHECK(hand["heldLightCoverage"][0]["maxTraversalDepth"] == 256);
	CHECK(hand["heldLightCoverage"][0]["traversalCoverage"] == "depth-limited-no-completeness-proof");
	CHECK(hand["visibleIlluminationProven"] == false);
}

TEST_CASE("held light observation selects conventional names without guessing equipment layouts")
{
	CHECK(std::string(dvb::ConventionalHeldNodeName(true)) == "SHIELD");
	CHECK(std::string(dvb::ConventionalHeldNodeName(false)) == "WEAPON");
	dvb::HeldLightObservation observation;
	observation.AddView("firstPerson", dvb::ConventionalHeldNodeName(false), true, true,
		json::array({ json{ { "inScene", "shadow" } } }));
	const auto hand = Finish(std::move(observation));
	CHECK(hand["heldLightCoverage"][0]["searchedNode"] == "WEAPON");
	CHECK(hand["heldLightCoverage"][0]["view"] == "firstPerson");
	CHECK(hand["heldLightEntriesInScene"] == 1);
}

TEST_CASE("held light observation retains a deprecated count alias and unrelated hand fields")
{
	dvb::HeldLightObservation observation;
	observation.AddView("firstPerson", "WEAPON", true, true,
		json::array({ json{ { "inScene", "active" } } }));
	const auto hand = Finish(std::move(observation));
	CHECK(hand["heldLightObservationVersion"] == 2);
	CHECK(hand["heldLightsRendered"] == hand["heldLightEntriesInScene"]);
	CHECK(hand["heldLightsRenderedSemantics"] ==
		"deprecated-alias-of-heldLightEntriesInScene-not-visible-or-unique");
	CHECK(hand["equipped"] == "preserved");
	CHECK(hand["caster"].is_null());
}
