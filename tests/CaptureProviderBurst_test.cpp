#include "test_framework.h"
#include "CaptureProviderBurst.h"

using dvb::json;

TEST_CASE("capture provider burst: preserves request and asynchronous acceptance")
{
	const json request{
		{ "action", "burst" }, { "requestId", "batch-1" },
		{ "outputDir", "L:/captures" }, { "prefix", "frame" }, { "count", 4 },
	};
	const json accepted{
		{ "queued", true }, { "runId", 17 }, { "apiRequestId", "seq-1" },
		{ "done", false }, { "ok", false },
	};
	int calls = 0;
	const dvb::ToolExtensions::Entry provider{
		json::parse(R"({"inputSchema":{"properties":{"action":{"enum":["capture","burst","burstStatus"]}}}})"),
		[&](const json& args, const dvb::ToolContext& ctx) {
			++calls;
			CHECK(args == request);
			CHECK(ctx.clientId == "client-1");
			return accepted;
		},
	};

	CHECK(dvb::Capture::InvokeProviderBurst(provider, request, { "client-1" }) == accepted);
	CHECK(calls == 1);
}

TEST_CASE("capture provider burst: returns terminal status and provider errors intact")
{
	const json request{ { "action", "burstStatus" }, { "runId", 17 } };
	const json terminal{
		{ "runId", 17 }, { "apiRequestId", "seq-1" }, { "done", true },
		{ "ok", true }, { "manifest", "L:/captures/frames.json" },
	};
	const json expired{ { "error", "burst runId is not retained in this API session" } };
	for (const auto& reply : { terminal, expired }) {
		const dvb::ToolExtensions::Entry provider{
			json::parse(R"({"inputSchema":{"properties":{"action":{"enum":["capture","burstStatus"]}}}})"),
			[&](const json& args, const dvb::ToolContext&) {
				CHECK(args == request);
				return reply;
			},
		};
		CHECK(dvb::Capture::InvokeProviderBurst(provider, request, {}) == reply);
	}
}

TEST_CASE("capture provider burst: refuses an unadvertised action before invocation")
{
	const json request{ { "action", "burstStatus" }, { "runId", 17 } };
	int calls = 0;
	for (const char* descriptor : {
			 R"({})",
			 R"({"inputSchema":{"properties":{"action":{"enum":["capture","burst"]}}}})",
			 R"({"inputSchema":{"properties":{"action":{"enum":"burstStatus"}}}})",
		 }) {
		const dvb::ToolExtensions::Entry provider{
			json::parse(descriptor),
			[&](const json&, const dvb::ToolContext&) {
				++calls;
				return json::object();
			},
		};
		CHECK_THROWS(dvb::Capture::InvokeProviderBurst(provider, request, {}));
	}
	CHECK(calls == 0);
}

TEST_CASE("capture provider burst: time-scale guard persists until matching terminal proof")
{
	dvb::Capture::ProviderBurstRun run;
	const json request{ { "action", "burst" }, { "requestId", "batch" } };
	const json statusRequest{ { "action", "burstStatus" }, { "runId", 17 } };
	json active{ { "queued", true }, { "done", false }, { "requestId", "batch" },
		{ "runId", 17 }, { "apiRequestId", "seq-1" } };
	auto terminal = active;
	terminal["queued"] = false;
	terminal["done"] = true;
	json status = active;
	int starts = 0;
	const dvb::ToolExtensions::Entry provider{
		json::parse(R"({"inputSchema":{"properties":{"action":{"enum":["burst","burstStatus"]}}}})"),
		[&](const json& args, const dvb::ToolContext&) {
			CHECK(run.InFlight());
			if (args == statusRequest)
				return status;
			CHECK(args == request);
			++starts;
			return active;
		},
	};

	CHECK(run.Start("communityshaders", provider, request, {}, 50) == active);
	CHECK(run.InFlight());
	CHECK(run.EventCursor() == 50);
	CHECK_THROWS(run.Start("communityshaders", provider, request, {}, 51));
	CHECK(starts == 1);
	CHECK(run.Status("communityshaders", provider, statusRequest, {}) == active);
	CHECK(run.InFlight());
	run.ObserveTerminal("communityshaders", terminal, 50);
	run.ObserveTerminal("another-provider", terminal, 51);
	CHECK(run.InFlight());

	status = terminal;
	status["apiRequestId"] = "other-sequence";
	CHECK(run.Status("communityshaders", provider, statusRequest, {}) == status);
	CHECK(run.InFlight());
	status = terminal;
	CHECK(run.Status("CommunityShaders", provider, statusRequest, {}) == terminal);
	CHECK(!run.InFlight());

	CHECK(run.Start("communityshaders", provider, request, {}, 60) == active);
	run.ObserveTerminal("communityshaders", terminal, 61);
	CHECK(!run.InFlight());
}

TEST_CASE("capture provider burst: cropped start refuses before invoking the provider")
{
	dvb::Capture::ProviderBurstRun run;
	int calls = 0;
	const dvb::ToolExtensions::Entry provider{
		json::parse(R"({"inputSchema":{"properties":{"action":{"enum":["burst"]}}}})"),
		[&](const json&, const dvb::ToolContext&) {
			++calls;
			return json::object();
		},
	};
	const json cropped{ { "action", "burst" }, { "requestId", "batch" },
		{ "subrect", { { "x", 0 }, { "y", 0 }, { "w", 0.5 }, { "h", 0.5 } } } };
	CHECK_THROWS(run.Start("communityshaders", provider, cropped, {}, 0));
	CHECK(calls == 0);
	CHECK(!run.InFlight());
}
