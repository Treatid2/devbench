-- Exact source overlays for pinned cpp-mcp f1117d5. Keep the dependency clean;
-- fail closed on anchor drift instead of silently compiling an unpatched server.
-- Streamable HTTP POSTs must count as activity, including notification/batch
-- and exceptional exits. Idle retirement and request admission serialize on
-- the dispatcher lock; an admitted long-running request is not idle.
function main()
return {
    header = {
        {
            from = "#define MCP_SERVER_H",
            to = '#define MCP_SERVER_H\n#include "ContainedThreadEntry.h"\n#include "SessionCapacityAdmission.h"\n#include "SessionInitializationState.h"\n#include "SessionRegistrationRollback.h"\n#include "SseProviderPump.h"\n#include "ImmutableCallbackSnapshot.h"',
        },
        {
            from = "    bool running_ = false;",
            to = "    dvb::ContainedThreadEntry thread_entry_state_;\n    bool session_admission_open_{false};  // protected by mutex_\n    std::atomic<bool> running_{false};",
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
            to = "    bool is_running() const;\n    std::uint64_t worker_entry_failures() const noexcept { return thread_entry_state_.Failures(); }",
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

    bool close_if_inactive(std::chrono::steady_clock::time_point now,
                           std::chrono::seconds timeout) {
        std::lock_guard<std::mutex> lk(m_);
        if (closed_.load(std::memory_order_acquire) || active_requests_ != 0 ||
            now - last_activity_ <= timeout) return false;
        closed_.store(true, std::memory_order_release);
        cv_.notify_all();
        return true;
    }

    // Get the last activity time]],
        },
        {
            from = "    std::chrono::steady_clock::time_point last_activity_{std::chrono::steady_clock::now()};",
            to = "    unsigned int active_requests_{0};\n    std::chrono::steady_clock::time_point last_activity_{std::chrono::steady_clock::now()};",
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
        {
            from = [[    session_cleanup_handler_[key] = handler;]],
            to = [[    dvb::UpdateCallbackSnapshot(session_cleanup_handler_, key, std::move(handler));]],
        },
        {
            from = [[    LOG_INFO("Starting MCP server on ", host_, ":", port_);]],
            to = [[    thread_pool_.restart();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        session_admission_open_ = true;
    }
    LOG_INFO("Starting MCP server on ", host_, ":", port_);]],
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
        if (!dvb::AdmitSession(session_dispatchers_, mutex_, max_sessions_, session_id, session_dispatcher, &session_admission_open_, &admission_closed)) {
            res.status = 503;
            res.set_content(admission_closed ? "{\"error\":\"Server stopping\"}" : "{\"error\":\"Too many sessions\"}", "application/json");
            return;
        }]],
        },
        {
            from = [[        maintenance_thread_ = std::make_unique<std::thread>([this]() {
            while (true) {
                // Check inactive sessions every 10 seconds
                std::unique_lock<std::mutex> lock(maintenance_mutex_);
                auto should_exit = maintenance_cond_.wait_for(lock, std::chrono::seconds(10), [this] {
                    return !maintenance_thread_run_;
                });
                if (should_exit) {
                    LOG_INFO("Maintenance thread exiting");
                    return;
                }
                lock.unlock();

                try {
                    check_inactive_sessions();
                } catch (const std::exception& e) {
                    LOG_ERROR("Exception in maintenance thread: ", e.what());
                } catch (...) {
                    LOG_ERROR("Unknown exception in maintenance thread");
                }
            }
        });]],
            to = [[        maintenance_thread_ = std::make_unique<std::thread>([this]() noexcept {
            (void)thread_entry_state_.Run([this]() {
                while (true) {
                    // Check inactive sessions every 10 seconds
                    std::unique_lock<std::mutex> lock(maintenance_mutex_);
                    auto should_exit = maintenance_cond_.wait_for(lock, std::chrono::seconds(10), [this] {
                        return !maintenance_thread_run_;
                    });
                    if (should_exit) {
                        LOG_INFO("Maintenance thread exiting");
                        return;
                    }
                    lock.unlock();

                    try {
                        check_inactive_sessions();
                    } catch (const std::exception& e) {
                        LOG_ERROR("Exception in maintenance thread: ", e.what());
                    } catch (...) {
                        LOG_ERROR("Unknown exception in maintenance thread");
                    }
                }
            });
        });]],
        },
        {
            from = [[        server_thread_ = std::make_unique<std::thread>([this]() {
            LOG_INFO("Starting server in separate thread");
            if (!http_server_->listen(host_.c_str(), port_)) {
                LOG_ERROR("Failed to start server on ", host_, ":", port_);
                running_ = false;
                return;
            }
        });]],
            to = [[        server_thread_ = std::make_unique<std::thread>([this]() noexcept {
            (void)thread_entry_state_.Run([this]() {
                LOG_INFO("Starting server in separate thread");
                if (!http_server_->listen(host_.c_str(), port_)) {
                    LOG_ERROR("Failed to start server on ", host_, ":", port_);
                    running_ = false;
                    return;
                }
            });
        });]],
        },
        {
            from = "bool server::is_running() const {\n    return running_;\n}",
            to = "bool server::is_running() const {\n    return running_.load(std::memory_order_acquire) && !thread_entry_state_.Failed();\n}",
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
            to = [[        // Recheck activity and retire under the same lock as POST admission.
        close_session(session_id, true);]],
        },
    },
}
end
