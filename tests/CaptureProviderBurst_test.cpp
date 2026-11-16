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
	const json request{ { "action", "burst" }, { "requestId", "batch" },
		{ "count", 2 }, { "outputDir", "L:/captures" }, { "prefix", "frame" } };
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

TEST_CASE("capture provider burst: lost, malformed and error-shaped start replies retain exclusion")
{
	const json request{ { "action", "burst" }, { "requestId", "batch" },
		{ "count", 2 }, { "outputDir", "L:/captures" }, { "prefix", "frame" } };
	const json statusRequest{ { "action", "burstStatus" }, { "runId", 17 } };
	const json terminal{ { "done", true }, { "requestId", "batch" }, { "runId", 17 }, { "apiRequestId", "sequence" } };
	for (const auto& reply : { json(nullptr), json{ { "queued", true } }, json{ { "error", "response lost" } } }) {
		dvb::Capture::ProviderBurstRun run;
		int admitted = 0;
		auto status = terminal;
		status["runId"] = 18;
		const dvb::ToolExtensions::Entry provider{
			json::parse(R"({"inputSchema":{"properties":{"action":{"enum":["burst","burstStatus"]}}}})"),
			[&](const json& args, const dvb::ToolContext&) {
				if (args == statusRequest)
					return status;
				++admitted;
				CHECK(run.InFlight());
				if (reply.is_null())
					throw std::runtime_error("decode failure after admission");
				return reply;
			},
		};

		if (reply.is_null())
			CHECK_THROWS(run.Start("communityshaders", provider, request, {}, 50));
		else
			CHECK(run.Start("communityshaders", provider, request, {}, 50) == reply);
		CHECK(admitted == 1);
		CHECK(run.InFlight());
		CHECK_THROWS(run.Start("communityshaders", provider, request, {}, 51));
		CHECK(admitted == 1);
		run.ObserveTerminal("communityshaders", terminal, 50);
		run.ObserveTerminal("another-provider", terminal, 51);
		CHECK(run.Status("communityshaders", provider, statusRequest, {}) == status);
		CHECK(run.InFlight());

		if (reply.is_null()) {
			run.ObserveTerminal("communityshaders", terminal, 51);
		} else {
			status = terminal;
			CHECK(run.Status("communityshaders", provider, statusRequest, {}) == terminal);
		}
		CHECK(!run.InFlight());
	}
}

TEST_CASE("capture provider burst: deterministic CSX refusal leaves the start slot available")
{
	dvb::Capture::ProviderBurstRun run;
	const json request{ { "action", "burst" }, { "requestId", "batch" }, { "count", 541 },
		{ "everyNFrames", 400 }, { "outputDir", "L:/captures" }, { "prefix", "frame" },
		{ "manifestPath", "L:/captures/frames.JSON" }, { "scale", 1 }, { "excludeUi", true } };
	const json accepted{ { "queued", true }, { "done", false }, { "requestId", "batch" },
		{ "runId", 17 }, { "apiRequestId", "sequence" } };
	int calls = 0;
	const dvb::ToolExtensions::Entry provider{
		json::parse(R"({"inputSchema":{"properties":{"action":{"enum":["burst"]}}}})"),
		[&](const json& a_args, const dvb::ToolContext&) {
			CHECK(a_args == request);
			++calls;
			return accepted;
		},
	};
	for (const auto& invalid : {
			 json{ { "count", -1 } }, json{ { "count", 601 } }, json{ { "everyNFrames", 0 } },
			 json{ { "everyNFrames", 401 } }, json{ { "prefix", "../frame" } }, json{ { "prefix", "cOm1.PNG" } },
			 json{ { "prefix", "frame." } }, json{ { "prefix", std::string(129, 'a') } },
			 json{ { "outputDir", "captures" } }, json{ { "outputDir", std::string("L:/captures\0else", 16) } },
			 json{ { "manifestPath", "L:/captures/NUL.json" } }, json{ { "manifestPath", "L:/captures/frames.png" } },
			 json{ { "manifestPath", "frames.json" } }, json{ { "scale", 0.5 } }, json{ { "scale", "1" } },
			 json{ { "excludeUi", 1 } },
		 }) {
		auto args = request;
		args.update(invalid);
		CHECK_THROWS(run.Start("CommunityShaders", provider, args, {}, 50));
		CHECK(!run.InFlight());
		CHECK(calls == 0);
	}
	CHECK(run.Start("CommunityShaders", provider, request, {}, 50) == accepted);
	CHECK(run.InFlight());
	CHECK(calls == 1);
}
