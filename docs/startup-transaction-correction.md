# Startup transaction correction — partial source checkpoint

This branch starts at actual experimental integration
`16dac4fc0653e1cc3c7f1275242949083b4697c2`. It preserves the original
`abc57702208817423a411d3ce868a0fb8fcf99f8` handoff in ancestry/history.
The accepted focused PR107 finding requires a complete startup transaction,
not a widened catch. The prior exact phase checkpoint `e312ae0` compiled both
production and host targets under Broker qualification on 9 October, without
assertion execution. Subsequent EventBus/pool source below has not been compiled;
that earlier compile does not cover this new tree. Neither checkpoint is a
complete correction, review submission, delivery, integration or runtime pass.

## Implemented substep

`CallbackLease` is used by actual MCP registry/event callbacks and REST event
callbacks and all four REST HTTP routes. Retirement closes admission first,
then drains admitted calls, and makes already-copied callbacks inert. Clearing registry/bus storage alone does not
revoke copies. The MCP tool handlers retain the registry rather than an adapter
address; the registry must remain alive until all handlers/consumers retire.
Server stop ordering retires adapters before transport destruction. This does
not prove transport quiescence. Owners cannot retire themselves from their own
admitted callbacks; all current adapter retirement is on the server owner path.

The shared production lease tracks active calls separately from callback-body
serialization. MCP retains serialized bodies; REST chooses concurrent lifetime
admission and protects its lifecycle data with the existing lifecycle mutex.
Thus a waiting REST tool does not take the lifetime gate's lock away from health
or event reads. Copied routes return 503 after retirement without dereferencing
the adapter. This does not establish listener/transport quiescence, a bounded
drain of a hung tool, or safety of consumers that retain the registry itself.

`PathUtf8` is the production bridge-discovery conversion, with source tests for
non-ASCII JSON round-trip and empty paths. Nine regression cases exercise this
conversion, throwing/admitted/copied callback retirement and the actual registry.
They also cover both admission modes and health admission while a tool waits.
These are tests of the production lease, not actual httplib request execution;
the host-independent target does not compile RestAdapter.cpp.
These nine cases compiled at exact `e312ae0` but were not executed. No
controls/pins/locks changed.

## Dependency findings and remaining source work

9 October production phase-boundary substep:

- `PostLoadStartup::Run` is now the controller used by actual main.cpp. It guards
  configuration, construction, preparation, listener result and publication, and
  suppresses diagnostic exceptions. Seven new source cases exercise this very
  sequence with five phase failures, false listener result, disabled configuration,
  optional reporting failure, retained escaped owner and reentrant/repeated calls.
- The whole actual kPostLoad branch returns inside a no-throw boundary, including
  construction of its operations/config object. Other SKSE-dependent input/menu/
  lifecycle and interface-delivery paths require explicit readiness, not merely
  a non-null process-lifetime server. All inherited hook installs remain present.
- The owner is retained before any consumer pointers escape; a failed attempt
  denies REST/MCP tool admission and menu publication, does not destroy the owner,
  and does not repeat irreversible publication in the same process. This is a
  quarantine, not a claim that transport has stopped. Optional discovery writing
  occurs after committed publication, so it cannot falsely undo that commit.
- REST checks readiness inside its existing concurrent lease; MCP checks it before
  tool execution/notifications. The readiness atom is server-owned and retains
  the same process-lifetime requirement as the registry used by escaped handlers.

These 16 source cases across the two startup files compiled at exact `e312ae0`.
None was executed by the owner. Phase tests are not game/SKSE/actual HTTP or
pinned-transport tests.
Critically, the controller still relies on `Server::Start`'s unqualified listener
result. Its bind proof, exception cleanup, Stop/Start retry and dependency-worker
retirement are NOT fixed by this substep. Do not compile and call this full PR107
acceptance, publish it to integration, or submit a prematurely complete review.

The pinned cpp-mcp is `f1117d5286efe6477ddd14322703f034615b6c0e`. Its current
session-activity overlays affect POST admission and session retirement only;
they do not repair server startup/stop.

- `src/mcp_server.cpp:120–214`: nonblocking start returns after spawning, not
  after bind success. Maintenance thread creation precedes listener creation.
  A partial throw can leave a joinable maintenance thread with `running_` false.
- `:216–346`: stop early-returns on false `running_`, clears dispatcher maps
  before a loop over those maps, and uses detached join helpers retaining local
  references. These paths cannot serve as a proven failed-start rollback.
- `include/mcp_server.h:451`: `running_` is a plain bool shared across workers.
- REST routes now use revocable concurrent admission retaining independent state.
  Dependency-owned listener/handler lifetime still requires a proven cleanup path;
  retiring an adapter is not proof of transport or escaped-registry retirement.
- The EventBus substep below addresses the copied-batch ordering and exception
  boundary. It does not establish transport quiescence or bounded socket/drain
  completion, and must not be reported as complete startup teardown qualification.

9 October EventBus source substep, after the exact `e312ae0` compile return:

- The actual worker takes the delivery barrier before copying subscriber callbacks.
  Unsubscribe removes under the state mutex, releases that mutex, then drains the
  delivery barrier. Thus either the snapshot omits the removed subscriber or its
  invocations finish before Unsubscribe returns. The old copy-before-barrier gap
  is closed without holding the state mutex during subscriber/socket execution.
- Subscriber exceptions are isolated per invocation and counted without allocating
  diagnostics; other subscribers and later events continue. Unexpected queue,
  snapshot-copy or wait exceptions cannot escape the thread entry point: delivery
  stops with an explicit atomic status rather than terminating the process.
  Publishers keep the bounded recent ring but do not endlessly queue undeliverable
  notifications after that terminal worker failure. No restart is attempted.
- Three source fixtures exercise the real EventBus: throwing subscriber continuity,
  admitted callback retirement/later non-invocation, and a throwing std::function
  copy during the production snapshot. Fixture waits are finite; none was run.
  There are now 19 startup/lifetime source cases, with the new three uncompiled.
- Unsubscribe/destruction remain owner-thread operations, not calls from a bus
  subscriber. A subscriber that never returns can still block retirement; this
  change does not promise a bounded drain or authorise destroying a live worker.

9 October worker-pool ownership source substep:

- The pinned thread-pool constructor can fail after earlier detached workers
  start. Its counter increments before a potentially throwing thread creation;
  destructor-only draining also occurs too late in the server's reverse member
  destruction order (session/cleanup/maintenance state follows the pool member).
- `PinnedMcpThreadPool.h` supplies the same pool surface through a fail-closed
  build-tree header mirror. Workers are joinable owned slots, not detached
  borrowers of pool state. Constructor rollback signals and joins all previously
  started workers before the exception escapes. Required growth occurs before
  enqueue accepts a task, so a failed spawn cannot orphan newly dispatched work.
  Active counts include only successfully returned owned threads; idle retirement
  and bounded slot reuse use one queue-mutex decision.
- Explicit owner `shutdown()` closes admission and drains tasks/joins workers;
  repeated shutdown and explicit restart use one lifecycle lock. Application
  exceptions retain packaged-task/future semantics. Unexpected worker failure
  closes admission and abandons pending futures rather than reporting success.
  This does not imply a bounded shutdown of an application task that never returns;
  shutdown/restart from a pool task is prohibited.
- Three source fixtures instantiate the very header used by the generated
  production mirror: partial constructor worker-creation failure, required-growth
  failure before task admission, and future/drain/stopped/restart behaviour.
  They are uncompiled/unrun. Total startup/lifetime source cases is now22.
- The pinned submodule is unchanged. `xmake/cpp-mcp.lua` compares its entire
  original pool header against `xmake/cpp-mcp-thread-pool-original.h` before
  mirroring the local header. Only CRLF and an optional terminal LF are normalised.
  These changed executed build controls and generated pool bytes require NEW
  Broker assessment; private exact `e312ae0` qualification does not cover them.
  The retained original header intentionally preserves pinned upstream trailing
  whitespace. Static diff checking excludes that exact comparison-data file;
  authored source/recipe/docs remain checked without changing Git whitespace policy.
- Server stop has NOT yet been wired to explicit pool shutdown before state
  retirement, and its listener/maintenance/SSE/session cleanup is still pinned.
  This pool substep must not be claimed as safe complete Server destruction,
  truthful bound readiness, Stop/Start or final PR107 review readiness.

Required next design/implementation:

1. Establish synchronous bind/listener readiness, safe partial-start cleanup,
   stopped/retry invariants and no detached worker borrowing destroyed state.
   Keep exact fail-closed overlays against the pinned dependency; do not edit
   the submodule or assume `is_running()` certifies binding. Build Broker must
   assess changed controls before compilation; this is not local-build authority.
   Wire explicit pool shutdown before session/cleanup/maintenance state retirement;
   verify constructor rollback and worker ownership together with all server tasks.
2. Implemented phase boundary, pending complete transport qualification: wrap the entire PostLoad path (configuration, construction, registration,
   consumer wiring, listening, discovery and reporting) in a production-used
   no-throw transaction with narrowly injectable phase operations. Treat optional
   discovery/reporting failure distinctly from required readiness failure.
3. Implemented publication gate, dependent on truthful listener readiness: gate dependent input/menu/lifecycle/host exposure on explicit readiness, not
   non-null `g_server`. Do not install irreversible hooks before required stages
   commit; do not claim hook rollback without an actual API.
4. Implemented failure retention; pending transport stop/retry proof: retain process-lifetime registry/bus ownership once published to Capture,
   HostApi, Autorun, game sinks, console hook or watchdog. Clear admission or
   intentionally retain failed/quarantined owners; do not delete a Server while
   escaped consumers or async work still refer to it. Avoid retrying irreversible
   stages or claiming stopped state while a transport listener remains alive.
5. Add production-used failure seams and regression source for construction,
   wiring, bind/start, post-start discovery/reporting, repeated Stop/Start,
   readiness gating, copied callbacks and failure after consumer publication.
   Broker compiles exact committed candidates; receiver owns assertion execution.

Only after the whole accepted finding is implemented and exact compilation is
admitted should a new hash-distinct focused PR107 cycle be prepared. This partial
checkpoint does not replace the independent console review's exact PASS at16dac.
Experimental source remains `16dac`; reviewed main remains `32823e4`. No runtime
command, replay, deployment, upstream publication or initial-heartbeat resume.
