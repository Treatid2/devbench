-- Out-of-tree CMake build of gopher-mcp (https://github.com/GopherSecurity/gopher-mcp),
-- vendored as the lib/gopher-mcp submodule. This is the MCP 2.0 (2026-07-28 spec)
-- candidate replacement for cpp-mcp (see xmake/cpp-mcp.lua) — SPIKE, not yet wired
-- into the rest of the build.
--
-- Unlike cpp-mcp (four hand-picked TUs compiled as a plain xmake target), gopher-mcp
-- is a full CMake project with its own dependency resolution (libevent, OpenSSL) and
-- no small in-tree-buildable subset, so it is driven via xmake's CMake package tool
-- instead of being inlined as a target.
--
-- We only need the C API (BUILD_C_API_STATIC -> gopher_mcp_c_static): consuming the
-- C++ headers directly would reopen the same shared-ABI problem cpp-mcp's
-- mcp_message.h patch works around (gopher-mcp bundles its own JSON handling), and
-- the C API is the boundary the upstream project itself recommends for embedding.

add_requires("libevent", { configs = { openssl = true, thread = true } })
add_requires("openssl")

package("gopher-mcp")
set_kind("library", { headeronly = false })
set_sourcedir(path.join(os.projectdir(), "build", "gopher-mcp-patched", "source"))

add_deps("cmake")
add_deps("libevent", "openssl")

-- Upstream bug (src/c_api/CMakeLists.txt): `gopher_mcp_c` is built SHARED
-- unconditionally, never gated on BUILD_SHARED_LIBS, and gopher_mcp_c_static's
-- OUTPUT_NAME collides with it -- both land on gopher_mcp_c.lib on Windows,
-- so ninja fails with "multiple rules generate src/c_api/gopher_mcp_c.lib".
-- Patched into a build-tree source mirror (same technique as cpp-mcp.lua's
-- header mirror) so lib/gopher-mcp stays a clean, unmodified submodule.
on_load(function(package)
    local root = path.join(os.projectdir(), "lib", "gopher-mcp")
    local out = path.join(os.projectdir(), "build", "gopher-mcp-patched", "source")
    if not os.isfile(path.join(root, "CMakeLists.txt")) then
        raise("gopher-mcp submodule missing. Run: git submodule update --init --recursive lib/gopher-mcp")
    end
    if not os.isdir(out) then
        os.cp(root, out)
    end
    local capi = path.join(out, "src", "c_api", "CMakeLists.txt")
    local content = io.readfile(capi)
    local marker = "endif() # BUILD_SHARED_LIBS (devbench patch)"
    if not content:find(marker, 1, true) then
        local start_anchor = "# Create shared library for C bindings\nadd_library(gopher_mcp_c SHARED"
        local start_pos = content:find(start_anchor, 1, true)
        if not start_pos then
            raise(
                "gopher-mcp: expected shared-library anchor in src/c_api/CMakeLists.txt; upstream changed — review xmake/gopher-mcp.lua"
            )
        end
        local end_anchor =
            "if(NGHTTP2_FOUND)\n    if(NGHTTP2_LIBRARY_DIRS)\n        target_link_directories(gopher_mcp_c PRIVATE ${NGHTTP2_LIBRARY_DIRS})\n    endif()\n    if(NGHTTP2_LIBRARIES)\n        target_link_libraries(gopher_mcp_c PRIVATE ${NGHTTP2_LIBRARIES})\n    endif()\nendif()\n\n# Compile features\ntarget_compile_features(gopher_mcp_c PRIVATE cxx_std_17)"
        local end_start, end_pos = content:find(end_anchor, start_pos, true)
        if not end_start then
            raise(
                "gopher-mcp: expected nghttp2 block end in src/c_api/CMakeLists.txt; upstream changed — review xmake/gopher-mcp.lua"
            )
        end
        content = content:sub(1, start_pos - 1)
            .. "if(BUILD_SHARED_LIBS)\n"
            .. content:sub(start_pos, end_pos)
            .. "\n"
            .. marker
            .. content:sub(end_pos + 1)
        io.writefile(capi, content)
    end

    -- Upstream bug (src/c_api/mcp_c_api_connection.cc): a #if/#else/#endif
    -- sits inside the TRY_WITH_RAII_NULL({ ... }) macro argument -- a
    -- preprocessor directive inside a macro argument is not portable C++;
    -- MSVC rejects it (GCC/Clang silently accept it as an extension) with
    -- "invalid character: possibly the result of a macro expansion". Our
    -- build always has MCP_HAS_LLHTTP=1, so collapse to the #if branch.
    local conn = path.join(out, "src", "c_api", "mcp_c_api_connection.cc")
    local conn_content = io.readfile(conn)
    local conn_marker = "// devbench patch: MCP_HAS_LLHTTP branch collapsed (MSVC macro-argument #if)"
    if not conn_content:find(conn_marker, 1, true) then
        -- there are two `#if MCP_HAS_LLHTTP` guards in this file (an include
        -- guard, then the offending one in the switch) -- anchor past the switch
        local switch_anchor = "switch (transport_config->type) {\n"
        local switch_pos = conn_content:find(switch_anchor, 1, true)
        if not switch_pos then
            raise(
                "gopher-mcp: expected switch anchor in mcp_c_api_connection.cc; upstream changed — review xmake/gopher-mcp.lua"
            )
        end
        local block_start_anchor = "#if MCP_HAS_LLHTTP\n"
        local block_start = conn_content:find(block_start_anchor, switch_pos, true)
        if not block_start then
            raise(
                "gopher-mcp: expected MCP_HAS_LLHTTP anchor in mcp_c_api_connection.cc; upstream changed — review xmake/gopher-mcp.lua"
            )
        end
        local else_anchor =
            '#else\n      case MCP_TRANSPORT_HTTP_SSE: {\n        ErrorManager::SetError(MCP_ERROR_NOT_IMPLEMENTED,\n                               "HTTP+SSE transport requires llhttp library");\n        return nullptr;\n      }\n#endif\n'
        local else_start, else_end = conn_content:find(else_anchor, block_start, true)
        if not else_start then
            raise(
                "gopher-mcp: expected #else/#endif anchor in mcp_c_api_connection.cc; upstream changed — review xmake/gopher-mcp.lua"
            )
        end
        conn_content = conn_content:sub(1, block_start - 1)
            .. conn_marker
            .. "\n"
            .. conn_content:sub(block_start + #block_start_anchor, else_start - 1)
            .. conn_content:sub(else_end + 1)
        io.writefile(conn, conn_content)
    end

    -- Upstream bug (src/c_api/mcp_c_filter_chain.cc): uses std::optional
    -- without including <optional> -- pulled in transitively on GCC/Clang's
    -- STL header graph, not MSVC's, so MSVC fails with "'optional': is not
    -- a member of 'std'" (and cascading errors on AsyncRequestQueue::dequeue).
    local fchain = path.join(out, "src", "c_api", "mcp_c_filter_chain.cc")
    local fchain_content = io.readfile(fchain)
    local include_anchor = '#include "mcp/c_api/mcp_c_filter_chain.h"\n'
    if not fchain_content:find("#include <optional>", 1, true) then
        local pos = fchain_content:find(include_anchor, 1, true)
        if not pos then
            raise(
                "gopher-mcp: expected top include in mcp_c_filter_chain.cc; upstream changed — review xmake/gopher-mcp.lua"
            )
        end
        fchain_content = fchain_content:sub(1, pos + #include_anchor - 1)
            .. "\n#include <optional> // devbench patch: MSVC doesn't transitively pull this in\n"
            .. fchain_content:sub(pos + #include_anchor)
        io.writefile(fchain, fchain_content)
    end
end)

on_install(function(package)
    local configs = {
        "-DBUILD_SHARED_LIBS=OFF",
        "-DBUILD_STATIC_LIBS=ON",
        "-DBUILD_C_API=ON",
        "-DBUILD_C_API_STATIC=ON",
        "-DGOPHER_MCP_BUILD_TESTS=OFF",
        "-DBUILD_TESTS=OFF",
        "-DBUILD_EXAMPLES=OFF",
        "-DBUILD_BINDINGS_EXAMPLES=OFF",
        "-DGOPHER_MCP_INSTALL=ON",
        "-DMCP_STRICT_WARNINGS=OFF",
    }
    import("package.tools.cmake").install(package, configs)
    -- upstream's install() rules don't export llhttp.lib even though
    -- gopher-mcp.lib depends on its symbols -- pull it from the CMake
    -- build tree by hand.
    local llhttp_lib = path.join(package:builddir(), "llhttp.lib")
    if os.isfile(llhttp_lib) then
        os.cp(llhttp_lib, path.join(package:installdir(), "lib", "llhttp.lib"))
    end
    package:add("includedirs", "include/gopher-mcp")
    -- link order matters for MSVC (no round-tripping between static libs):
    -- the C API depends on the C++ SDK core, which depends on its event loop
    -- and logging, which pull in fmt and (via HTTP/2 support) nghttp2/llhttp.
    package:add(
        "links",
        "gopher_mcp_c",
        "gopher-mcp",
        "gopher-mcp-event",
        "gopher-mcp-logging",
        "fmt",
        "nghttp2",
        "llhttp"
    )
end)

on_test(function(package)
    -- NOT mcp_client_create/mcp_server_create: src/c_api/mcp_c_api_client.cc
    -- and mcp_c_api_server.cc are commented out of MCP_C_API_SOURCES upstream
    -- (mid-refactor to the new opaque-handle API per their own TODO) and are
    -- stale against the current header signatures -- those two symbols do
    -- not currently exist in the built static lib. mcp_init is implemented
    -- and is enough to prove the link set here is otherwise correct.
    assert(package:has_cfuncs("mcp_init", {
        includes = "mcp/c_api/mcp_c_api.h",
        configs = {
            includedirs = package:installdir("include/gopher-mcp"),
            linkdirs = package:installdir("lib"),
            links = {
                "gopher_mcp_c",
                "gopher-mcp",
                "gopher-mcp-event",
                "gopher-mcp-logging",
                "fmt",
                "nghttp2",
                "llhttp",
            },
        },
    }))
end)
package_end()

add_requires("gopher-mcp")
