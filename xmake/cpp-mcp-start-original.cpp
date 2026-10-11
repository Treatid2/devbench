bool server::start(bool blocking) {
    if (running_) {
        return true;  // Already running
    }
    
    LOG_INFO("Starting MCP server on ", host_, ":", port_);
    
    // Setup CORS handling
    http_server_->Options(".*", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Accept, Mcp-Session-Id, MCP-Protocol-Version");
        res.set_header("Access-Control-Expose-Headers", "Mcp-Session-Id, MCP-Protocol-Version");
        res.status = 204; // No Content
    });

    // Setup JSON-RPC endpoint (SSE transport)
    http_server_->Post(msg_endpoint_.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
        this->handle_jsonrpc(req, res);
        LOG_INFO(req.remote_addr, ":", req.remote_port, " - \"POST ", req.path, " HTTP/1.1\" ", res.status);
    });

    // Setup SSE endpoint (legacy 2024-11-05 transport)
    http_server_->Get(sse_endpoint_.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
        this->handle_sse(req, res);
        LOG_INFO(req.remote_addr, ":", req.remote_port, " - \"GET ", req.path, " HTTP/1.1\" ", res.status);
    });

    // Streamable HTTP transport (2025-03-26)
    http_server_->Post(mcp_endpoint_.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
        this->handle_mcp_post(req, res);
        LOG_INFO(req.remote_addr, ":", req.remote_port, " - \"POST ", req.path, " HTTP/1.1\" ", res.status);
    });

    http_server_->Get(mcp_endpoint_.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
        this->handle_mcp_get(req, res);
        LOG_INFO(req.remote_addr, ":", req.remote_port, " - \"GET ", req.path, " HTTP/1.1\" ", res.status);
    });

    http_server_->Delete(mcp_endpoint_.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
        this->handle_mcp_delete(req, res);
        LOG_INFO(req.remote_addr, ":", req.remote_port, " - \"DELETE ", req.path, " HTTP/1.1\" ", res.status);
    });
    
    // Start resource check thread (only start in non-blocking mode)
    if (!blocking) {
        maintenance_thread_run_ = true;
        maintenance_thread_ = std::make_unique<std::thread>([this]() {
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
    }
    
    // Start server
    if (blocking) {
        running_ = true;
        LOG_INFO("Starting server in blocking mode");
        if (!http_server_->listen(host_.c_str(), port_)) {
            running_ = false;
            LOG_ERROR("Failed to start server on ", host_, ":", port_);
            return false;
        }
        return true;
    } else {
        // Start server in a separate thread
        server_thread_ = std::make_unique<std::thread>([this]() {
            LOG_INFO("Starting server in separate thread");
            if (!http_server_->listen(host_.c_str(), port_)) {
                LOG_ERROR("Failed to start server on ", host_, ":", port_);
                running_ = false;
                return;
            }
        });
        running_ = true;
        return true;
    }
}
