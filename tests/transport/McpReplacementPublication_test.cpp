#include "test_framework.h"
#include "EventBus.h"
#include "McpAdapter.h"
#include "ToolRegistry.h"
#include "mcp_server.h"

#include <atomic>
#include <future>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

namespace dvb
{
	struct ToolRegistryPublicationFixture
	{
		static void WrapInstalled(ToolRegistry& registry, ToolRegistry::RegistrationListener before)
		{
			std::lock_guard lock(registry.m_mutex);
			auto actual = registry.m_onRegister;
			if (!actual) throw std::runtime_error("actual adapter listener missing");
			registry.m_onRegister = [before = std::move(before), actual = std::move(actual)](const ToolDescriptor& descriptor) {
				before(descriptor); // Register calls this after releasing its commit lock.
				actual(descriptor);
			};
		}
	};
}

namespace
{
	constexpr int port = 37394;
	using Json = mcp::json;
	bool SameSchema(const Json& actual, const Json& expected)
	{
		// MCP uses ordered_json; the registry uses json. Object-member order is
		// not schema content. Canonicalize objects, without changing array order,
		// fields or values, before comparing the public catalogue with its source.
		return dvb::json(actual) == dvb::json(expected);
	}
	void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
	Json Request(const char* method, Json params = Json::object())
	{
		return { { "jsonrpc", "2.0" }, { "id", 1 }, { "method", method }, { "params", std::move(params) } };
	}
	struct Fixture
	{
		std::atomic<bool> ready{ true };
		dvb::ToolRegistry registry;
		dvb::EventBus events;
		mcp::server server;
		dvb::McpAdapter adapter;
		Fixture() : server(Config()), adapter(registry, events, server, ready)
		{
			adapter.Wire();
			Require(server.start(false), "replacement fixture did not acquire owned listener");
		}
		~Fixture() { server.stop(); }
		Fixture(const Fixture&) = delete;
		Fixture& operator=(const Fixture&) = delete;
		mcp::server::configuration Config()
		{
			mcp::server::configuration c;
			c.host = "127.0.0.1"; c.port = port; c.threadpool_size = 2;
			c.request_admission = [this]() noexcept { return ready.load(); };
			return c;
		}
		httplib::Result Post(const std::string& session, const Json& body)
		{
			httplib::Client client("127.0.0.1", port);
			client.set_connection_timeout(1, 0); client.set_read_timeout(2, 0);
			httplib::Headers headers{ { "Accept", "application/json" } };
			if (!session.empty()) headers.emplace("Mcp-Session-Id", session);
			return client.Post("/mcp", headers, body.dump(), "application/json");
		}
	};
	struct ReleaseOnExit
	{
		std::promise<void>& release;
		~ReleaseOnExit() { try { release.set_value(); } catch (...) {} }
	};
}

TEST_CASE("catalogue schema comparison ignores object member order but preserves schema content")
{
	const Json expected{ { "type", "object" }, { "properties", { { "value", { { "type", "integer" } } } } } };
	const Json registryRoundtrip = dvb::json(expected);
	CHECK(registryRoundtrip.dump() != expected.dump()); // Deliberately different serialization order.
	CHECK(SameSchema(registryRoundtrip, expected));
	auto older = registryRoundtrip;
	older["properties"]["value"]["type"] = "string";
	CHECK(!SameSchema(older, expected));
	auto missing = registryRoundtrip;
	missing.erase("properties");
	CHECK(!SameSchema(missing, expected));
	const Json positional{ { "type", "array" }, { "prefixItems", Json::array({ Json{ { "type", "integer" } }, Json{ { "type", "string" } } }) } };
	auto reversed = positional;
	reversed["prefixItems"] = Json::array({ positional["prefixItems"][1], positional["prefixItems"][0] });
	CHECK(!SameSchema(reversed, positional));
}

TEST_CASE("same-name replacement with inverted real adapter callbacks converges to final registry generation")
{
	Fixture f;
	std::promise<void> olderCommitted, releaseOlder;
	auto entered = olderCommitted.get_future();
	auto release = releaseOlder.get_future().share();
	std::atomic<unsigned> workerErrors{ 0 };
	dvb::ToolRegistryPublicationFixture::WrapInstalled(f.registry, [&](const dvb::ToolDescriptor& descriptor) {
		if (descriptor.description == "older") {
			olderCommitted.set_value();
			if (release.wait_for(5s) != std::future_status::ready)
				throw std::runtime_error("bounded inversion fixture release expired");
		}
	});
	std::jthread older;
	ReleaseOnExit unblock{ releaseOlder }; // Releases before older's destructor joins on any failure.
	older = std::jthread([&] {
		try {
			f.registry.Register({ "replace", "older", { { "type", "object" }, { "properties", { { "value", { { "type", "string" } } } } } } },
				[](const dvb::json&, const dvb::ToolContext&) { return dvb::json{ { "generation", "older" } }; });
		} catch (...) { ++workerErrors; }
	});
	Require(entered.wait_for(3s) == std::future_status::ready, "older registration did not commit before its callback barrier");
	const Json finalSchema{ { "type", "object" }, { "properties", { { "value", { { "type", "integer" } } } } } };
	CHECK(!f.registry.Register({ "replace", "final", finalSchema },
		[](const dvb::json& arguments, const dvb::ToolContext&) {
			return dvb::json{ { "generation", "final" }, { "value", arguments.at("value").get<int>() } };
		})); // The later commit AND its actual adapter callback finish first.
	releaseOlder.set_value();
	older.join(); // The delayed older actual callback runs last.
	CHECK(workerErrors == 0);
	const auto current = f.registry.Describe("replace");
	Require(current.has_value(), "final registry entry missing");
	CHECK(current->description == "final"); CHECK(current->inputSchema == finalSchema);
	auto initialize = f.Post("", Request("initialize", { { "protocolVersion", "2025-03-26" },
		{ "capabilities", Json::object() }, { "clientInfo", { { "name", "replacement-fixture" }, { "version", "1" } } } }));
	Require(initialize && initialize->status == 200, "owned initialize failed");
	const auto session = initialize->get_header_value("Mcp-Session-Id");
	Require(!session.empty(), "owned session missing");
	auto initialized = f.Post(session, { { "jsonrpc", "2.0" }, { "method", "notifications/initialized" } });
	Require(initialized && initialized->status == 202, "owned initialized notification failed");
	auto list = f.Post(session, Request("tools/list"));
	Require(list && list->status == 200, "final public catalogue unavailable");
	const auto tools = Json::parse(list->body).at("result").at("tools");
	Require(tools.size() == 1, "unexpected catalogue fixture inventory");
	CHECK(tools.at(0).at("name") == "replace");
	CHECK(tools.at(0).at("description") == "final");
	const auto& publishedSchema = tools.at(0).at("inputSchema");
	CHECK_MESSAGE(SameSchema(publishedSchema, finalSchema),
		"catalogue inputSchema differs: actual=" + publishedSchema.dump() + "; expected=" + finalSchema.dump());
	auto call = f.Post(session, Request("tools/call", { { "name", "replace" }, { "arguments", { { "value", 42 } } } }));
	Require(call && call->status == 200, "final public callable unavailable");
	const auto result = Json::parse(call->body).at("result");
	CHECK(result.at("isError") == false);
	const auto value = Json::parse(result.at("content").at(0).at("text").get<std::string>());
	CHECK(value.at("generation") == "final"); CHECK(value.at("value") == 42);
}
