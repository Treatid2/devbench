#include "test_framework.h"
#include "mcp_server.h"

#include <atomic>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

namespace
{
	// Own isolated loopback sockets only; never attach to a game or delete an
	// unknown session. A collision fails the fixture, not a foreign owner.
	constexpr int port = 37392;
	mcp::server::configuration Config()
	{
		mcp::server::configuration result;
		result.host = "127.0.0.1";
		result.port = port;
		result.threadpool_size = 2;
		return result;
	}
	void RequireStarted(mcp::server& server)
	{
		if (!server.start(false))
			throw std::runtime_error("owned startup fixture did not acquire its listener");
	}
	void CheckPublicOptions(mcp::server& server)
	{
		if (!server.is_running())
			throw std::runtime_error("public request requires the owned ready listener");
		httplib::Client client("127.0.0.1", port);
		client.set_connection_timeout(1, 0);
		client.set_read_timeout(2, 0);
		auto response = client.Options("/mcp");
		CHECK(response && response->status == 204);
	}
}

TEST_CASE("actual bound socket failure cannot report startup success; retry owns the freed port")
{
	httplib::Server blocker;
	if (!blocker.bind_to_port("127.0.0.1", port))
		throw std::runtime_error("startup fixture port already owned");
	mcp::server server(Config());
	CHECK(!server.start(false));
	CHECK(server.last_start_bind_failed());
	CHECK(!server.is_running());
	blocker.stop();
	RequireStarted(server);
	CHECK(!server.last_start_bind_failed());
	CHECK(server.is_running());
	CheckPublicOptions(server);
	server.stop();
	CHECK(!server.is_running());
	RequireStarted(server); // completed routes reused, not registered again
	CheckPublicOptions(server);
	server.stop();
}

TEST_CASE("actual HTTP queue constructor failure is rolled back before a bounded explicit retry")
{
	mcp::server server(Config());
	std::atomic<int> constructions{0};
	server.http()->new_task_queue = [&]() -> httplib::TaskQueue* {
		if (++constructions == 1) throw std::runtime_error("injected HTTP queue failure");
		return new httplib::ThreadPool(2);
	};
	CHECK(!server.start(false));
	CHECK(constructions == 1);
	CHECK(!server.is_running());
	CHECK(server.worker_entry_failures() == 1);
	RequireStarted(server); // exactly one fixture-owned retry, not production replay
	CHECK(constructions == 2);
	CHECK(server.is_running());
	CHECK(server.worker_entry_failures() == 1); // historical failure preserved
	CheckPublicOptions(server);
	server.stop();
}

TEST_CASE("null HTTP queue fails readiness without dereference or accepted session")
{
	mcp::server server(Config());
	server.http()->new_task_queue = []() -> httplib::TaskQueue* { return nullptr; };
	CHECK(!server.start(false));
	CHECK(!server.is_running());
	CHECK(!server.last_start_bind_failed());
	server.stop();
}

TEST_CASE("blocking API releases lifecycle ownership so an external owner can stop its exact listener")
{
	mcp::server server(Config());
	bool outcome = false;
	bool exception = false;
	std::thread blocking([&] {
		try { outcome = server.start(true); }
		catch (...) { exception = true; }
	});
	const auto deadline = std::chrono::steady_clock::now() + 5s;
	while (!server.is_running() && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(10ms);
	const bool ready = server.is_running();
	server.stop();
	blocking.join();
	CHECK(ready);
	CHECK(!exception);
	CHECK(outcome);
	CHECK(!server.is_running());
}

TEST_CASE("embedded stdio fails explicitly before admitting host-owned input work")
{
	mcp::server server(Config());
	CHECK_THROWS(server.start_stdio());
	CHECK(!server.is_running());
}

TEST_CASE("partial route registration is poisoned rather than appended again on retry")
{
	auto config = Config();
	config.msg_endpoint = "("; // regex construction fails after CORS insertion
	mcp::server server(config);
	std::atomic<int> queues{0};
	server.http()->new_task_queue = [&]() -> httplib::TaskQueue* {
		++queues;
		return new httplib::ThreadPool(2);
	};
	CHECK(!server.start(false));
	CHECK(!server.last_start_bind_failed());
	CHECK(!server.start(false)); // explicit finite test of retained poison
	CHECK(queues == 0);
	CHECK(!server.is_running());
	server.stop();
}

TEST_CASE("first startup worker constructor failure leaves no listener and permits one explicit retry")
{
	auto config = Config();
	std::atomic<int> creations{0};
	config.startup_thread_factory = [&](std::function<void()> body) {
		if (++creations == 1) throw std::runtime_error("injected maintenance creation failure");
		return std::thread(std::move(body));
	};
	mcp::server server(config);
	CHECK(!server.start(false));
	CHECK(creations == 1);
	CHECK(!server.is_running());
	CHECK(!server.last_start_bind_failed());
	RequireStarted(server);
	CHECK(creations == 3);
	CheckPublicOptions(server);
	server.stop();
}

TEST_CASE("second startup worker constructor failure joins the already-created maintenance worker")
{
	auto config = Config();
	std::atomic<int> creations{0};
	std::atomic<int> liveWorkers{0};
	config.startup_thread_factory = [&](std::function<void()> body) {
		if (++creations == 2) throw std::runtime_error("injected listener creation failure");
		return std::thread([&, body = std::move(body)] {
			++liveWorkers;
			body();
			--liveWorkers;
		});
	};
	mcp::server server(config);
	CHECK(!server.start(false));
	CHECK(creations == 2);
	CHECK(liveWorkers == 0); // source failure return happens AFTER original join
	CHECK(!server.is_running());
	RequireStarted(server);
	CHECK(creations == 4);
	CheckPublicOptions(server);
	server.stop();
	CHECK(liveWorkers == 0);
}
