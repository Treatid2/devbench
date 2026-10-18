-- Exact source overlays for pinned cpp-mcp f1117d5. Keep the dependency clean;
-- fail closed on anchor drift instead of silently compiling an unpatched server.
-- Streamable HTTP POSTs must count as activity, including notification/batch
-- and exceptional exits. Idle retirement and request admission serialize on
-- the dispatcher lock; an admitted long-running request is not idle.
function main()
return {
    header = {
        {
            from = "        unsigned int threadpool_size{ std::thread::hardware_concurrency() };",
            to = [[        unsigned int threadpool_size{ std::thread::hardware_concurrency() };
        // Immutable embedding gate: read only its retained readiness token.
        // No reentry/locks; standalone default admits after listener readiness.
        std::function<bool()> request_admission{ []() noexcept { return true; } };
        // Host qualification seam. Must create the supplied function exactly
        // once asynchronously and return its owned joinable thread; no inline
        // execution, foreign worker or detached substitute is permitted.
        std::function<std::thread(std::function<void()>)> startup_thread_factory{
            [](std::function<void()> body) { return std::thread(std::move(body)); }
        };]],
        },
        {
            from = "    thread_pool thread_pool_;",
            to = "    const std::function<bool()> request_admission_;\n    std::function<std::thread(std::function<void()>)> startup_thread_factory_;\n    thread_pool thread_pool_;",
        },
        {
            from = "#define MCP_SERVER_H",
            to = '#define MCP_SERVER_H\n#include <future>\n#include "ContainedThreadEntry.h"\n#include "SessionCapacityAdmission.h"\n#include "SessionInitializationState.h"\n#include "SessionRegistrationRollback.h"\n#include "SseProviderPump.h"\n#include "ImmutableCallbackSnapshot.h"\nnamespace dvb { struct McpSessionRetirementFixture; }',
        },
        {
            from = "class event_dispatcher {",
            to = "class event_dispatcher {\n    friend struct ::dvb::McpSessionRetirementFixture;",
        },
        {
            from = "class server {",
            to = "class server {\n    friend struct ::dvb::McpSessionRetirementFixture;",
        },
        {
            from = "    bool running_ = false;",
            to = [[    // Lifecycle is owner-only; callbacks must not reenter Start/Stop.
    std::mutex lifecycle_mutex_;
    void stop_locked();
    bool routes_ready_{false};
    bool routes_failed_{false};
    dvb::HttpListenerStartup::Token active_startup_token_{0};
    dvb::ContainedThreadEntry thread_entry_state_;
    std::atomic<std::uint64_t> entry_failure_baseline_{0};
    std::atomic<std::uint64_t> pool_failure_baseline_{0};
    std::atomic<bool> bind_failed_{false};
    bool session_admission_open_{false};  // protected by mutex_
    std::atomic<bool> running_{false};]],
        },
        {
            from = [[    // SSE thread
    std::map<std::string, std::unique_ptr<std::thread>> sse_threads_;]],
            to = "    // Legacy SSE is owned by the HTTP response provider; no auxiliary worker.",
        },
        {
            from = "    std::map<std::string, session_cleanup_handler> session_cleanup_handler_;",
            to = "    using cleanup_handler_map = std::map<std::string, session_cleanup_handler>;\n    std::shared_ptr<const cleanup_handler_map> session_cleanup_handler_;",
        },
        {
            from = "    bool is_running() const;",
            to = [[    bool is_running() const;
    // Per-request admission, not cancellation of already admitted work.
    bool requests_admitted() const noexcept {
        try { return request_admission_(); } catch (...) { return false; }
    }
    bool admit_http_request(httplib::Response& response) const {
        if (requests_admitted()) return true;
        response.status = 503;
        response.set_header("Access-Control-Allow-Origin", "*");
        response.set_content("{\"error\":\"Server not ready\"}", "application/json");
        return false;
    }
    std::uint64_t worker_entry_failures() const noexcept { return thread_entry_state_.Failures(); }
    bool last_start_bind_failed() const noexcept { return bind_failed_.load(std::memory_order_acquire); }]],
        },
        {
            from = "    // Get the last activity time",
            to = [[    // Streamable HTTP request activity, protected by the dispatcher mutex.
    bool begin_request() {
        std::lock_guard<std::mutex> lk(m_);
        if (closed_.load(std::memory_order_acquire)) return false;
        ++active_requests_;
        last_activity_ = std::chrono::steady_clock::now();
        return true;
    }

    void end_request() {
        std::lock_guard<std::mutex> lk(m_);
        --active_requests_;
        last_activity_ = std::chrono::steady_clock::now();
    }

    // Terminal state and idle expiry share the actual request-admission lock.
    // A closed stream is eligible even when healthy idle expiry is disabled.
    bool try_retire(std::chrono::steady_clock::time_point now,
                    std::chrono::seconds timeout, bool close_requested = false) {
        std::lock_guard<std::mutex> lk(m_);
        if (close_requested) {
            closed_.store(true, std::memory_order_release);
            cv_.notify_all();
        }
        if (retirement_claimed_ || active_requests_ != 0) return false;
        if (!closed_.load(std::memory_order_acquire) &&
            (timeout.count() == 0 || now - last_activity_ <= timeout)) return false;
        retirement_claimed_ = true;
        closed_.store(true, std::memory_order_release);
        cv_.notify_all();
        return true;
    }

    bool close_if_inactive(std::chrono::steady_clock::time_point now,
                           std::chrono::seconds timeout) {
        return try_retire(now, timeout);
    }

    // Get the last activity time]],
        },
        {
            from = "    std::chrono::steady_clock::time_point last_activity_{std::chrono::steady_clock::now()};",
            to = "    unsigned int active_requests_{0};\n    bool retirement_claimed_{false};  // protected by m_\n    std::chrono::steady_clock::time_point last_activity_{std::chrono::steady_clock::now()};",
        },
        {
            from = "/**\n * @class server",
            to = [[// Holds the exact admitted dispatcher until every POST exit, including throws.
// It neither creates a session nor restores any application custody.
class session_request_guard {
public:
    session_request_guard() = default;
    session_request_guard(const session_request_guard&) = delete;
    session_request_guard& operator=(const session_request_guard&) = delete;
    ~session_request_guard() { if (dispatcher_) dispatcher_->end_request(); }

    bool acquire(std::shared_ptr<event_dispatcher> dispatcher) {
        if (dispatcher_ || !dispatcher || !dispatcher->begin_request()) return false;
        dispatcher_ = std::move(dispatcher);
        return true;
    }
private:
    std::shared_ptr<event_dispatcher> dispatcher_;
};

/**
 * @class server]],
        },
        {
            from = "    void close_session(const std::string& session_id);",
            to = "    void close_session(const std::string& session_id, bool idle_only = false, std::shared_ptr<event_dispatcher> expected = {});",
        },
        {
            from = [[    bool wait_event(httplib::DataSink* sink, const std::chrono::milliseconds& timeout = std::chrono::milliseconds(10000)) {
        if (!sink || closed_.load(std::memory_order_acquire)) {]],
            to = [[    bool wait_event(httplib::DataSink* sink, const std::chrono::milliseconds& timeout = std::chrono::milliseconds(10000), bool* timed_out = nullptr) {
        if (timed_out) *timed_out = false;
        if (!sink || closed_.load(std::memory_order_acquire)) {]],
        },
        {
            from = [[            if (!result) {
                return false;
            }]],
            to = [[            if (!result) {
                if (timed_out) *timed_out = true;
                return false;
            }]],
        },
    },
    source = {
        -- Parser exception text can quote request fragments; log the boundary,
        -- not peer argument/body snippets. Other exception text remains bounded.
        { from = 'LOG_ERROR("Failed to parse JSON request: ", e.what());', to = 'LOG_ERROR("Failed to parse JSON request");' },
        { from = 'LOG_ERROR("Failed to create request object: ", e.what());', to = 'LOG_ERROR("Failed to create request object");' },
        { from = 'LOG_ERROR("Failed to parse JSON: ", e.what());', to = 'LOG_ERROR("Failed to parse JSON");' },
        { from = 'LOG_ERROR("MCP exception: ", e.what(), ", code: ", static_cast<int>(e.code()));', to = 'LOG_ERROR("MCP exception code: ", static_cast<int>(e.code()));' },
        { from = 'LOG_ERROR("Exception while processing request: ", e.what());', to = 'LOG_ERROR("Exception while processing request");' },
        {
            from = "json server::process_request(const request& req, const std::string& session_id) {",
            to = [[json server::process_request(const request& req, const std::string& session_id) {
    // Includes initialize, initialized-notification and built-in catalogue.
    if (!requests_admitted()) {
        if (req.is_notification()) return json::object();
        return response::create_error(req.id, error_code::invalid_request,
                                      "Server not ready").to_json();
    }]],
        },
        {
            from = "void server::handle_mcp_post(const httplib::Request& req, httplib::Response& res) {",
            to = "void server::handle_mcp_post(const httplib::Request& req, httplib::Response& res) {\n    if (!admit_http_request(res)) return;",
        },
        {
            from = "void server::handle_mcp_get(const httplib::Request& req, httplib::Response& res) {",
            to = "void server::handle_mcp_get(const httplib::Request& req, httplib::Response& res) {\n    if (!admit_http_request(res)) return;",
        },
        {
            from = "void server::handle_jsonrpc(const httplib::Request& req, httplib::Response& res) {",
            to = "void server::handle_jsonrpc(const httplib::Request& req, httplib::Response& res) {\n    if (!admit_http_request(res)) return;",
        },
        {
            from = "void server::handle_mcp_delete(const httplib::Request& req, httplib::Response& res) {",
            to = "void server::handle_mcp_delete(const httplib::Request& req, httplib::Response& res) {\n    if (!admit_http_request(res)) return;",
        },
        {
            from = "            try {\n                if (dispatcher->is_closed() || !running_)",
            to = "            try {\n                if (dispatcher->is_closed() || !running_ || !requests_admitted())",
        },
        {
            from = "                if (!result) {\n                    if (dispatcher->is_closed() || !running_)",
            to = "                if (!result) {\n                    if (dispatcher->is_closed() || !running_ || !requests_admitted())",
        },
        {
            from = "    , thread_pool_(conf.threadpool_size)",
            to = "    , request_admission_(conf.request_admission)\n    , startup_thread_factory_(conf.startup_thread_factory)\n    , thread_pool_(conf.threadpool_size)",
        },
        {
            from = [[    session_cleanup_handler_[key] = handler;]],
            to = [[    dvb::UpdateCallbackSnapshot(session_cleanup_handler_, key, std::move(handler));]],
        },
        {
            from = [[        auto mcp_req = parse_jsonrpc_message(items[0]);
        json result = handle_initialize(mcp_req, session_id);

        res.set_header("Mcp-Session-Id", session_id);
        res.set_header("Content-Type", "application/json");
        res.set_content(result.dump(), "application/json");
        return;]],
            to = [[        // Failed initialization must not consume an undisclosed session slot.
        // No published session is closed here; rollback requires exact allocation identity.
        auto rollback = [&]() {
            (void)dvb::RollbackSessionRegistration(
                session_dispatchers_, session_initialized_, mutex_, session_id, session_dispatcher);
        };
        try {
            if (!request_activity.acquire(session_dispatcher)) {
                rollback();
                res.status = 503;
                res.set_content("{\"error\":\"Session closed during initialization\"}", "application/json");
                return;
            }
            auto mcp_req = parse_jsonrpc_message(items[0]);
            json result = handle_initialize(mcp_req, session_id);
            if (result.contains("error")) {
                rollback();
                res.set_content(result.dump(), "application/json");
                return;
            }
            // Complete serialization/response construction before exposing the new ID.
            res.set_content(result.dump(), "application/json");
            res.set_header("Mcp-Session-Id", session_id);
        } catch (...) {
            rollback();
            throw;
        }
        return;]],
        },
        {
            from = [[        std::lock_guard<std::mutex> lock(mutex_);
        // Check if session still exists (either SSE or HTTP mode)
        auto it = session_dispatchers_.find(session_id);
        bool has_dispatcher = (it != session_dispatchers_.end());

        // For HTTP mode, we also track initialization in session_initialized_ map
        // So we allow setting initialized state even without a dispatcher for HTTP sessions
        if (!has_dispatcher) {
            LOG_DEBUG("Setting initialization state for HTTP session: ", session_id);
        }

        session_initialized_[session_id] = initialized;]],
            to = [[        // Initialization cannot recreate state after dispatcher retirement.
        // The helper uses the same owner lock as registration and retirement.
        (void)dvb::SetRegisteredSessionInitialized(
            session_dispatchers_, session_initialized_, mutex_, session_id, initialized);]],
        },
        {
            from = [[        // Enforce session limit
        if (max_sessions_ > 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (session_dispatchers_.size() >= max_sessions_) {
                res.status = 503;
                res.set_content("{\"error\":\"Too many sessions\"}", "application/json");
                return;
            }
        }
]],
            to = "",
        },
        {
            from = [[        {
            std::lock_guard<std::mutex> lock(mutex_);
            session_dispatchers_[session_id] = session_dispatcher;
        }]],
            to = [[        bool admission_closed = false;
        std::size_t count_before_admission = 0;
        if (!dvb::AdmitSession(session_dispatchers_, mutex_, max_sessions_, session_id, session_dispatcher, &session_admission_open_, &admission_closed, &request_admission_, &count_before_admission)) {
            LOG_WARNING("Session admission refused: transport=streamable_http reason=", admission_closed ? "server_not_ready" : "capacity",
                        " count=", count_before_admission, " limit=", max_sessions_);
            res.status = 503;
            res.set_content(admission_closed ? "{\"error\":\"Server stopping\"}" : "{\"error\":\"Too many sessions\"}", "application/json");
            return;
        }]],
        },
        {
            from = "bool server::is_running() const {\n    return running_;\n}",
            to = [[bool server::is_running() const {
    return running_.load(std::memory_order_acquire) &&
        thread_entry_state_.Failures() == entry_failure_baseline_.load(std::memory_order_acquire) &&
        thread_pool_.worker_failures() == pool_failure_baseline_.load(std::memory_order_acquire) &&
        http_server_->listener_startup_ready();
}]],
        },
        {
            from = "    // Validate session for non-initialize requests\n    if (!is_initialize) {",
            to = "    session_request_guard request_activity;\n    // Validate session for non-initialize requests\n    if (!is_initialize) {",
        },
        {
            from = [[        if (session_dispatchers_.find(session_id) == session_dispatchers_.end()) {
            // Session expired or invalid]],
            to = [[        auto session = session_dispatchers_.find(session_id);
        if (session == session_dispatchers_.end() ||
            !request_activity.acquire(session->second)) {
            // Session expired or invalid]],
        },
        {
            from = [[        LOG_INFO("Closing inactive session: ", session_id);
        
        close_session(session_id);]],
            to = [[        // Exact instance recheck under the same lock as POST admission.
        close_session(session_id, true, dispatcher);]],
        },
        {
            from = "    if (!running_ || session_timeout_ == 0) return;",
            to = "    if (!running_) return;",
        },
        {
            from = "    std::vector<std::string> sessions_to_close;",
            to = "    std::vector<std::pair<std::string, std::shared_ptr<event_dispatcher>>> sessions_to_close;",
        },
        {
            from = "            if (now - dispatcher->last_activity() > timeout) {",
            to = "            if (dispatcher->is_closed() || (session_timeout_ != 0 && now - dispatcher->last_activity() > timeout)) {",
        },
        {
            from = "                sessions_to_close.push_back(session_id);",
            to = "                sessions_to_close.emplace_back(session_id, dispatcher);",
        },
        {
            from = "    for (const auto& session_id : sessions_to_close) {",
            to = "    for (const auto& [session_id, dispatcher] : sessions_to_close) {",
        },
        {
            from = [[    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (session_dispatchers_.find(session_id) == session_dispatchers_.end()) {
            res.status = 404;
            return;
        }
    }

    close_session(session_id);]],
            to = [[    std::shared_ptr<event_dispatcher> expected;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto session = session_dispatchers_.find(session_id);
        if (session == session_dispatchers_.end()) {
            res.status = 404;
            return;
        }
        expected = session->second;
    }

    close_session(session_id, false, expected);]],
        },
    },
}
end
