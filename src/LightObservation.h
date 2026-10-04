#pragma once

#include "Json.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace dvb::LightObservation
{
	// Shared by all views, conventional-node searches, caster lights and renderer
	// list reads in ONE public observation. These are work limits, not a deadline,
	// pointer-validity guarantee or cancellation of an engine call already entered.
	struct Limits
	{
		std::size_t nodes = 4096, edges = 16384, parents = 32768;
		std::size_t rendererEntries = 4096, uniqueLights = 512, outputs = 512;
		std::size_t depth = 256, ancestors = 64;
	};
	struct Budget
	{
		Limits limits;
		std::size_t nodes = 0, edges = 0, parents = 0, rendererEntries = 0, outputs = 0;
		std::unordered_set<const void*> lights;
		std::set<std::string> reasons;

		bool Take(std::size_t& a_used, std::size_t a_cap, const char* a_reason)
		{
			if (a_used >= a_cap) {
				reasons.emplace(a_reason);
				return false;
			}
			++a_used;
			return true;
		}
		bool TrackLight(const void* a_light)
		{
			if (lights.contains(a_light))
				return true;
			if (lights.size() >= limits.uniqueLights) {
				reasons.emplace("unique-light-budget");
				return false;
			}
			lights.insert(a_light);
			return true;
		}
		bool Emit(const void* a_light)
		{
			return TrackLight(a_light) && Take(outputs, limits.outputs, "output-budget");
		}
		json Fields() const
		{
			return json{
				{ "complete", reasons.empty() }, { "reasons", reasons },
				{ "used", { { "nodes", nodes }, { "edges", edges }, { "parents", parents },
					{ "rendererEntries", rendererEntries }, { "uniqueLights", lights.size() }, { "outputs", outputs } } },
				{ "limits", { { "nodes", limits.nodes }, { "edges", limits.edges }, { "parents", limits.parents },
					{ "rendererEntries", limits.rendererEntries }, { "uniqueLights", limits.uniqueLights },
					{ "outputs", limits.outputs }, { "depth", limits.depth }, { "ancestors", limits.ancestors } } }
			};
		}
	};
	struct WalkReport
	{
		bool available = false, complete = true, stoppedAfterMatch = false;
		std::size_t nodes = 0, edges = 0, repeats = 0;
		std::set<std::string> reasons;
		void Cut(Budget& a_budget, const char* a_reason)
		{
			complete = false;
			reasons.emplace(a_reason);
			a_budget.reasons.emplace(a_reason);
		}
		json Fields() const
		{
			return json{ { "available", available }, { "complete", available && complete },
				{ "stoppedAfterMatch", stoppedAfterMatch }, { "visitedNodes", nodes },
				{ "examinedEdges", edges }, { "repeatedPointers", repeats }, { "reasons", reasons } };
		}
	};

	// Iterative depth-first search, child slots read lazily. A wide node cannot
	// allocate an unbounded pending-child vector before the edge budget is checked.
	// Identity visitation is local to each search: a later subtree/view observation
	// may legitimately revisit a pointer, but still consumes the shared budget.
	template <class Pointer, class Children, class Visitor>
	WalkReport Walk(Pointer a_root, Budget& a_budget, Children a_children, Visitor a_visit)
	{
		WalkReport report;
		report.available = a_root != nullptr;
		if (!a_root)
			return report;
		struct Frame { Pointer object; std::size_t next = 0, count = 0, depth = 0; bool entered = false; };
		std::vector<Frame> stack{ { a_root } };
		std::unordered_set<Pointer> visited;
		while (!stack.empty()) {
			auto& frame = stack.back();
			if (!frame.entered) {
				if (!frame.object) { stack.pop_back(); continue; }
				if (visited.contains(frame.object)) { ++report.repeats; stack.pop_back(); continue; }
				if (!a_budget.Take(a_budget.nodes, a_budget.limits.nodes, "node-budget")) {
					report.Cut(a_budget, "node-budget"); break;
				}
				visited.insert(frame.object);
				++report.nodes;
				frame.entered = true;
				// Visitor returns true only for a completed first-match search.
				if (a_visit(frame.object)) { report.stoppedAfterMatch = true; break; }
				frame.count = a_children.Count(frame.object);
				if (frame.depth >= a_budget.limits.depth && frame.count != 0) {
					report.Cut(a_budget, "depth-budget"); stack.pop_back(); continue;
				}
			}
			if (frame.next == frame.count) { stack.pop_back(); continue; }
			if (!a_budget.Take(a_budget.edges, a_budget.limits.edges, "edge-budget")) {
				report.Cut(a_budget, "edge-budget"); break;
			}
			++report.edges;
			auto child = a_children.Child(frame.object, frame.next++);
			const auto depth = frame.depth + 1;
			stack.push_back({ child, 0, 0, depth });
		}
		return report;
	}

	template <class Pointer, class Parent, class Visitor>
	WalkReport Parents(Pointer a_start, Budget& a_budget, Parent a_parent, Visitor a_visit)
	{
		WalkReport report;
		report.available = a_start != nullptr;
		std::unordered_set<Pointer> visited;
		for (auto p = a_start; p; p = a_parent(p)) {
			if (visited.contains(p)) { report.Cut(a_budget, "parent-cycle"); break; }
			if (report.nodes >= a_budget.limits.ancestors) { report.Cut(a_budget, "ancestor-budget"); break; }
			if (!a_budget.Take(a_budget.parents, a_budget.limits.parents, "parent-budget")) {
				report.Cut(a_budget, "parent-budget"); break;
			}
			visited.insert(p);
			++report.nodes;
			if (a_visit(p)) { report.stoppedAfterMatch = true; break; }
		}
		return report;
	}

	struct Request
	{
		bool scene = false, selected = false;
		std::string formId;
		double radius = 0;
		int limit = 100;
	};
	inline Request ParseRequest(const json& a_args)
	{
		if (!a_args.is_object()) throw std::invalid_argument("arguments must be an object");
		Request request;
		if (a_args.contains("scope")) {
			const auto& scope = a_args["scope"];
			if (!scope.is_string() || (scope != "ref" && scope != "scene"))
				throw std::invalid_argument("scope must be 'ref' or 'scene'");
			request.scene = scope == "scene";
		}
		// Even false/empty reference selectors are rejected in scene scope. Presence
		// is the contract; no ignored selector can imply a different observation.
		if (request.scene && (a_args.contains("formId") || a_args.contains("selected")))
			throw std::invalid_argument("scene scope forbids formId and selected");
		if (a_args.contains("formId")) {
			if (!a_args["formId"].is_string()) throw std::invalid_argument("formId must be a string");
			request.formId = a_args["formId"].get<std::string>();
		}
		if (a_args.contains("selected")) {
			if (!a_args["selected"].is_boolean()) throw std::invalid_argument("selected must be a boolean");
			request.selected = a_args["selected"].get<bool>();
		}
		if (request.selected && !request.formId.empty())
			throw std::invalid_argument("selected and formId are mutually exclusive");
		if (a_args.contains("radius")) {
			if (!a_args["radius"].is_number()) throw std::invalid_argument("radius must be a number");
			request.radius = a_args["radius"].get<double>();
			if (!std::isfinite(request.radius) || request.radius < 0)
				throw std::invalid_argument("radius must be finite and >= 0");
		}
		if (a_args.contains("limit")) {
			const auto& limit = a_args["limit"];
			if (!limit.is_number_integer() || limit.get<double>() < 0 ||
				limit.get<double>() > std::numeric_limits<int>::max())
				throw std::invalid_argument("limit must be an integer in 0..INT_MAX");
			request.limit = limit.get<int>();
		}
		return request;
	}
	using Position = std::array<double, 3>;
	inline bool Finite(const Position& a_position)
	{
		return std::isfinite(a_position[0]) && std::isfinite(a_position[1]) && std::isfinite(a_position[2]);
	}
	inline std::optional<double> Distance(const std::optional<Position>& a_origin, const Position& a_light)
	{
		if (!a_origin || !Finite(*a_origin) || !Finite(a_light)) return std::nullopt;
		const auto distance = std::hypot((*a_origin)[0] - a_light[0], (*a_origin)[1] - a_light[1], (*a_origin)[2] - a_light[2]);
		return std::isfinite(distance) ? std::optional<double>(distance) : std::nullopt;
	}
	struct SortKey
	{
		std::optional<double> distance;
		std::uint32_t owner = 0;
		std::string path, name, type;
		std::uintptr_t pointer = 0;  // final tie-break only; stable within this process
		bool operator<(const SortKey& a_other) const
		{
			return std::tuple(distance.value_or(std::numeric_limits<double>::infinity()), owner, path, name, type, pointer) <
				std::tuple(a_other.distance.value_or(std::numeric_limits<double>::infinity()), a_other.owner, a_other.path, a_other.name, a_other.type, a_other.pointer);
		}
	};
	inline std::string BoundedName(const char* a_name, Budget& a_budget)
	{
		if (!a_name) return {};
		constexpr std::size_t cap = 256;
		std::size_t length = 0;
		while (length < cap && a_name[length]) ++length;
		if (length == cap) {
			a_budget.reasons.emplace("name-length-budget");
			// Do not introduce an incomplete UTF-8 character when clipping an
			// otherwise valid engine name. Existing invalid encodings are not repaired.
			std::size_t lead = length - 1;
			while (lead > 0 && (static_cast<unsigned char>(a_name[lead]) & 0xC0) == 0x80) --lead;
			const auto byte = static_cast<unsigned char>(a_name[lead]);
			const std::size_t width = byte >= 0xF0 ? 4 : byte >= 0xE0 ? 3 : byte >= 0xC0 ? 2 : 1;
			if (length - lead < width) length = lead;
		}
		return std::string(a_name, length);
	}
}
