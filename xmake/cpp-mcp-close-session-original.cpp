void server::close_session(const std::string& session_id) {
     // Clean up resources safely
    try {
        for (const auto& [key, handler] : session_cleanup_handler_) {
            handler(key);
        }

        // Copy resources to be processed
        std::shared_ptr<event_dispatcher> dispatcher_to_close;
        std::unique_ptr<std::thread> thread_to_release;
        
        {
            std::lock_guard<std::mutex> lock(mutex_);
            
            // Get dispatcher pointer
            auto dispatcher_it = session_dispatchers_.find(session_id);
            if (dispatcher_it != session_dispatchers_.end()) {
                dispatcher_to_close = dispatcher_it->second;
                session_dispatchers_.erase(dispatcher_it);
            }
            
            // Get thread pointer
            auto thread_it = sse_threads_.find(session_id);
            if (thread_it != sse_threads_.end()) {
                thread_to_release = std::move(thread_it->second);
                sse_threads_.erase(thread_it);
            }
            
            // Clean up initialization status
            session_initialized_.erase(session_id);
        }
        
        // Close dispatcher outside the lock
        if (dispatcher_to_close && !dispatcher_to_close->is_closed()) {
            dispatcher_to_close->close();
        }
        
        // Release thread resources
        if (thread_to_release) {
            thread_to_release.release();
        }
    } catch (const std::exception& e) {
        LOG_WARNING("Exception while cleaning up session resources: ", session_id, ", ", e.what());
    } catch (...) {
        LOG_WARNING("Unknown exception while cleaning up session resources: ", session_id);
    }
}
