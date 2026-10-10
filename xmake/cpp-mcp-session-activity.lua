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
            to = '#define MCP_SERVER_H\n#include "ContainedThreadEntry.h"\n#include "SessionCapacityAdmission.h"\n#include "SessionInitializationState.h"',
        },
        {
            from = "    bool running_ = false;",
            to = "    dvb::ContainedThreadEntry thread_entry_state_;\n    std::atomic<bool> running_{false};",
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
            to = "    void close_session(const std::string& session_id, bool idle_only = false);",
        },
    },
    source = {
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
            from = [[    // Enforce session limit
    if (max_sessions_ > 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (session_dispatchers_.size() >= max_sessions_) {
            LOG_WARNING("Max sessions reached (", max_sessions_, "), rejecting SSE connection");
            res.status = 503;
            res.set_content("{\"error\":\"Too many sessions\"}", "application/json");
            return;
        }
    }
]],
            to = "",
        },
        {
            from = [[    // Add session dispatcher to mapping table
    {
        std::lock_guard<std::mutex> lock(mutex_);
        session_dispatchers_[session_id] = session_dispatcher;
    }]],
            to = [[    // Both transports share atomic count-and-insert admission.
    if (!dvb::AdmitSession(session_dispatchers_, mutex_, max_sessions_, session_id, session_dispatcher)) {
        res.status = 503;
        res.set_content("{\"error\":\"Too many sessions\"}", "application/json");
        return;
    }]],
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
            to = [[        if (!dvb::AdmitSession(session_dispatchers_, mutex_, max_sessions_, session_id, session_dispatcher)) {
            res.status = 503;
            res.set_content("{\"error\":\"Too many sessions\"}", "application/json");
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
        {
            from = "void server::close_session(const std::string& session_id) {",
            to = "void server::close_session(const std::string& session_id, bool idle_only) {",
        },
        {
            from = [[        for (const auto& [key, handler] : session_cleanup_handler_) {
            handler(key);
        }]],
            to = "        std::map<std::string, session_cleanup_handler> cleanup_handlers;",
        },
        {
            from = [[            // Get dispatcher pointer
            auto dispatcher_it = session_dispatchers_.find(session_id);
            if (dispatcher_it != session_dispatchers_.end()) {
                dispatcher_to_close = dispatcher_it->second;
                session_dispatchers_.erase(dispatcher_it);
            }]],
            to = [[            // Only the winning retirement owns callbacks, even if DELETE races
            // a maintenance candidate. No application callbacks under mutex_.
            auto dispatcher_it = session_dispatchers_.find(session_id);
            if (dispatcher_it == session_dispatchers_.end()) return;
            if (idle_only && !dispatcher_it->second->close_if_inactive(
                    std::chrono::steady_clock::now(), std::chrono::seconds(session_timeout_))) return;
            cleanup_handlers = session_cleanup_handler_;
            dispatcher_to_close = dispatcher_it->second;
            session_dispatchers_.erase(dispatcher_it);]],
        },
        {
            from = "        // Release thread resources\n        if (thread_to_release) {",
            to = [[        LOG_INFO("Closed session: ", session_id, idle_only ? " (idle)" : " (explicit)");
        for (const auto& [key, handler] : cleanup_handlers) {
            try { handler(key); }
            catch (const std::exception& e) { LOG_WARNING("Session cleanup handler failed: ", key, ", ", e.what()); }
            catch (...) { LOG_WARNING("Session cleanup handler failed: ", key); }
        }

        // Release thread resources
        if (thread_to_release) {]],
        },
    },
}
end
