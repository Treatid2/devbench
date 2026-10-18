#include "test_framework.h"
#include "mcp_server.h"

#include <atomic>
#include <future>
#include <latch>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

namespace dvb {
// Friend-only qualification seam into the actual production table/dispatcher.
// Never starts a listener, sends HTTP, or connects to a game/native service.
struct McpSessionRetirementFixture {
    static mcp::server::configuration Config(unsigned timeout) {
        mcp::server::configuration c;
        c.threadpool_size = 1;
        c.session_timeout = timeout;
        c.max_sessions = 1;
        return c;
    }
    explicit McpSessionRetirementFixture(unsigned timeout = 30) : server(Config(timeout)) {
        server.running_ = true; // Enable only the production maintenance pass.
        server.session_admission_open_ = true;
        server.register_session_cleanup("owned", [this](const std::string&) {
            ++cleanups;
            (void)Count(); // Callback must be outside the owner lock.
        });
    }
    ~McpSessionRetirementFixture() { server.stop(); }
    std::shared_ptr<mcp::event_dispatcher> Insert(const std::string& id) {
        auto d = std::make_shared<mcp::event_dispatcher>();
        if (!AdmitSession(server.session_dispatchers_, server.mutex_, server.max_sessions_, id, d,
                          &server.session_admission_open_)) return {};
        SetRegisteredSessionInitialized(server.session_dispatchers_, server.session_initialized_,
                                         server.mutex_, id, true);
        return d;
    }
    std::size_t Count() {
        std::lock_guard lock(server.mutex_);
        return server.session_dispatchers_.size();
    }
    bool Contains(const std::string& id, const std::shared_ptr<mcp::event_dispatcher>& d) {
        std::lock_guard lock(server.mutex_);
        auto it = server.session_dispatchers_.find(id);
        return it != server.session_dispatchers_.end() && it->second == d;
    }
    void Age(const std::shared_ptr<mcp::event_dispatcher>& d) {
        std::lock_guard lock(d->m_);
        d->last_activity_ = std::chrono::steady_clock::now() - 1h;
    }
    void Sweep() { server.check_inactive_sessions(); }
    void Close(const std::string& id, const std::shared_ptr<mcp::event_dispatcher>& d, bool idle = false) {
        server.close_session(id, idle, d);
    }
    void Delete(const std::string& id) {
        httplib::Request req;
        httplib::Response res;
        res.status = 200; // HTTP routing normally supplies the successful default.
        req.headers.emplace("Mcp-Session-Id", id);
        server.handle_mcp_delete(req, res);
    }
    void Post(const std::string& id, const mcp::json& body, httplib::Response& res) {
        httplib::Request req;
        res.status = 200; // Invoke the handler without opening an HTTP listener.
        req.headers.emplace("Mcp-Session-Id", id);
        req.headers.emplace("Accept", "application/json");
        req.body = body.dump();
        server.handle_mcp_post(req, res);
    }
    void Replace(const std::string& id, const std::shared_ptr<mcp::event_dispatcher>& replacement) {
        std::lock_guard lock(server.mutex_);
        server.session_dispatchers_.at(id) = replacement;
    }
    std::atomic<unsigned> cleanups{0};
    mcp::server server;
};
}

namespace {
using Fixture = dvb::McpSessionRetirementFixture;
void SinkFailure(bool throws) {
    Fixture f(0); // Disabled healthy idle expiry must not disable closed cleanup.
    auto d = f.Insert("owned");
    if (!d) throw std::runtime_error("fixture admission failed");
    httplib::DataSink sink;
    sink.write = [throws](const char*, std::size_t) -> bool {
        if (throws) throw std::runtime_error("injected sink failure");
        return false;
    };
    CHECK(d->send_event("event: message\ndata: payload\n\n"));
    CHECK(!d->wait_event(&sink, 0ms));
    CHECK(d->is_closed());
    f.Sweep();
    CHECK(f.Count() == 0);
    CHECK(f.cleanups == 1);
    CHECK(f.Insert("slot-reused") != nullptr);
    f.Sweep();
    CHECK(f.Count() == 1); // Healthy registration with idle expiry disabled.
    CHECK(f.cleanups == 1);
}
}

TEST_CASE("MCP closed retirement: payload write false releases the actual capacity slot") {
    SinkFailure(false);
}

TEST_CASE("MCP closed retirement: payload write throw releases the actual capacity slot") {
    SinkFailure(true);
}

TEST_CASE("MCP closed retirement: closed active POST drains before cleanup and rejects late admission") {
    Fixture f;
    auto d = f.Insert("owned");
    std::promise<void> entered, release;
    auto enteredFuture = entered.get_future();
    auto releaseFuture = release.get_future().share();
    f.server.register_method("test/hold", [&](const mcp::json&, const std::string&) {
        entered.set_value();
        if (releaseFuture.wait_for(2s) != std::future_status::ready)
            throw std::runtime_error("bounded fixture release deadline");
        return mcp::json{{"finished", true}};
    });
    httplib::Response response;
    auto post = std::async(std::launch::async, [&] {
        f.Post("owned", {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "test/hold"}}, response);
    });
    const bool started = enteredFuture.wait_for(2s) == std::future_status::ready;
    CHECK(started);
    if (started) {
        d->close();
        f.Sweep();
        f.Delete("owned");
        f.Close("owned", d); // Provider/explicit close is also deferred.
        CHECK(f.Contains("owned", d));
        CHECK(f.cleanups == 0);
        mcp::session_request_guard late;
        CHECK(!late.acquire(d));
    }
    release.set_value();
    post.get();
    CHECK(response.status == 200);
    CHECK(mcp::json::parse(response.body).at("result").at("finished") == true);
    f.Sweep();
    CHECK(f.Count() == 0);
    CHECK(f.cleanups == 1);
}

TEST_CASE("MCP closed retirement: DELETE maintenance and provider have one exact cleanup winner") {
    Fixture f;
    auto d = f.Insert("owned");
    d->close();
    std::latch start(1);
    std::thread remove([&] { start.wait(); f.Delete("owned"); });
    std::thread maintain([&] { start.wait(); f.Sweep(); });
    std::thread provider([&] { start.wait(); f.Close("owned", d); });
    start.count_down();
    remove.join(); maintain.join(); provider.join();
    CHECK(f.Count() == 0);
    CHECK(f.cleanups == 1);
    auto replacement = f.Insert("owned");
    CHECK(replacement != nullptr);
    f.Close("owned", d);
    f.Close("owned", d, true);
    f.Close("foreign", replacement);
    CHECK(f.Contains("owned", replacement));
    CHECK(f.cleanups == 1);
}

TEST_CASE("MCP closed retirement: stale selected instance cannot close a replacement") {
    Fixture f;
    auto old = f.Insert("owned");
    old->close();
    auto replacement = std::make_shared<mcp::event_dispatcher>();
    f.Replace("owned", replacement);
    f.Close("owned", old, true);
    f.Close("owned", old);
    CHECK(f.Contains("owned", replacement));
    CHECK(!replacement->is_closed());
    CHECK(f.cleanups == 0);
}

TEST_CASE("MCP closed retirement: heartbeat and admitted work protect healthy sessions") {
    Fixture f;
    auto d = f.Insert("owned");
    f.Age(d);
    d->update_activity(); // Same production activity update used by GET heartbeat.
    f.Sweep();
    CHECK(f.Count() == 1);
    {
        mcp::session_request_guard request;
        CHECK(request.acquire(d));
        f.Age(d); // Deterministic long-request clock, not a shortened real timeout.
        f.Sweep();
        CHECK(f.Count() == 1);
        CHECK(!d->is_closed());
    }
    f.Sweep(); // Completion resets activity.
    CHECK(f.Count() == 1);
    f.Age(d);
    f.Sweep();
    CHECK(f.Count() == 0);
    CHECK(f.cleanups == 1);
}

TEST_CASE("MCP closed retirement: actual failed initialize rollback and atomic cap stay intact") {
    Fixture f;
    httplib::Response bad;
    f.Post("", {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                {"params", {{"protocolVersion", 7}}}}, bad);
    CHECK(bad.get_header_value("Mcp-Session-Id").empty());
    CHECK(mcp::json::parse(bad.body).contains("error"));
    CHECK(f.Count() == 0);
    CHECK(f.cleanups == 0); // Unpublished initialize rollback is not session cleanup.
    std::latch start(1);
    std::shared_ptr<mcp::event_dispatcher> a, b;
    std::thread first([&] { start.wait(); a = f.Insert("a"); });
    std::thread second([&] { start.wait(); b = f.Insert("b"); });
    start.count_down(); first.join(); second.join();
    CHECK(static_cast<bool>(a) != static_cast<bool>(b));
    CHECK(f.Count() == 1);
    const auto id = a ? "a" : "b";
    auto d = a ? a : b;
    d->close(); f.Sweep();
    CHECK(f.Count() == 0);
    CHECK(f.Insert("reused") != nullptr);
}
