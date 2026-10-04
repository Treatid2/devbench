#include "test_framework.h"
#include "LightObservation.h"
#include "HeldLightObservation.h"

#include <algorithm>

namespace
{
	using namespace dvb::LightObservation;
	struct Node { std::vector<Node*> children; Node* parent = nullptr; bool light = false; };
	struct Children
	{
		std::size_t Count(Node* n) const { return n->children.size(); }
		Node* Child(Node* n, std::size_t i) const { return n->children[i]; }
	};
	WalkReport Collect(Node* root, Budget& budget, std::vector<Node*>& lights)
	{
		return Walk(root, budget, Children{}, [&](Node* n) {
			if (n->light && budget.Emit(n)) lights.push_back(n);
			return false;
		});
	}
}

TEST_CASE("light graph self and two-node cycles terminate with unique pointer visitation")
{
	Node a, b;
	a.children = { &a, &b }; b.children = { &a }; a.light = b.light = true;
	Budget budget; std::vector<Node*> lights;
	const auto report = Collect(&a, budget, lights);
	CHECK(report.complete); CHECK(report.nodes == 2); CHECK(report.edges == 3);
	CHECK(report.repeats == 2); CHECK(lights.size() == 2);
}

TEST_CASE("light graph shared child and repeated light emit once within a graph")
{
	Node a, b, c, light;
	a.children = { &b, &c, &light, nullptr }; b.children = { &light }; c.children = { &light };
	light.light = true;
	Budget budget; std::vector<Node*> lights;
	const auto report = Collect(&a, budget, lights);
	CHECK(report.complete); CHECK(report.nodes == 4); CHECK(lights.size() == 1);
	CHECK(budget.lights.size() == 1);
}

TEST_CASE("light graph extreme width reads only budgeted child slots without materialising a frontier")
{
	struct Wide
	{
		std::size_t* reads;
		std::size_t Count(Node*) const { return std::numeric_limits<std::size_t>::max(); }
		Node* Child(Node*, std::size_t) const { ++*reads; return nullptr; }
	};
	Node root; Budget budget; budget.limits.edges = 7; std::size_t reads = 0;
	const auto report = Walk(&root, budget, Wide{ &reads }, [](Node*) { return false; });
	CHECK(!report.complete); CHECK(reads == 7); CHECK(report.edges == 7);
	CHECK(budget.reasons.contains("edge-budget"));
}

TEST_CASE("light graph depth cutoff is explicit and iterative")
{
	std::vector<Node> nodes(300);
	for (std::size_t i = 0; i + 1 < nodes.size(); ++i) nodes[i].children = { &nodes[i + 1] };
	Budget budget; budget.limits.depth = 8; std::vector<Node*> lights;
	const auto report = Collect(&nodes[0], budget, lights);
	CHECK(!report.complete); CHECK(report.nodes == 9); CHECK(budget.nodes == 9);
	CHECK(report.reasons.contains("depth-budget"));
}

TEST_CASE("light graph node budget is shared across independent searches and view collections")
{
	Node root, child; root.children = { &child }; Budget budget; budget.limits.nodes = 3;
	std::vector<Node*> lights;
	CHECK(Collect(&root, budget, lights).complete);
	const auto second = Collect(&root, budget, lights);
	CHECK(!second.complete); CHECK(budget.nodes == 3);
	CHECK(second.reasons.contains("node-budget"));
}

TEST_CASE("light parent self and two-node cycles are explicit partial lineage")
{
	Node a, b; a.parent = &b; b.parent = &a; Budget budget;
	const auto report = Parents(&a, budget, [](Node* n) { return n->parent; }, [](Node*) { return false; });
	CHECK(!report.complete); CHECK(report.nodes == 2); CHECK(report.reasons.contains("parent-cycle"));
	a.parent = &a; Budget self;
	CHECK(Parents(&a, self, [](Node* n) { return n->parent; }, [](Node*) { return false; }).nodes == 1);
}

TEST_CASE("light parent ancestor and shared-work caps prevent unbounded lookup")
{
	Node a, b, c; a.parent = &b; b.parent = &c;
	Budget budget; budget.limits.ancestors = 2;
	CHECK(!Parents(&a, budget, [](Node* n) { return n->parent; }, [](Node*) { return false; }).complete);
	CHECK(budget.reasons.contains("ancestor-budget"));
	Budget shared; shared.limits.parents = 1;
	CHECK(!Parents(&a, shared, [](Node* n) { return n->parent; }, [](Node*) { return false; }).complete);
	CHECK(shared.parents == 1); CHECK(shared.reasons.contains("parent-budget"));
}

TEST_CASE("light conventional first-match search uses the bounded production traversal")
{
	Node a, b, c; a.children = { &b, &c }; b.children = { &a }; Budget budget; Node* found = nullptr;
	const auto report = Walk(&a, budget, Children{}, [&](Node* n) { if (n == &c) found = n; return found != nullptr; });
	CHECK(found == &c); CHECK(report.complete); CHECK(report.stoppedAfterMatch);
	CHECK(report.nodes == 3);
}

TEST_CASE("light unique-pointer cap differs from view-occurrence output cap")
{
	Node a, b; Budget budget; budget.limits.uniqueLights = 1; budget.limits.outputs = 2;
	CHECK(budget.Emit(&a)); CHECK(budget.Emit(&a)); CHECK(budget.lights.size() == 1);
	CHECK(!budget.Emit(&b)); CHECK(budget.reasons.contains("unique-light-budget"));
	CHECK(!budget.Emit(&a)); CHECK(budget.reasons.contains("output-budget"));
	CHECK(budget.outputs == 2);
}

TEST_CASE("light null roots expose availability without consuming graph budget")
{
	Budget budget; std::vector<Node*> lights;
	const auto report = Collect(nullptr, budget, lights);
	CHECK(!report.available); CHECK(report.Fields()["complete"] == false);
	CHECK(budget.nodes == 0); CHECK(lights.empty());
}

TEST_CASE("light renderer slot budget charges null and duplicate slots through shared Take seam")
{
	Budget budget; budget.limits.rendererEntries = 2;
	CHECK(budget.Take(budget.rendererEntries, budget.limits.rendererEntries, "renderer-entry-budget"));
	CHECK(budget.Take(budget.rendererEntries, budget.limits.rendererEntries, "renderer-entry-budget"));
	CHECK(!budget.Take(budget.rendererEntries, budget.limits.rendererEntries, "renderer-entry-budget"));
	CHECK(budget.Fields()["complete"] == false); CHECK(budget.rendererEntries == 2);
}

TEST_CASE("light request scope omission and explicit ref or scene are admitted")
{
	CHECK(!ParseRequest(dvb::json::object()).scene);
	CHECK(!ParseRequest(dvb::json{ { "scope", "ref" } }).scene);
	CHECK(ParseRequest(dvb::json{ { "scope", "scene" } }).scene);
}

TEST_CASE("light request invalid scope and scene selector presence fail before native work")
{
	for (const auto& scope : dvb::json::array({ "", "Scene", "other", 1, false, nullptr }))
		CHECK_THROWS(ParseRequest(dvb::json{ { "scope", scope } }));
	CHECK_THROWS(ParseRequest(dvb::json{ { "scope", "scene" }, { "selected", false } }));
	CHECK_THROWS(ParseRequest(dvb::json{ { "scope", "scene" }, { "formId", "" } }));
	CHECK_THROWS(ParseRequest(dvb::json{ { "selected", true }, { "formId", "A" } }));
}

TEST_CASE("light request typed finite numeric and selector admission is strict")
{
	CHECK_THROWS(ParseRequest(dvb::json{ { "formId", nullptr } }));
	CHECK_THROWS(ParseRequest(dvb::json{ { "selected", "true" } }));
	CHECK_THROWS(ParseRequest(dvb::json{ { "radius", -1 } }));
	CHECK_THROWS(ParseRequest(dvb::json{ { "radius", "1" } }));
	CHECK_THROWS(ParseRequest(dvb::json{ { "radius", std::numeric_limits<double>::infinity() } }));
	CHECK_THROWS(ParseRequest(dvb::json{ { "limit", 1.5 } }));
	CHECK_THROWS(ParseRequest(dvb::json{ { "limit", -1 } }));
	CHECK_THROWS(ParseRequest(dvb::json{ { "limit", std::numeric_limits<std::uint64_t>::max() } }));
	CHECK(ParseRequest(dvb::json{ { "radius", 0 }, { "limit", 0 } }).limit == 0);
}

TEST_CASE("light distance does not invent origin and rejects nonfinite geometry")
{
	CHECK(!Distance(std::nullopt, { 1, 2, 3 }));
	CHECK(!Distance(Position{ 0, 0, 0 }, { std::numeric_limits<double>::quiet_NaN(), 0, 0 }));
	const auto distance = Distance(Position{ 0, 0, 0 }, { 3, 4, 0 });
	CHECK(distance.has_value()); CHECK(*distance == 5);
}

TEST_CASE("light scene equal distances have stable secondary ordering before limit")
{
	std::vector<SortKey> keys{ { 5, 2, "A", "B", "T", 1 }, { 5, 1, "Z", "B", "T", 2 },
		{ 5, 1, "A", "B", "T", 4 }, { 5, 1, "A", "A", "T", 3 }, { std::nullopt, 0, "", "", "", 0 } };
	std::sort(keys.begin(), keys.end());
	CHECK(keys[0].name == "A"); CHECK(keys[1].pointer == 4); CHECK(keys[2].path == "Z");
	CHECK(keys[3].owner == 2); CHECK(!keys[4].distance);
}

TEST_CASE("held-light bounded coverage keeps version2 aliases and cross-view occurrences")
{
	dvb::HeldLightObservation held;
	const dvb::json light{ { "inScene", "shadow" } };
	held.AddView("thirdPerson", "WEAPON", true, true, dvb::json::array({ light }),
		dvb::json{ { "search", { { "complete", true } } }, { "traversal", { { "complete", false } } } });
	held.AddView("firstPerson", "WEAPON", true, true, dvb::json::array({ light }));
	dvb::json hand; std::move(held).WriteTo(hand);
	CHECK(hand["heldLightObservationVersion"] == 2); CHECK(hand["heldLightEntriesInScene"] == 2);
	CHECK(hand["heldLightsRendered"] == 2); CHECK(hand["visibleIlluminationProven"] == false);
	CHECK(hand["heldLightCoverage"][0]["traversalCoverage"] == "bounded-unique-pointer-subgraph");
	CHECK(hand["heldLightCoverage"][0]["graphCoverage"]["traversal"]["complete"] == false);
}
