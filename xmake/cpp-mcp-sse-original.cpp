void server::handle_sse(const httplib::Request& req, httplib::Response& res) {
    // Enforce session limit
    if (max_sessions_ > 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (session_dispatchers_.size() >= max_sessions_) {
            LOG_WARNING("Max sessions reached (", max_sessions_, "), rejecting SSE connection");
            res.status = 503;
            res.set_content("{\"error\":\"Too many sessions\"}", "application/json");
            return;
        }
    }

    std::string session_id = generate_session_id();
    std::string session_uri = msg_endpoint_ + "?session_id=" + session_id;
    
    // Setup SSE response headers
    res.set_header("Content-Type", "text/event-stream");
    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection", "keep-alive");
    res.set_header("Access-Control-Allow-Origin", "*");
    
    // Create session-specific event dispatcher
    auto session_dispatcher = std::make_shared<event_dispatcher>();
    
    // Initialize activity time
    session_dispatcher->update_activity();
    
    // Add session dispatcher to mapping table
    {
        std::lock_guard<std::mutex> lock(mutex_);
        session_dispatchers_[session_id] = session_dispatcher;
    }
    
    // Create session thread
    auto thread = std::make_unique<std::thread>([this, res, session_id, session_uri, session_dispatcher]() {
        try {
            // Send initial session URI
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            std::stringstream ss;
            ss << "event: endpoint\r\ndata: " << session_uri << "\r\n\r\n";
            session_dispatcher->send_event(ss.str());
            
            // Update activity time (after sending message)
            session_dispatcher->update_activity();
            
            // Send periodic heartbeats to detect connection status
            int heartbeat_count = 0;
            while (running_ && !session_dispatcher->is_closed()) {
               std::this_thread::sleep_for(std::chrono::seconds(5) + std::chrono::milliseconds(rand() % 500)); // NOTE: DO NOT set it the same as the timeout of wait_event
                
                if (session_dispatcher->is_closed() || !running_) {
                    break;
                }
                
                std::stringstream heartbeat;
                heartbeat << "event: heartbeat\r\ndata: " << heartbeat_count++ << "\r\n\r\n";
                
                try {
                    bool sent = session_dispatcher->send_event(heartbeat.str());
                    if (!sent) {
                        LOG_WARNING("Failed to send heartbeat, client may have closed connection: ", session_id);
                        break;
                    }
                    
                    // Update activity time (heartbeat successful)
                    session_dispatcher->update_activity();
                } catch (const std::exception& e) {
                    LOG_ERROR("Failed to send heartbeat: ", e.what());
                    break;
                }
            }
        } catch (const std::exception& e) {
            LOG_ERROR("SSE session thread exception: ", session_id, ", ", e.what());
        }
        
        close_session(session_id);
    });
    
    // Store thread
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sse_threads_[session_id] = std::move(thread);
    }
    
    // Setup chunked content provider
    res.set_chunked_content_provider("text/event-stream", [this, session_id, session_dispatcher](size_t /* offset */, httplib::DataSink& sink) {
        try {
            // Check if session is closed - directly get status from dispatcher, reduce lock contention
            if (session_dispatcher->is_closed()) {
                return false;
            }
            
            // Update activity time (received request)
            session_dispatcher->update_activity();
            
            // Wait for event
            bool result = session_dispatcher->wait_event(&sink);
            if (!result) {
                LOG_WARNING("Failed to wait for event, closing connection: ", session_id);
                
                close_session(session_id);
                
                return false;
            }
            
            // Update activity time (successfully received message)
            session_dispatcher->update_activity();

            return true;
        } catch (const std::exception& e) {
            LOG_ERROR("SSE content provider exception: ", e.what());
            
            close_session(session_id);
            
            return false;
        }
    });
}
