# Standalone native session regression

`devbench-transport-tests` compiles the exact patched cpp-mcp server and the
dependency-free DevBench test harness. It has no SKSE/CommonLib dependency and
does not contact a running game, deploy artifacts, or operate calendar/probe
custody. Build Broker must assess this new compile target and source-overlay
recipe. Compile success alone is not an assertion pass.

The receiving task may run the returned executable in isolation with a bounded
330-second process deadline. It starts its own loopback server on port37391;
an occupied port is a failure, never permission to use that listener. Preserve
exit code, stdout/stderr, exact executable hash and its source/build receipt.
Do not run this fixture concurrently with another copy.

Four cases exercise the production transport/dispatcher:

- One original POST-only session for180seconds with the deployed30second
  timeout and at least600RPCs; no GET heartbeat or reinitialization. Explicit
  DELETE succeeds, subsequent POST/DELETE404, cleanup occurs once.
- A35second admitted POST remains live across the timeout and maintenance
  tick; completion starts a fresh idle interval.
- Genuine idle expiry via the real maintenance thread, with a1second timeout
  solely to shorten that independent case. No status polling refreshes it.
- Dispatcher admission/retirement serialization, exceptional guard release,
  fresh completion timestamp and refusal to revive a retired dispatcher.

These transport assertions do not prove game-state restoration. Application
lease cleanup, requester runtime acceptance and review verdict remain separate.
