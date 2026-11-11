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
