// In-process DevBench owns HTTP workers. Lifecycle calls are owner-only and
// must not be made from transport tasks or cleanup callbacks.
bool server::start(bool blocking) {
    std::unique_lock<std::mutex> lifecycle(lifecycle_mutex_);
    if (is_running()) return true;
    // Failed generations can retain workers even though running_ is false.
    stop_locked();
    if (routes_failed_) return false;  // partial route graph requires a fresh server

    std::future<bool> listener_outcome;
    dvb::HttpListenerStartup::Token token = 0;
    try {
        bind_failed_.store(false, std::memory_order_release);
        // This is the actual held listener socket, not a free-port probe.
        if (!http_server_->bind_to_port(host_, port_)) {
            bind_failed_.store(true, std::memory_order_release);
            stop_locked();
            return false;
        }
        token = http_server_->begin_listener_startup();
        active_startup_token_ = token;
        if (!routes_ready_) {
            // Route insertion is fallible and cannot be atomically undone by
            // pinned httplib. Never append the same partial graph on a retry.
            routes_failed_ = true;
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
            routes_ready_ = true;
            routes_failed_ = false;
        }
        entry_failure_baseline_.store(thread_entry_state_.Failures(), std::memory_order_release);
        pool_failure_baseline_.store(thread_pool_.worker_failures(), std::memory_order_release);
        // Snapshot only the retired generation. A failure during restart is
        // new evidence and must not be swallowed by advancing the baseline.
        thread_pool_.restart();
        {
            std::lock_guard<std::mutex> lock(maintenance_mutex_);
            maintenance_thread_run_ = true;
        }
        // Allocate the slot before creating a worker, so no later allocation
        // can destroy an escaped joinable temporary during exception unwind.
        maintenance_thread_ = std::make_unique<std::thread>();
        *maintenance_thread_ = startup_thread_factory_([this]() noexcept {
            (void)thread_entry_state_.Run([this]() {
                for (;;) {
                    std::unique_lock<std::mutex> lock(maintenance_mutex_);
                    if (maintenance_cond_.wait_for(lock, std::chrono::seconds(10),
                                                   [this] { return !maintenance_thread_run_; })) return;
                    lock.unlock();
                    // Failure is contained by the outer worker boundary and
                    // retained in the cumulative counter, not logged/retried.
                    check_inactive_sessions();
                }
            });
        });
        if (!maintenance_thread_->joinable())
            throw std::runtime_error("Startup factory did not return an owned worker");
        auto completion = std::make_shared<std::promise<bool>>();
        listener_outcome = completion->get_future();
        running_.store(true, std::memory_order_release);
        server_thread_ = std::make_unique<std::thread>();
        *server_thread_ = startup_thread_factory_(
            [this, completion = std::move(completion)]() mutable noexcept {
                bool listened = false;
                const bool contained = thread_entry_state_.Run([&] {
                    listened = http_server_->listen_after_bind();
                });
                running_.store(false, std::memory_order_release);
                // Also wake the startup waiter if failure preceded listen entry.
                // This is HTTP socket cancellation, never this worker's join.
                (void)thread_entry_state_.Run([this] { http_server_->stop(); });
                try { completion->set_value(contained && listened); }
                catch (...) {
                    // Explicitly destroy the sole promise even if a platform
                    // retains the thread callable after return. The blocking
                    // owner receives broken_promise instead of waiting forever.
                    completion.reset();
                }
            });
        if (!server_thread_->joinable())
            throw std::runtime_error("Startup factory did not return an owned worker");
        if (!http_server_->wait_listener_startup_until(token,
                std::chrono::steady_clock::now() + std::chrono::seconds(30)) ||
            !is_running()) {
            stop_locked();
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            session_admission_open_ = true;
        }
    } catch (...) {
        // Includes route allocation, core restart and either worker constructor.
        // Dependency/callback owners outlive all workers in this rollback.
        stop_locked();
        try { LOG_ERROR("MCP server startup failed"); } catch (...) {}
        return false;
    }

    if (!blocking) return true;
    // Preserve the blocking API without preventing another owner from stopping
    // it. Completion belongs to this exact generation, not a later restart.
    lifecycle.unlock();
    bool listened = false;
    try { listened = listener_outcome.get(); } catch (...) {}
    lifecycle.lock();
    if (active_startup_token_ == token) stop_locked();
    return listened;
}
