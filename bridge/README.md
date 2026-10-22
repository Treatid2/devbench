# devbench-bridge

A stdio MCP proxy for [devbench](../README.md). Spawned by your MCP client per-session,
it forwards `tools/list`/`tools/call` to whatever Skyrim SE/AE/VR process is currently
running devbench's REST API — so your MCP connection survives the game restarting
(the normal SKSE dev loop: rebuild → close game → relaunch), which a direct connection
to devbench's own `/mcp` endpoint does not.

It holds no port/cache state of its own — every call reads the live port from that
install's `Data/SKSE/Plugins/devbench/runtime.json` and proxies straight through, so a
live call never drifts from whatever devbench build is actually running. Under a VFS mod
manager (MO2, …) that path is virtual and unreachable from outside the manager's own
hook, so devbench also mirrors `runtime.json` to `%LOCALAPPDATA%\devbench\<se|vr>\`, which
isn't virtualized — the bridge checks both and uses whichever is freshest. `tools/list`
is the one exception: it returns `src/tools-fallback.json` (devbench's real core tool
set, baked in at build time) when no game is up, since an MCP client typically caches
`tools/list` for the whole session and can't be relied on to notice a
`tools/list_changed` push once the game starts — the client sees the full tool set from
its very first connection either way. When the game isn't up, `tools/call` returns a
clean `{ok:false, reason:"game not running"}` instead of killing your MCP session —
restart the game and keep calling, no reconnect.

`src/tools-fallback.json` is regenerated from a live devbench with
`node scripts/sync-tools-fallback.mjs [url]` (default `http://127.0.0.1:8920`) — run
this and commit the result after changing devbench's core tool registry
(`src/Tools.cpp`, `src/Capture.cpp`, `src/HostApi.cpp`, `src/ToolRegistry.h`); CI fails
the build otherwise
(`scripts/check-tools-fallback-sync.mjs`).

## Setup

If devbench is installed, the bridge is already at
`Data/SKSE/Plugins/devbench/devbench-bridge.exe` — nothing to download. Add one entry to
your MCP client's config (Claude Code's `.mcp.json`, Claude Desktop's config, etc.).

**Under a VFS mod manager (MO2, …)** that path is only reachable by processes the manager
itself launches — an MCP client spawning the bridge directly (the normal case) can't see
it. Extract `devbench-bridge.exe` from the release archive to a real, on-disk folder and
point `command` at that copy instead; `runtime.json`'s VFS-proof mirror (above) means the
extracted exe still finds the live game either way.

```json
{
  "mcpServers": {
    "devbench-se": {
      "command": "C:/path/to/Skyrim Special Edition/Data/SKSE/Plugins/devbench/devbench-bridge.exe",
      "args": ["--game", "se"]
    }
  }
}
```

Running SE and VR at once? Add a second entry pointed at the VR install with `--game vr`
(or `--install <path>` for either, if it's not one of the common Steam locations). Each
is its own bridge process — they don't share state, so both can be used concurrently.

`devbench-bridge setup --game se` prints this snippet for you (never writes to your
client config itself — you paste it in).

You can also get this exact snippet live from a running game: `GET /api/tools`'s
`mcp_bridge` field, or `Data/SKSE/Plugins/devbench/mcp-bridge.json`.

## Flags

- `--game se|vr` — target the default Steam install for that runtime.
- `--install <path>` — target a specific Skyrim install directory (required if devbench
  isn't in one of the default Steam locations, or you're running a non-default copy).
- `setup` — print the `.mcp.json` snippet instead of running as an MCP server.
- `--rest-timeout-ms <milliseconds>` — tool-call REST deadline, integer1..540000;
  default30000. Applies to the response headers AND complete body of each single
  forwarded tool call. `tools/list` discovery retains its independent30s bound.
  Invalid/missing/duplicate timeout values and unknown flags fail before starting
  stdio or issuing requests. `setup` preserves an explicitly selected value in
  its printed snippet; it still writes no configuration.

## Long calls and uncertain results

This option is per bridge process, not a native argument or an MCP per-call timeout
extension. It forwards the SAME native method/arguments ONCE, without splitting
waits or replaying. Tool requests force manual redirect handling after caller
options: no `Location` is followed, including POST-preserving307/308 and
method-transforming301/302/303. A3xx response is an uncertain, non-retryable HTTP
error, preserving the original status and JSON/raw body where the runtime exposes
them. No second request probes or qualifies the redirect target. The same deadline
still covers the original response body; a stalled redirect body can time out.
Example: `--game vr --rest-timeout-ms 124000` admits a120000ms
server operation only if the independently configured MCP client envelope is
strictly longer (e.g.125000ms). Other selected envelopes are60000/64000/65000 and
30000/34000/35000ms (server/bridge/client). A shared bridge configuration must be
large enough for its longest admitted operation. The entire owning package still
has its own600s limit; selecting540000 does not establish that other work fits.
Changing an outer client timeout alone cannot extend this bridge deadline.

The maximum is a finite bridge guard, not proof that a particular native operation,
client, artifact or SDK supports that duration. Callers must qualify the actual
selected bridge binary and client configuration. The old a1c0056/77104d76 issued
binary has NO timeout flag and remains fixed30s; new source is not its runtime.
Neither this option nor `setup` edits/reloads client config or deploys a bridge.

POST timeout, transport/read failure, malformed response or HTTP error returns
an MCP error with `restError` including code, outcome="unknown", retrySafe=false
and timeoutMs; HTTP responses also preserve status and exact parsed body (or raw
non-JSON text). A complete successful JSON response is passed through unchanged,
including native error values in HTTP200. In particular a timed-out POST is NOT
relabeled "game not running": native work may already have happened or may still
complete. Reconcile its original identity/effects; never automatically replay.
The abort bounds bridge response waiting, not native task cancellation or cleanup.

`node --test test/rest-policy.test.mjs` runs offline policy/owned-loopback tests
on Node24's native TypeScript stripping, no tsc/build output or game connection.
The test-only resolver maps proxy.ts's two relative.js imports to their exact.ts
sources during loading, then deregisters; it does not replace HTTP or production
logic. Redirect regressions exercise actual `callTool` using an owned temporary
runtime.json and two ephemeral loopback servers. They require one original POST,
unchanged arguments, zero redirect-target requests, uncertain errors for each of
301/302/303/307/308, non-overridable policy and a bounded stalled redirect body.
Standalone executable compilation, matching issued source/artifact identity and
actual stdio adapter/client qualification remain separate owner stages.

## Development

```
npm install
npm run build      # tsc, for iterating
npm run compile    # bun build --compile → dist/devbench-bridge.exe (standalone, no Node/Bun required to run it)
```

`test/` holds a throwaway mock devbench server and MCP-client smoke test for exercising
the proxy logic without a live game — not part of the shipped bridge.
