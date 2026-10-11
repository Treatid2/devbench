void server::close_session(const std::string& session_id, bool idle_only, std::shared_ptr<event_dispatcher> expected) {
    std::shared_ptr<event_dispatcher> dispatcher;
    std::shared_ptr<const cleanup_handler_map> handlers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto session = session_dispatchers_.find(session_id);
        if (session == session_dispatchers_.end()) return;
        if (expected && session->second != expected) return;
        if (idle_only && !session->second->close_if_inactive(
                std::chrono::steady_clock::now(), std::chrono::seconds(session_timeout_))) return;
        // These shared_ptr copies do not allocate or copy a user callable.
        handlers = session_cleanup_handler_;
        dispatcher = session->second;
        session_initialized_.erase(session_id);
        session_dispatchers_.erase(session);
    }
    dispatcher->close();
    if (handlers) {
        for (const auto& [key, handler] : *handlers) {
            try { handler(key); }
            catch (...) {
                try { LOG_WARNING("Session cleanup handler failed"); } catch (...) {}
            }
        }
    }
}
