// Exact pinned handle_sse replacement inserted into the generated dependency TU.
// The .inc suffix excludes this fragment from the src/**.cpp target glob.
// cpp-mcp.lua guards the entire original before inserting it in namespace mcp.
void server::handle_sse(const httplib::Request& req, httplib::Response& res) {
    if (!admit_http_request(res)) return;
    (void)req;
    const std::string session_id = generate_session_id();
    auto session_dispatcher = std::make_shared<event_dispatcher>();
    auto pump = std::make_shared<dvb::SseProviderPump>(
        "event: endpoint\r\ndata: " + msg_endpoint_ + "?session_id=" + session_id + "\r\n\r\n");
    session_dispatcher->update_activity();
    // Exact identity prevents a delayed provider/releaser from retiring a replacement.
    // The noexcept completion also covers failures before provider installation.
    const auto retire = [this, session_id, session_dispatcher]() noexcept {
        session_dispatcher->close();
        try { close_session(session_id, false, session_dispatcher); }
        catch (...) {} // Cleanup/logging exceptions cannot escape Response destruction.
    };
    bool admission_closed = false;
    std::size_t count_before_admission = 0;
    if (!dvb::AdmitSession(session_dispatchers_, mutex_, max_sessions_, session_id, session_dispatcher, &session_admission_open_, &admission_closed, &request_admission_, &count_before_admission)) {
        LOG_WARNING("Session admission refused: transport=legacy_sse reason=", admission_closed ? "server_not_ready" : "capacity",
                    " count=", count_before_admission, " limit=", max_sessions_);
        res.status = 503;
        res.set_content(admission_closed ? "{\"error\":\"Server stopping\"}" : "{\"error\":\"Too many sessions\"}", "application/json");
        return;
    }
    try {
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_chunked_content_provider("text/event-stream",
            [this, session_dispatcher, pump, retire](size_t, httplib::DataSink& sink) {
                const bool result = pump->Pump(
                    [this, &session_dispatcher]() {
                        return running_.load(std::memory_order_acquire) && requests_admitted() && !session_dispatcher->is_closed();
                    },
                    [&sink](const std::string& message) { return sink.write(message.data(), message.size()); },
                    [&session_dispatcher, &sink]() {
                        bool timed_out = false;
                        const bool event = session_dispatcher->wait_event(&sink, std::chrono::milliseconds(5000), &timed_out);
                        return event ? dvb::SseWaitOutcome::Event :
                            timed_out ? dvb::SseWaitOutcome::Timeout : dvb::SseWaitOutcome::Closed;
                    });
                if (!result) {
                    retire();
                    return false;
                }
                try { session_dispatcher->update_activity(); }
                catch (...) { retire(); return false; }
                return true;
            },
            [retire](bool) noexcept { retire(); });
    } catch (...) {
        retire();
        throw;
    }
}
