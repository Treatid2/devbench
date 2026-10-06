-- Exact source overlays for pinned cpp-mcp f1117d5. Keep the dependency clean;
-- fail closed on anchor drift instead of silently compiling an unpatched server.
-- Streamable HTTP POSTs must count as activity, including notification/batch
-- and exceptional exits. Idle retirement and request admission serialize on
-- the dispatcher lock; an admitted long-running request is not idle.
function main()
return {
    header = {
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
