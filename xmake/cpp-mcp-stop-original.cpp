void server::stop() {
    if (!running_) {
        return;
    }
    
    LOG_INFO("Stopping MCP server on ", host_, ":", port_);
    running_ = false;

    // Close maintenance thread
    if (maintenance_thread_ && maintenance_thread_->joinable()) {
        {
            std::unique_lock<std::mutex> lock(maintenance_mutex_);
            maintenance_thread_run_ = false;
        }

        maintenance_cond_.notify_one();

        try {
            maintenance_thread_->join();
        } catch (...) {
            maintenance_thread_->detach();
        }
    }
    
    // Copy all dispatchers and threads to avoid holding the lock for too long
    std::vector<std::shared_ptr<event_dispatcher>> dispatchers_to_close;
    std::vector<std::unique_ptr<std::thread>> threads_to_join;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // Copy all dispatchers
        dispatchers_to_close.reserve(session_dispatchers_.size());
        for (const auto& [_, dispatcher] : session_dispatchers_) {
            dispatchers_to_close.push_back(dispatcher);
        }
        
        // Copy all threads
        threads_to_join.reserve(sse_threads_.size());
        for (auto& [_, thread] : sse_threads_) {
            if (thread && thread->joinable()) {
                threads_to_join.push_back(std::move(thread));
            }
        }
        
        // Clear the maps
        session_dispatchers_.clear();
        sse_threads_.clear();
        session_initialized_.clear();
    }
    
    // Close all sessions
    for (const auto& [session_id, _] : session_dispatchers_) {
        close_session(session_id);
    }
    
    // Give threads some time to handle close events
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    
    // Wait for threads to finish outside the lock (with timeout limit)
    const auto timeout_point = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    
    for (auto& thread : threads_to_join) {
        if (!thread || !thread->joinable()) {
            continue;
        }
        
        if (std::chrono::steady_clock::now() >= timeout_point) {
            // If timeout reached, detach remaining threads
            LOG_WARNING("Thread join timeout reached, detaching remaining threads");
            thread->detach();
            continue;
        }
        
        // Try using timeout join
        bool joined = false;
        try {
            // Create future and promise for timeout join
            std::promise<void> thread_done;
            auto future = thread_done.get_future();
            
            // Try join in another thread
            std::thread join_helper([&thread, &thread_done]() {
                try {
                    thread->join();
                    thread_done.set_value();
                } catch (...) {
                    try {
                        thread_done.set_exception(std::current_exception());
                    } catch (...) {}
                }
            });
            
            // Wait for join to complete or timeout
            if (future.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready) {
                future.get(); // Get possible exception
                joined = true;
            }
            
            // Process join_helper thread
            if (join_helper.joinable()) {
                if (joined) {
                    join_helper.join();
                } else {
                    join_helper.detach();
                }
            }
        } catch (...) {
            joined = false;
        }
        
        // If join fails, then detach
        if (!joined) {
            try {
                thread->detach();
            } catch (...) {
                // Ignore exceptions
            }
        }
    }
    
    if (server_thread_ && server_thread_->joinable()) {
        http_server_->stop();
        try {
            server_thread_->join();
        } catch (...) {
            server_thread_->detach();
        }
    } else {
        http_server_->stop();
    }
    
    LOG_INFO("MCP server stopped");
}
