#include "test_framework.h"
#include "mcp_server.h"

#include <atomic>
#include <future>
#include <stdexcept>

using namespace std::chrono_literals;

namespace {
// A separate host-only server. Never point this fixture at a game endpoint.
// Fail on port collision before creating any session on an unknown listener.
class Fixture {
public:
    explicit Fixture(unsigned timeout) : server(config(timeout)) {
        httplib::Client preflight("127.0.0.1", port);
        preflight.set_connection_timeout(0, 100000);
        if (preflight.Get("/")) throw std::runtime_error("transport test port occupied");
        server.register_session_cleanup("transport-test", [this](const std::string&) { ++cleanups; });
        server.register_method("test/status", [](const mcp::json&, const std::string& sid) {
            return mcp::json{{"session", sid}};
        });
        server.register_method("test/slow", [](const mcp::json&, const std::string& sid) {
            std::this_thread::sleep_for(35s);
            return mcp::json{{"session", sid}};
        });
        if (!server.start(false)) throw std::runtime_error("transport fixture failed to start");
        auto until = std::chrono::steady_clock::now() + 5s;
        while (!server.http()->is_running() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(10ms);
        if (!server.http()->is_running()) throw std::runtime_error("transport fixture listen deadline");
    }
    ~Fixture() { server.stop(); }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    std::string initialize() {
        auto result = post("", mcp::json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
            {"params", {{"protocolVersion", "2025-03-26"}, {"capabilities", mcp::json::object()},
                        {"clientInfo", {{"name", "devbench-transport-fixture"}, {"version", "1"}}}}}});
        if (!result || result->status != 200) throw std::runtime_error("initialize failed");
        auto sid = result->get_header_value("Mcp-Session-Id");
        if (sid.empty()) throw std::runtime_error("initialize omitted session");
        auto initialized = post(sid, mcp::json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
        if (!initialized || initialized->status != 202) throw std::runtime_error("initialized notification failed");
        return sid;
    }
    httplib::Result post(const std::string& sid, const mcp::json& body) {
        httplib::Client client("127.0.0.1", port);
        client.set_connection_timeout(1, 0);
        client.set_read_timeout(45, 0);
        httplib::Headers headers{{"Accept", "application/json"}};
        if (!sid.empty()) headers.emplace("Mcp-Session-Id", sid);
        return client.Post("/mcp", headers, body.dump(), "application/json");
    }
    httplib::Result remove(const std::string& sid) {
        httplib::Client client("127.0.0.1", port);
        client.set_connection_timeout(1, 0);
        client.set_read_timeout(2, 0);
        return client.Delete("/mcp", httplib::Headers{{"Mcp-Session-Id", sid}});
    }
    static mcp::json status(int id) {
        return {{"jsonrpc", "2.0"}, {"id", id}, {"method", "test/status"}};
    }
    static mcp::server::configuration config(unsigned timeout) {
        mcp::server::configuration c;
        c.host = "127.0.0.1"; c.port = port; c.session_timeout = timeout; c.threadpool_size = 2;
        return c;
    }
    static constexpr int port = 37391;
    std::atomic<unsigned> cleanups{0};
    mcp::server server;
};
}

TEST_CASE("POST-only same session survives 180 seconds and over 600 requests; DELETE stays final") {
    Fixture f(30); // Exactly the deployed timeout, not a larger workaround.
    auto sid = f.initialize();
    auto deadline = std::chrono::steady_clock::now() + 180s;
    unsigned count = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        bool notification = count % 11 == 0;
        bool batch = !notification && count % 13 == 0;
        mcp::json body = notification ? mcp::json{{"jsonrpc", "2.0"}, {"method", "notifications/test"}} : Fixture::status(2 + count);
        if (batch) body = mcp::json::array({body, Fixture::status(10000 + count)});
        auto reply = f.post(sid, body);
        if (!reply || reply->status != (notification ? 202 : 200))
            throw std::runtime_error("active POST-only session retired");
        if (!notification) {
            auto response = mcp::json::parse(reply->body);
            if (batch) {
                CHECK(response.size() == 2);
                for (const auto& item : response) CHECK(item.at("result").at("session") == sid);
            } else CHECK(response.at("result").at("session") == sid);
        }
        ++count;
        std::this_thread::sleep_for(200ms);
    }
    CHECK(count >= 600);
    CHECK(f.cleanups == 0);
    auto invalid = f.post("not-the-owned-session", Fixture::status(1999));
    CHECK(invalid && invalid->status == 404);
    auto removed = f.remove(sid);
    CHECK(removed && removed->status == 200);
    CHECK(f.cleanups == 1);
    auto absent = f.post(sid, Fixture::status(2000));
    CHECK(absent && absent->status == 404);
    auto removedAgain = f.remove(sid);
    CHECK(removedAgain && removedAgain->status == 404);
    CHECK(f.cleanups == 1); // No recreated session or duplicate cleanup.
}

TEST_CASE("admitted 35-second POST is not idle; its completion restarts the idle interval") {
    Fixture f(30);
    auto sid = f.initialize();
    auto slow = f.post(sid, mcp::json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "test/slow"}});
    CHECK(slow && slow->status == 200);
    CHECK(f.cleanups == 0);
    auto stillPresent = f.post(sid, Fixture::status(3));
    CHECK(stillPresent && stillPresent->status == 200);
    auto removed = f.remove(sid);
    CHECK(removed && removed->status == 200);
    CHECK(f.cleanups == 1);
}

TEST_CASE("genuinely idle session retires and cleanup happens once") {
    Fixture f(1); // Exercise the real maintenance route without a game or a 40s wait.
    auto sid = f.initialize();
    auto until = std::chrono::steady_clock::now() + 15s;
    while (f.cleanups == 0 && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(50ms); // No RPC polling that could refresh activity.
    CHECK(f.cleanups == 1);
    auto absent = f.post(sid, Fixture::status(2));
    CHECK(absent && absent->status == 404);
    auto removed = f.remove(sid);
    CHECK(removed && removed->status == 404);
    CHECK(f.cleanups == 1);
}

TEST_CASE("dispatcher retirement is atomic with activity admission and guard exceptional cleanup") {
    auto d = std::make_shared<mcp::event_dispatcher>();
    auto farFuture = std::chrono::steady_clock::now() + 1h;
    try {
        mcp::session_request_guard guard;
        CHECK(guard.acquire(d));
        CHECK(!d->close_if_inactive(farFuture, 30s)); // Cannot retire an in-flight request.
        throw std::runtime_error("exceptional POST exit");
    } catch (const std::runtime_error&) {}
    CHECK(!d->close_if_inactive(std::chrono::steady_clock::now(), 30s)); // Fresh completion.
    CHECK(d->close_if_inactive(farFuture, 30s));
    mcp::session_request_guard late;
    CHECK(!late.acquire(d)); // A selected retirement cannot be revived by a racing POST.
    CHECK(!d->close_if_inactive(farFuture, 30s)); // Single retirement.
}
