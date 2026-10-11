# Standalone native session regression

`devbench-transport-tests` compiles the exact patched cpp-mcp server and the
dependency-free DevBench test harness. It has no SKSE/CommonLib dependency and
does not contact a running game, deploy artifacts, or operate calendar/probe
custody. Build Broker must assess this new compile target and source-overlay
recipe. Compile success alone is not an assertion pass.

The receiving task may run the returned aggregate executable once in isolation
with a bounded 660-second process deadline. The session cases start their own
loopback server on port37391; the startup cases use port37392, and the new
committed-admission/real-adapter cases use port37393; the new same-name replacement
fixture uses port37394.
An occupied port is a failure, never permission to use that listener. Preserve
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

## Startup transaction source fixtures

Eight new cases in McpStartupTransaction_test.cpp use only their own loopback
port37392: actual bind refusal/free-port retry and repeated stop/start; thrown
HTTP queue construction with one explicit retry and preserved historical count;
null queue; blocking start stopped by another owner; fail-closed embedded stdio;
partial invalid-regex route registration refusing graph reuse; and injected first
or second worker creation failure. Factories must return the owned joinable thread
running the supplied body exactly once asynchronously, never inline or detached.
These cases compiled at exact aa5b857; no assertion execution is claimed here. The dependency
configuration seam is not a DevBench tool or permission to create runtime workers.

No test may attach to a game, delete a foreign session or retry arbitrary production
failure. Named injected failures permit the one explicit fixture retry described
above. The former 330-second session-only budget does not cover this aggregate.
Its successful session lane already includes 180s of POST-only activity plus a
35s admitted request and idle maintenance; startup preparation can wait 30s per
case. The 660-second aggregate bound permits that finite additional work, but is
still a hard deadline, not a guarantee that a failed drain terminates naturally.
Preserve timeout/hang evidence and cancel only the owned standalone fixture
through the requester's supported process wrapper; never target a game, foreign
listener or unrelated process. Do not automatically replay a failed fixture.
The manager does not run this executable; Broker compilation and requester-owned
host qualification remain distinct stages. Persistent Mapping environments need
no restart for these standalone host fixtures. A future installed startup candidate
does require changed-component/startup qualification coordinated by runtime owners.

## Review correction fixtures and qualification identity

McpCommittedAdmission_test.cpp compiles the actual McpAdapter and ToolRegistry
production translation units with the existing host-only logs stub. It uses the
same PostLoadStartup controller and retained atomic token as production, but no
SKSE hook implementation. Listening-before-publication, injected publication
failure, commitment, retained-ID quarantine and single-slot non-consumption are
checked through real /mcp POST, GET/DELETE, legacy /sse and /message entries.
Predicate exceptions fail closed through the public HTTP route.

Finite concurrent workers register160 tools and perform80 public catalogue and
80 callable requests under a20s useful-work budget, with1s connection/2s read
bounds and failure counters read after all joins. No harness CHECK runs in a
worker. A real callable registers another tool, checking that catalogue locking
is released before application code. These fixtures add a60s margin over the
previous600s aggregate budget; they do not extend that budget dynamically. A
deadlock still requires the requester's hard process deadline and owned-process
cancellation, never an unsafe detached worker. Preserve any original failure;
do not automatically replay. New compiled-source receipts are required before
requester execution or claiming these fixtures ran.

All three compiler attempts at aa5b85737c39253c7d56e2da440f417635aebb8f /
treec6bcf5e3bd0372ff31af6f75baa1c0522b4ed0b1 exited zero once. Its transport
log explicitly compiles both public-admission fixture TUs and the actual adapter/
registry. Host/transport outer jobs succeeded; production outer NONZERO_EXIT from
post-publication scratch-release lock timeout remains preserved with separate
successful cleanup reconciliation, no compiler replay. All assertions remain
unrun in these compile receipts. This is not the predecessor ed16 host-publication
recovery narrative, nor proof that the later source changes below compiled.

McpReplacementPublication_test.cpp adds ONE later source case on its own37394
loopback server: a barrier after older registry commit forces the newer same-name
replacement callback to publish first and the older callback last. It wraps the
actual adapter callback through narrow friend access, not a substitute adapter.
Final registry descriptor, real tools/list schema and tools/call handler/value
must agree. No CHECK on worker threads;3s entered/5s release waits and1s connect/
2s read bounds, no detached timeout helper. Current660s aggregate hard deadline
is unchanged. This changed-source fixture needs its own exact compile receipt;
do not substitute the older aa5 EXE or repeat a completed qualification. The
aggregate source declaration count becomes17, not an assertion-result count.
