-- In-tree build of cpp-mcp (https://github.com/hkr04/cpp-mcp), vendored as the
-- lib/cpp-mcp submodule pinned to f1117d5286efe6477ddd14322703f034615b6c0e.
--
-- Only the server-side TUs are compiled (mcp_message/resource/server/tool); the
-- stdio/SSE *client* implementations are intentionally omitted — devbench is a
-- server only. Upstream ships no install rules (PR #12 open), so we drive the
-- build ourselves rather than consume it as a package.
--
-- Source edits are applied into a build-tree mirror so the submodule
-- stays clean (see gen_patched_headers below):
--   1. mcp_message.h: `#include "json.hpp"` -> `#include <nlohmann/json.hpp>`,
--      so cpp-mcp and devbench share one nlohmann_json ABI (the vendored 3.11.3
--      and our 3.12.0 wrap their API in different ABI-tagged inline namespaces;
--      mixing them is an LNK2001 at link time).
--   2. mcp_server.h: insert a public `http()` getter returning the underlying
--      httplib::Server*, so the REST facade can mount routes on the MCP port
--      (the cpp-mcp-expose-http.patch, applied here as a string edit).
--   3. Exact fail-closed header/server overlays in cpp-mcp-session-activity.lua
--      protect active Streamable HTTP POSTs from idle retirement. The pinned
--      source and dependency objects are never edited by configuration.
--   4. mcp_thread_pool.h: a locally owned joinable-worker implementation, after
--      exact pinned-header comparison. This supports explicit owner shutdown
--      and safe constructor rollback; server stop wiring is a separate substep.
--   5. httplib.h: exact HTTP worker-class replacement delegates to that owned
--      pool. HTTP listener/socket rollback is still a separate substep.

local cpp_mcp_root = path.join(os.projectdir(), "lib", "cpp-mcp")
local patched_inc = path.join(os.projectdir(), "build", "cpp-mcp-patched", "include")
local patched_src = path.join(os.projectdir(), "build", "cpp-mcp-patched", "src")

target("cpp-mcp")
set_kind("static")
set_languages("c++17")
set_group("extern")

add_files(
    path.join(cpp_mcp_root, "src", "mcp_message.cpp"),
    path.join(cpp_mcp_root, "src", "mcp_resource.cpp"),
    path.join(patched_src, "mcp_server.cpp"),
    path.join(cpp_mcp_root, "src", "mcp_tool.cpp")
)

-- Patched mirror first so its mcp_message.h / mcp_server.h win over the
-- submodule's; common/ supplies httplib.h (no shared-ABI concern there).
add_includedirs(patched_inc, path.join(cpp_mcp_root, "common"), { public = true })

add_defines(
    "MCP_MAX_SESSIONS=10",
    "MCP_SESSION_TIMEOUT=30",
    "_WINSOCKAPI_",
    "_CRT_SECURE_NO_WARNINGS",
    { public = true }
)

add_cxflags("/utf-8", "/bigobj", "/W0", "/std:c++17", { force = true })
add_packages("nlohmann_json")
add_syslinks("ws2_32", "crypt32")

-- Generate the patched header mirror before compiling. on_load runs with the
-- full os/io/raise API available (unlike the description sandbox).
on_load(function(target)
    local root = path.join(os.projectdir(), "lib", "cpp-mcp")
    local out = path.join(os.projectdir(), "build", "cpp-mcp-patched", "include")
    if not os.isfile(path.join(root, "src", "mcp_server.cpp")) then
        raise("cpp-mcp submodule missing. Run: git submodule update --init --recursive lib/cpp-mcp")
    end
    os.mkdir(out)
    local overlays = import("xmake.cpp-mcp-session-activity", { rootdir = os.projectdir() }).main()
    local function overlay_once(content, edits)
        content = content:gsub("\r\n", "\n")
        for _, edit in ipairs(edits) do
            local first, last = content:find(edit.from, 1, true)
            if not first or content:find(edit.from, last + 1, true) then
                raise("cpp-mcp session activity: expected exactly one source anchor; pinned source changed")
            end
            content = content:sub(1, first - 1) .. edit.to .. content:sub(last + 1)
        end
        return content
    end
    for _, hdr in ipairs(os.files(path.join(root, "include", "*.h"))) do
        local name = path.filename(hdr)
        local content = io.readfile(hdr)
        if name == "mcp_message.h" then
            if not content:find('#include "json.hpp"', 1, true) then
                raise(
                    'cpp-mcp: expected `#include "json.hpp"` in mcp_message.h; upstream changed — review xmake/cpp-mcp.lua'
                )
            end
            content = content:gsub('#include "json%.hpp"', "#include <nlohmann/json.hpp>")
        elseif name == "mcp_thread_pool.h" then
            local original = io.readfile(path.join(os.projectdir(), "xmake", "cpp-mcp-thread-pool-original.h"))
            -- The retained text file has a terminal LF; pinned upstream does not.
            -- Normalize CRLF and at most one terminal LF, not code/other whitespace.
            if content:gsub("\r\n", "\n"):gsub("\n$", "") ~= original:gsub("\r\n", "\n"):gsub("\n$", "") then
                raise("cpp-mcp startup: pinned thread-pool source changed; owner assessment required")
            end
            content = io.readfile(path.join(os.projectdir(), "src", "PinnedMcpThreadPool.h"))
        elseif name == "mcp_server.h" then
            content = overlay_once(content, overlays.header)
            local anchor = "\nprivate:\n    std::string host_;"
            if not content:find(anchor, 1, true) then
                raise(
                    "cpp-mcp: expected private section anchor in mcp_server.h; upstream changed — review xmake/cpp-mcp.lua"
                )
            end
            local getter = "\n"
                .. "    /**\n"
                .. "     * @brief Access the underlying httplib server to register custom routes.\n"
                .. "     * Lets a consumer mount additional endpoints (e.g. a REST facade) on the\n"
                .. "     * same host/port as the MCP transport. Register routes before start().\n"
                .. "     */\n"
                .. "    httplib::Server* http() { return http_server_.get(); }\n"
                .. anchor
            content = content:gsub("\nprivate:\n    std::string host_;", getter, 1)
        end
        io.writefile(path.join(out, name), content)
    end
    -- httplib is included by mcp_server.h from this mirror before common/.
    -- Do not edit the pinned dependency or silently accept a changed class.
    local http = io.readfile(path.join(root, "common", "httplib.h"))
    local http_original = io.readfile(path.join(os.projectdir(), "xmake", "httplib-thread-pool-original.h"))
    local socket_original = io.readfile(path.join(os.projectdir(), "xmake", "httplib-socket-handler-original.h"))
    local listener_original = io.readfile(path.join(os.projectdir(), "xmake", "httplib-listener-original.h"))
    local stop_original = io.readfile(path.join(os.projectdir(), "xmake", "httplib-stop-original.h"))
    socket_original = socket_original:gsub("\r\n", "\n"):gsub("\n$", "")
    listener_original = listener_original:gsub("\r\n", "\n"):gsub("\n$", "")
    stop_original = stop_original:gsub("\r\n", "\n"):gsub("\n$", "")
    -- Establish allocation-free ownership before address-string/request work.
    -- Whole-function source comparison is the drift gate; normal close calls
    -- move into the owner's destructor instead of closing a descriptor twice.
    local socket_owned = overlay_once(socket_original, {
        {
            from = "inline bool Server::process_and_close_socket(socket_t sock) {",
            to = "inline bool Server::process_and_close_socket(socket_t sock) {\n  dvb::HttpSocketOwner<socket_t> owned(sock, detail::shutdown_socket, detail::close_socket);",
        },
        { from = "  detail::shutdown_socket(sock);\n  detail::close_socket(sock);\n", to = "" },
    })
    local http_wrapper = [[class ThreadPool final : public TaskQueue {
public:
    explicit ThreadPool(size_t n, size_t mqr = 0) : owned_(n, mqr) {}
    bool enqueue(std::function<void()> fn) override { return owned_.enqueue(std::move(fn)); }
    void shutdown() override { owned_.shutdown(); }
    std::uint64_t failed_requests() const { return owned_.failed_requests(); }
private:
    dvb::HttpTaskQueueOwnership owned_;
};]]
    -- Guard before any allocating flag/queue preparation. The inner guard
    -- closes admission on exceptional exit BEFORE the queue destructor drains;
    -- the outer guard also covers queue-constructor failure. Atomic slot claims
    -- let Stop, normal teardown and either guard coexist without double close.
    local listener_owned = overlay_once(listener_original, {
        {
            from = "inline bool Server::listen_internal() {",
            to = "inline bool Server::listen_internal() {\n  dvb::HttpListenerSocketOwner<socket_t> listener_owned(svr_sock_, INVALID_SOCKET, detail::shutdown_socket, detail::close_socket);\n  const auto startup_token = listener_startup_.CurrentToken();\n  dvb::HttpListenerStartupExit startup_exit(listener_startup_, startup_token);",
        },
        {
            from = "  is_running_ = true;\n  auto se = detail::scope_exit([&]() { is_running_ = false; });",
            to = "  dvb::HttpListenerRunningFlag listener_running(is_running_);",
        },
        {
            from = "    std::unique_ptr<TaskQueue> task_queue(new_task_queue());",
            to = "    std::unique_ptr<TaskQueue> task_queue(new_task_queue());\n    dvb::HttpListenerSocketOwner<socket_t> close_before_drain(svr_sock_, INVALID_SOCKET, detail::shutdown_socket, detail::close_socket);\n    dvb::HttpListenerStartupExit fail_before_drain(listener_startup_, startup_token);\n    if (!task_queue) { return false; }\n    if (svr_sock_ != INVALID_SOCKET) { (void)listener_startup_.PublishReady(startup_token); }",
        },
        {
            from = "        if (svr_sock_ != INVALID_SOCKET) {\n          detail::close_socket(svr_sock_);",
            to = "        if (listener_owned.Close()) {",
        },
        {
            from = "      detail::set_socket_opt_time(sock, SOL_SOCKET, SO_RCVTIMEO,",
            to = "      dvb::HttpSocketOwner<socket_t> accepted_owned(sock, detail::shutdown_socket, detail::close_socket);\n      detail::set_socket_opt_time(sock, SOL_SOCKET, SO_RCVTIMEO,",
        },
        {
            from = [[      if (!task_queue->enqueue(
              [this, sock]() { process_and_close_socket(sock); })) {
        detail::shutdown_socket(sock);
        detail::close_socket(sock);
      }]],
            to = [[      if (task_queue->enqueue(
              [this, sock]() { process_and_close_socket(sock); })) {
        accepted_owned.Release();
      }]],
        },
        { from = "    task_queue->shutdown();", to = "    (void)listener_owned.Close();\n    task_queue->shutdown();" },
    })
    local stop_owned = [[inline void Server::stop() {
  listener_startup_.Cancel();
  dvb::HttpListenerSocketOwner<socket_t> bound_socket(svr_sock_, INVALID_SOCKET, detail::shutdown_socket, detail::close_socket);
  (void)bound_socket.Close();
  is_decommisioned = false;
}]]
    http = overlay_once(http, {
        {
            from = "#define CPPHTTPLIB_HTTPLIB_H",
            to = '#define CPPHTTPLIB_HTTPLIB_H\n#include "HttpTaskQueueOwnership.h"\n#include "HttpSocketOwner.h"\n#include "HttpListenerSocketOwner.h"\n#include "HttpListenerStartup.h"',
        },
        {
            from = "  bool listen_after_bind();",
            to = [[  bool listen_after_bind();
  // The transport owner serializes lifecycle and joins old workers before begin.
  dvb::HttpListenerStartup::Token begin_listener_startup() { return listener_startup_.Begin(); }
  bool wait_listener_startup_until(dvb::HttpListenerStartup::Token token,
                                  dvb::HttpListenerStartup::Deadline deadline) {
    return listener_startup_.WaitUntil(token, deadline);
  }
  bool listener_startup_ready() const { return listener_startup_.Ready(); }]],
        },
        {
            from = "  void wait_until_ready() const;",
            to = [[  void wait_until_ready() const;
  // Owner-only, after joining the previous listener and before another bind.
  void reset_listener_after_join() {
    listener_startup_.Cancel();
    is_decommisioned = false;
  }]],
        },
        {
            from = "  std::atomic<bool> is_running_{false};",
            to = "  dvb::HttpListenerStartup listener_startup_;\n  std::atomic<bool> is_running_{false};",
        },
        { from = http_original:gsub("\r\n", "\n"):gsub("\n$", ""), to = http_wrapper },
        { from = socket_original, to = socket_owned },
        { from = listener_original, to = listener_owned },
        { from = stop_original, to = stop_owned },
    })
    -- Existing HTTP SSL per-worker cleanup is not silently removed. Current
    -- standalone DevBench is non-SSL; a future SSL lane needs its own adapter.
    http = http .. "\n#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)\n#error DevBench HTTP ownership overlay requires separate SSL worker qualification\n#endif\n"
    io.writefile(path.join(out, "httplib.h"), http)
    io.writefile(path.join(out, "HttpTaskQueueOwnership.h"),
        io.readfile(path.join(os.projectdir(), "src", "HttpTaskQueueOwnership.h")))
    io.writefile(path.join(out, "PinnedMcpThreadPool.h"),
        io.readfile(path.join(os.projectdir(), "src", "PinnedMcpThreadPool.h")))
    io.writefile(path.join(out, "HttpSocketOwner.h"),
        io.readfile(path.join(os.projectdir(), "src", "HttpSocketOwner.h")))
    io.writefile(path.join(out, "HttpListenerSocketOwner.h"),
        io.readfile(path.join(os.projectdir(), "src", "HttpListenerSocketOwner.h")))
    io.writefile(path.join(out, "HttpListenerStartup.h"),
        io.readfile(path.join(os.projectdir(), "src", "HttpListenerStartup.h")))
    io.writefile(path.join(out, "ContainedThreadEntry.h"),
        io.readfile(path.join(os.projectdir(), "src", "ContainedThreadEntry.h")))
    io.writefile(path.join(out, "SessionCapacityAdmission.h"),
        io.readfile(path.join(os.projectdir(), "src", "SessionCapacityAdmission.h")))
    io.writefile(path.join(out, "SessionInitializationState.h"),
        io.readfile(path.join(os.projectdir(), "src", "SessionInitializationState.h")))
    io.writefile(path.join(out, "SessionRegistrationRollback.h"),
        io.readfile(path.join(os.projectdir(), "src", "SessionRegistrationRollback.h")))
    io.writefile(path.join(out, "SseProviderPump.h"),
        io.readfile(path.join(os.projectdir(), "src", "SseProviderPump.h")))
    io.writefile(path.join(out, "ImmutableCallbackSnapshot.h"),
        io.readfile(path.join(os.projectdir(), "src", "ImmutableCallbackSnapshot.h")))
    os.mkdir(patched_src)
    -- Full-function comparison precedes the smaller session overlays. A changed
    -- dependency implementation cannot silently select the local SSE producer.
    local sse_original = io.readfile(path.join(os.projectdir(), "xmake", "cpp-mcp-sse-original.cpp"))
    local sse_owned = io.readfile(path.join(os.projectdir(), "src", "PinnedMcpSseHandler.inc"))
    local server_source = overlay_once(io.readfile(path.join(root, "src", "mcp_server.cpp")), {
        { from = sse_original:gsub("\r\n", "\n"):gsub("\n$", ""), to = sse_owned },
    })
    for _, replacement in ipairs({
        { original = "cpp-mcp-start-original.cpp", owned = "PinnedMcpStart.inc" },
        { original = "cpp-mcp-stdio-original.cpp", owned = "PinnedMcpStdio.inc" },
        { original = "cpp-mcp-stop-original.cpp", owned = "PinnedMcpStop.inc" },
        { original = "cpp-mcp-close-session-original.cpp", owned = "PinnedMcpCloseSession.inc" },
    }) do
        local original = io.readfile(path.join(os.projectdir(), "xmake", replacement.original))
        local owned = io.readfile(path.join(os.projectdir(), "src", replacement.owned))
        server_source = overlay_once(server_source, {
            { from = original:gsub("\r\n", "\n"):gsub("\n$", ""), to = owned },
        })
    end
    io.writefile(path.join(patched_src, "mcp_server.cpp"),
        overlay_once(server_source, overlays.source))
end)
target_end()
