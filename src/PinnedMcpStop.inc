// Owner-thread only. The full startup transaction must serialize Start/Stop;
// no application callback may call this drain from an owned worker.
void server::stop() {
    // Even a failed listener may leave owned workers: never return on !running_.
    running_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        session_admission_open_ = false;
        for (const auto& [id, dispatcher] : session_dispatchers_) {
            (void)id;
            dispatcher->close();
        }
    }
    thread_pool_.close_admission();
    {
        std::lock_guard<std::mutex> lock(maintenance_mutex_);
        maintenance_thread_run_ = false;
    }
    maintenance_cond_.notify_all();
    // The patched HTTP stop claims/closes a bound socket even before listen.
    // Joining its listener also drains its owned HTTP queue and destroys every
    // response/releaser while this server and its callback owners are alive.
    http_server_->stop();
    if (maintenance_thread_ && maintenance_thread_->joinable()) maintenance_thread_->join();
    if (server_thread_ && server_thread_->joinable()) server_thread_->join();
    maintenance_thread_.reset();
    server_thread_.reset();
    thread_pool_.shutdown();

    // No workers remain. Extract one node at a time without allocating a new
    // ID vector/map or copying fallible std::function targets. Provider/explicit
    // retirement may already have won; only remaining registrations get callbacks.
    for (;;) {
        decltype(session_dispatchers_)::node_type retired;
        std::shared_ptr<const cleanup_handler_map> handlers;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (session_dispatchers_.empty()) {
                session_initialized_.clear();
                break;
            }
            auto session = session_dispatchers_.begin();
            handlers = session_cleanup_handler_;
            session_initialized_.erase(session->first);
            retired = session_dispatchers_.extract(session);
        }
        if (handlers) {
            for (const auto& [key, handler] : *handlers) {
                // Preserve the pinned registration-key callback contract.
                try { handler(key); }
                catch (...) { try { LOG_WARNING("Session cleanup handler failed"); } catch (...) {} }
            }
        }
    }
}
