#include "test_framework.h"
#include "EventBus.h"
#include "McpAdapter.h"
#include "PostLoadStartup.h"
#include "ToolRegistry.h"
#include "mcp_server.h"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <unordered_set>

using namespace std::chrono_literals;

namespace
{
	constexpr int port = 37393;
	using Json = mcp::json;
	Json Request(const char* method, Json params = Json::object())
	{
		return { { "jsonrpc", "2.0" }, { "id", 1 }, { "method", method }, { "params", std::move(params) } };
	}
	Json Initialize()
	{
		return Request("initialize", { { "protocolVersion", "2025-03-26" },
			{ "capabilities", Json::object() }, { "clientInfo", { { "name", "owned-admission-fixture" }, { "version", "1" } } } });
	}
	Json Initialized() { return { { "jsonrpc", "2.0" }, { "method", "notifications/initialized" } }; }
	void Require(bool condition, const char* failure)
	{
		if (!condition) throw std::runtime_error(failure);
	}
	class Fixture
	{
	public:
		Fixture() : server(Config()), adapter(registry, events, server, ready)
		{
			server.register_session_cleanup("owned-admission-fixture", [this](const std::string&) { ++cleanups; });
		}
		~Fixture() { server.stop(); } // Transport drains while every captured owner is alive.
		Fixture(const Fixture&) = delete;
		Fixture& operator=(const Fixture&) = delete;
		mcp::server::configuration Config()
		{
			mcp::server::configuration c;
			c.host = "127.0.0.1"; c.port = port; c.threadpool_size = 4; c.max_sessions = 1;
			c.request_admission = [this]() noexcept { return ready.load(); };
			return c;
		}
		void Prepare()
		{
			registry.Register({ "echo", "owned echo", dvb::DefaultInputSchema(), true },
				[](const dvb::json& args, const dvb::ToolContext& context) {
					return dvb::json{ { "args", args }, { "session", context.clientId } };
				});
			adapter.Wire();
		}
		void Start() { Require(server.start(false), "fixture did not acquire its own listener"); }
		httplib::Result Post(const std::string& session, const Json& body)
		{
			httplib::Client client("127.0.0.1", port);
			client.set_connection_timeout(1, 0); client.set_read_timeout(2, 0);
			httplib::Headers headers{ { "Accept", "application/json" } };
			if (!session.empty()) headers.emplace("Mcp-Session-Id", session);
			return client.Post("/mcp", headers, body.dump(), "application/json");
		}
		std::string Admit()
		{
			auto reply = Post("", Initialize());
			Require(reply && reply->status == 200, "committed initialize refused");
			const auto session = reply->get_header_value("Mcp-Session-Id");
			Require(!session.empty(), "initialize omitted its owned session");
			Require(Json::parse(reply->body).contains("result"), "initialize returned a protocol error");
			auto initialized = Post(session, Initialized());
			Require(initialized && initialized->status == 202, "committed initialized notification refused");
			return session;
		}
		void CheckClosed(const std::string& session = "")
		{
			for (const auto& request : { Initialize(), Request("tools/list"),
				Request("tools/call", { { "name", "echo" } }), Initialized() }) {
				auto reply = Post(session, request);
				Require(reply && reply->status == 503, "uncommitted public request was not refused");
				CHECK(reply->get_header_value("Mcp-Session-Id").empty());
				CHECK(Json::parse(reply->body).at("error") == "Server not ready");
			}
			httplib::Client client("127.0.0.1", port);
			client.set_connection_timeout(1, 0); client.set_read_timeout(2, 0);
			for (const auto* path : { "/sse", "/mcp" }) {
				auto reply = client.Get(path, httplib::Headers{ { "Mcp-Session-Id", session } });
				Require(reply && reply->status == 503, "uncommitted SSE request was not refused");
			}
			auto legacy = client.Post("/message?session_id=" + session, Initialized().dump(), "application/json");
			Require(legacy && legacy->status == 503, "uncommitted legacy POST was not refused");
			auto removal = client.Delete("/mcp", httplib::Headers{ { "Mcp-Session-Id", session } });
			Require(removal && removal->status == 503, "quarantined client deletion was not refused");
			CHECK(cleanups == 0);
		}
		std::atomic<bool> ready{ false };
		std::atomic<unsigned> cleanups{ 0 };
		dvb::ToolRegistry registry;
		dvb::EventBus events;
		mcp::server server;
		dvb::McpAdapter adapter;
	};
	struct Operations
	{
		Fixture& f;
		bool failPublication;
		bool Configure() { return true; }
		void Construct() {} // The same retained fixture owns the token and transport.
		void Prepare() { f.Prepare(); }
		bool Listen() { f.Start(); f.CheckClosed(); return true; }
		void Publish()
		{
			if (failPublication) throw std::runtime_error("injected publication failure after listening");
			f.ready.store(true);
		}
		void Quarantine() noexcept { f.ready.store(false); }
		void Report(dvb::PostLoadStartup::Outcome, dvb::PostLoadStartup::Phase, std::exception_ptr) noexcept {}
	};
}

TEST_CASE("actual PostLoad sequence admits public MCP only after publication and revokes retained IDs")
{
	Fixture f;
	Operations operations{ f, false };
	dvb::PostLoadStartup startup;
	CHECK(startup.Run(operations) == dvb::PostLoadStartup::Outcome::Ready);
	Require(startup.Ready(), "PostLoad did not commit");
	const auto session = f.Admit(); // Refused pre-commit requests did not consume the single slot.
	auto catalogue = f.Post(session, Request("tools/list"));
	Require(catalogue && catalogue->status == 200, "committed catalogue unavailable");
	CHECK(Json::parse(catalogue->body).at("result").at("tools").size() == 1);
	f.ready.store(false);
	f.CheckClosed(session);
	CHECK(startup.Run(operations) == dvb::PostLoadStartup::Outcome::AlreadyAttempted);
	CHECK(!f.ready.load());
	f.server.stop();
	CHECK(f.cleanups == 1); // Normal owned drain, not client/foreign DELETE.
}

TEST_CASE("failed PostLoad publication leaves public native routes quarantined without a new session")
{
	Fixture f;
	Operations operations{ f, true };
	dvb::PostLoadStartup startup;
	CHECK(startup.Run(operations) == dvb::PostLoadStartup::Outcome::Failed);
	CHECK(startup.Current() == dvb::PostLoadStartup::State::Failed);
	CHECK(!f.ready.load());
	f.CheckClosed();
	CHECK(startup.Run(operations) == dvb::PostLoadStartup::Outcome::AlreadyAttempted);
	f.server.stop();
	CHECK(f.cleanups == 0);
}

TEST_CASE("real adapter late registration coexists with public catalogue and exact callable dispatch")
{
	Fixture f;
	f.Prepare(); f.Start(); f.ready.store(true);
	const auto session = f.Admit();
	constexpr unsigned registrations = 160;
	constexpr unsigned reads = 80;
	std::atomic<unsigned> errors{ 0 }, catalogues{ 0 }, calls{ 0 }, registered{ 0 };
	const auto deadline = std::chrono::steady_clock::now() + 20s;
	// No CHECK on worker threads: the harness counter is deliberately non-atomic.
	std::jthread producer([&] {
		try {
			for (unsigned i = 0; i < registrations; ++i) {
				Require(std::chrono::steady_clock::now() < deadline, "registration budget exceeded");
				const auto name = "late-" + std::to_string(i);
				f.registry.Register({ name, "late consumer", dvb::DefaultInputSchema(), true },
					[name](const dvb::json&, const dvb::ToolContext&) { return dvb::json{ { "name", name } }; });
				++registered;
				std::this_thread::sleep_for(1ms);
			}
		} catch (...) { ++errors; }
	});
	std::jthread reader([&] {
		try {
			std::size_t previous = 1;
			for (unsigned i = 0; i < reads; ++i) {
				Require(std::chrono::steady_clock::now() < deadline, "catalogue budget exceeded");
				auto reply = f.Post(session, Request("tools/list"));
				Require(reply && reply->status == 200, "concurrent catalogue refused");
				const auto tools = Json::parse(reply->body).at("result").at("tools");
				Require(tools.is_array() && tools.size() >= previous, "catalogue was not a monotonic snapshot");
				std::unordered_set<std::string> names;
				for (const auto& tool : tools) {
					const auto name = tool.at("name").get<std::string>();
					Require(names.insert(name).second, "duplicate catalogue name");
					Require(name == "echo" || name.starts_with("late-"), "unexpected catalogue name");
					Require(tool.at("inputSchema").at("type") == "object", "partial descriptor snapshot");
				}
				Require(names.contains("echo"), "base tool vanished");
				previous = tools.size(); ++catalogues;
			}
		} catch (...) { ++errors; }
	});
	std::jthread caller([&] {
		try {
			for (unsigned i = 0; i < reads; ++i) {
				Require(std::chrono::steady_clock::now() < deadline, "call budget exceeded");
				const Json arguments{ { "iteration", i } };
				auto reply = f.Post(session, Request("tools/call", { { "name", "echo" }, { "arguments", arguments } }));
				Require(reply && reply->status == 200, "concurrent callable refused");
				const auto result = Json::parse(reply->body).at("result");
				Require(result.at("isError") == false, "adapter call failed");
				const auto value = Json::parse(result.at("content").at(0).at("text").get<std::string>());
				Require(value.at("args") == arguments && value.at("session") == session, "dispatch provenance changed");
				const auto published = registered.load();
				if (published != 0) {
					const auto name = "late-" + std::to_string(published - 1);
					auto late = f.Post(session, Request("tools/call", { { "name", name } }));
					Require(late && late->status == 200, "published late callable unavailable");
					const auto lateResult = Json::parse(late->body).at("result");
					Require(lateResult.at("isError") == false, "published late callable failed");
					const auto lateValue = Json::parse(lateResult.at("content").at(0).at("text").get<std::string>());
					Require(lateValue.at("name") == name, "wrong late callable selected");
				}
				++calls;
			}
		} catch (...) { ++errors; }
	});
	producer.join(); reader.join(); caller.join();
	CHECK(errors == 0); CHECK(registered == registrations); CHECK(catalogues == reads); CHECK(calls == reads);
	auto catalogue = f.Post(session, Request("tools/list"));
	Require(catalogue && catalogue->status == 200, "final catalogue unavailable");
	CHECK(Json::parse(catalogue->body).at("result").at("tools").size() == registrations + 1);
	// Reentrant registration from a real tool proves application code runs after
	// unlocking cpp-mcp's catalogue mutex. No detached timeout worker can hide a deadlock.
	f.registry.Register({ "register-from-call", "reentrant registration" },
		[&](const dvb::json&, const dvb::ToolContext&) {
			f.registry.Register({ "registered-from-call", "nested registration" },
				[](const dvb::json&, const dvb::ToolContext&) { return dvb::json(true); });
			return dvb::json(true);
		});
	auto nested = f.Post(session, Request("tools/call", { { "name", "register-from-call" } }));
	Require(nested && nested->status == 200, "reentrant registration did not complete");
	CHECK(Json::parse(nested->body).at("result").at("isError") == false);
	CHECK(f.server.get_tools().size() == registrations + 3);
}
