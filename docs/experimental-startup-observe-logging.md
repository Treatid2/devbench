# Experimental startup / observe / logging composition

This composition merges the isolated startup correction into the existing
experimental integration. It is not reviewed fork-main promotion, an upstream
submission, an installed-runtime replacement, or incident-cause proof.

Inputs are experimental e4fd3133f88d2e925a0c6d08d573a318ced4ac98 and startup
8e53fb636e5faaaeb717cfdb51ca808c1c46ebac, with shared base
16dac4fc0653e1cc3c7f1275242949083b4697c2. The former includes the native/fallback
input observe contract and retained-log initialization correction. The latter
includes transactional startup, joined worker/socket/session ownership,
readiness-gated admission and same-name registry publication convergence.
Their prior compile/review/test receipts apply to their original identities.

## Compatibility resolutions

- `main.cpp` keeps the startup transaction and input/host readiness gates.
  It also retains the independent MCP logger, single rotating file owner,
  initialization-stage diagnostics and disabled CommonLib automatic logger.
  MCP logging is configured once in the transaction's configuration phase,
  before constructing the transport, including when the server is disabled.
- `cpp-mcp.lua` keeps all exact startup header/function/HTTP ownership drift
  guards and the pinned logger replacement / generated `McpLog.h` copy.
- Session overlays retain parser-body redaction. The old capacity-check log
  anchors no longer exist because admission is now one locked transaction.
  Both Streamable HTTP and legacy SSE instead log refusals at that boundary,
  reporting transport, reason, pre-admission count and limit without handles.
  The helper snapshots the count under its owner mutex; formatting and sink
  work occur after unlocking. A later retirement can change the live count;
  the recorded value describes the failed admission, not current occupancy.

The policy, limit (10), timeout (30 seconds), request-admission predicates and
retirement semantics are not changed by the count snapshot. No session is
deleted to make room and no target mutation is replayed. Two additional pure
host declarations cover count snapshots and readiness-versus-capacity refusal.
They are source coverage until exact compiled assertions are returned.

## Qualification boundaries

This merge changes actual generated dependency recipes and needs a new exact
Broker control assessment and compilation. A source projection is not Lua
configuration, compiler success, host assertions or runtime correctness.
The composition requires its own focused compatibility/resolution review;
neither parent's PASS covers these new resolved bytes. Preserve all original
failed jobs and their reconciliations; do not relabel old binaries as this head.

Mapping retains direct native discovery, assertion/live qualification and
interpretation; Auto retains environment/custody; Broker owns compilation.
Changed loaded DLLs require a real component-reload boundary chosen by the
runtime owner. Ordinary mapping can continue on its current admitted runtime
while this candidate is compiled/reviewed; this document requests no restart.
No capacity-saturation experiment, foreign session DELETE, installation,
deployment, game launch or scientific acceptance follows from this source.
