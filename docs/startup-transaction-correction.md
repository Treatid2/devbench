# Startup transaction correction

This is the focused PR107 startup/lifetime correction on the experimental
`16dac4fc0653e1cc3c7f1275242949083b4697c2` lineage. The original
`abc57702208817423a411d3ce868a0fb8fcf99f8` handoff remains in ancestry. Intermediate
design checkpoints remain in Git history; this document describes the current
implemented contract, not those checkpoints' former validation states.

## Publication and readiness

The production `PostLoadStartup::Run` transaction guards configuration,
construction, preparation, listener startup and required publication. The entire
actual PostLoad entry has a no-throw boundary, including creation of its phase
operations. Optional discovery/reporting failure cannot undo an already committed
required publication. Diagnostic exceptions do not escape the startup boundary.

Input, menu, lifecycle, REST/MCP and host-interface exposure require explicit
readiness, not merely a non-null server pointer. The server retains its registry,
event bus and readiness atom before their addresses can escape to consumers.
Failure closes tool admission and retains/quarantines that process-lifetime owner;
it does not delete objects still referenced by installed hooks, consumers or
asynchronous work. Irreversible publication is not retried in the same process.
Retained ownership is not a claim that a failed transport has stopped.

Listener readiness and embedding commitment are distinct. The production Server
supplies an immutable `request_admission` predicate reading its retained readiness
atom. Every native MCP HTTP entry and core request dispatch checks it, including
initialize, initialized notifications, catalogue reads and both SSE variants.
Session insertion rechecks that same token under the capacity/map owner mutex.
Thus a listening but uncommitted/quarantined owner refuses public requests with
HTTP 503 before consuming a slot. A standalone server's default predicate admits
normally after listener startup; it does not change the DevBench commitment gate.
Predicate exceptions fail closed. This internal constructor seam must only read
its retained token, never acquire locks, reenter the server or perform callbacks.

Revocation refuses new requests on retained IDs as well as new initialization;
already admitted work may finish. Existing sessions retire through normal idle
expiry or owned Stop. Client DELETE is refused while quarantined, not replaced
with guessed deletion or a global reset. Inert CORS OPTIONS remains available and
proves neither MCP admission nor commitment. Irreversible PostLoad cannot recommit
automatically. These are admission rules, not cancellation or bounded-drain promises.

`PathUtf8` supplies the production bridge-discovery path conversion. Its source
fixtures include a non-ASCII JSON round-trip and empty-path handling.

## Callback and worker lifetime

Actual MCP registry/event callbacks and REST events/routes use `CallbackLease`.
Retirement first closes admission, then drains admitted calls. Already-copied
callbacks become inert without dereferencing the retired adapter. MCP serializes
callback bodies; REST uses concurrent lifetime admission so a waiting tool does
not exclude independent health/event reads. Registry consumers still require
the retained process-lifetime registry owner.

Late cross-plugin registration remains supported after clients connect. The
pinned transport uses its existing mutex for every `tools_` access: tools/list
copies a descriptor snapshot, tools/call copies its selected callable, and the
existing get_tools snapshot already holds that mutex. Formatting and application
callbacks run after unlocking. In particular, a tool may register another tool
without recursively acquiring a held catalogue mutex. Adapter retirement leases
protect lifetime; they do not substitute for collection synchronization.

The real EventBus takes its delivery barrier before copying a subscriber batch.
Unsubscribe removes the subscriber under the state mutex, releases that mutex,
then drains the barrier. A removed callback is either absent from the snapshot
or finished before retirement returns. Subscriber exceptions are isolated and
counted; an unexpected worker-entry failure closes delivery with sticky status
rather than escaping the thread entry or endlessly queueing undeliverable work.

The mirrored cpp-mcp pool owns joinable workers. Partial construction rolls back
and joins earlier workers; required growth precedes accepting new work. Explicit
`close_admission()` refuses new tasks without joining, and `shutdown()` then
drains admitted work and joins workers. The separate mirrored httplib queue uses
the same ownership primitive while preserving its waiting-job capacity bound.
Contained worker exceptions retain counters/future failure semantics; containment
alone is not an HTTP error response or proof that application work succeeded.

Bound-listener and accepted-request sockets acquire cleanup ownership before
fallible work. The production socket owners attempt shutdown/close on normal
and exceptional exit. Cleanup error codes are not proof of successful network
shutdown. Legacy SSE uses response-owned provider lifetime instead of detached
session worker threads. Session admission's capacity check and insertion are
atomic under the existing owner mutex; unpublished initialization rollback uses
the exact dispatcher identity, not a guessed or foreign session.

## Start, Stop and explicit restart

Pinned Start serializes owner lifecycle, drains the retired generation, binds
the actual socket synchronously and waits for the HTTP queue-ready handshake.
Only then does it open session admission. A free-port probe or a pointer to a
worker is not bound readiness. For the DevBench embedding, native request/session
admission additionally requires the retained PostLoad commitment token described
above. Partial route insertion poisons that instance;
it cannot append another partial route graph on retry. Completed routes are
reused by an explicit stopped restart.

Fallible route, core, maintenance and listener creation shares ordered rollback:
close session/core admission; wake dispatchers and maintenance; close the socket;
join original workers; reset listener generation after join; drain core work;
then retire remaining session state. Dependent adapters remain alive during
transport drain and are retired afterwards. Neither Stop nor rollback detaches
a worker to destroy state it may still borrow.

Blocking Start releases the lifecycle mutex while waiting for its exact
generation's completion, allowing another owner to Stop it. Stale completion
cannot stop a newer generation. Worker-entry and core-failure baselines preserve
cumulative history but distinguish a newly failed attempt. The startup thread
factory seam must run the exact body asynchronously and return its owned joinable
thread; it is not an arbitrary executor or runtime-facing interface.

The higher `dvb::Server` serializes Start/Stop with one lifecycle mutex and uses
atomic shared transport snapshots for read-only Running. A failed retained owner
does not report success merely because its pointer exists. Only actual bind
refusal advances the finite port-selection sequence; route, worker and readiness
failures are not automatically replayed. The bound port is published after
prepared listener success; tool/menu publication has its separate PostLoad gate.

Embedded stdio deliberately refuses host-owned stdin. This embedding uses
threaded HTTP and the separate bridge. Consuming an uninterruptible host input
stream cannot supply the same safe Stop contract.

## Pinned overlay and drift boundary

cpp-mcp remains pinned at `f1117d5286efe6477ddd14322703f034615b6c0e` with
httplib 0.19.0. The submodule, dependency locks and recursive pins are unchanged.
Local build-tree mirrors select the corrected implementation without modifying
the retained dependency checkout. Whole original functions/classes are retained
as comparison anchors; replacements must match uniquely and fail closed on
dependency drift. Only CRLF and the documented terminal-LF comparison boundary
are normalized. Imported comparison data intentionally preserves pinned upstream
whitespace; authored source, recipes and docs remain whitespace-checked.

The native include ordering selects mirrored headers first. An unqualified
OpenSSL worker lane fails compilation rather than silently losing the pinned
per-worker SSL cleanup. No new TLS qualification is claimed. Changed executed
recipes/generated bytes require Broker assessment, not a local build or an old
candidate's inherited qualification.

## Validation and limits

The 166bea0 focused review returned CHANGES_REQUIRED for concurrent native tool
map reads and pre-commit native MCP admission. Both findings were accepted and
the original review transaction completed. The current correction changes native
source and executed overlay/transport-target recipes; the older ed16 compilation
below does not qualify these new bytes. New source fixtures exercise the actual
McpAdapter/ToolRegistry with public concurrent catalogue/call requests, reentrant
registration, and the production-used PostLoadStartup controller with successful
or injected failed publication and the same retained readiness token. The fixture
does not execute SKSE hooks or claim a game-level publication test. Compilation,
requester assertions and a hash-distinct successor review remain separate stages.

Exact native/test/recipe candidate
`ed16b82d7bedc7d2f9e709599e5bf3e355935385`, tree
`0fbd06bbf4fcb64008069ce1aca4bd2a319ac939`, compiled once per target under exact
Broker assessment: production, pure host and standalone transport. Production
and transport qualification succeeded. Pure-host compilation succeeded, but
its outer publication job failed; the Broker recovered the original EXE/PDB
without compiling again. That recovery is supplemental artifact custody, not
a rewritten outer result or replacement canonical qualification PASS.
Subsequent documentation-only composition must keep its distinct head and
prove unchanged compiled-source closure instead of relabelling these artifacts.

Source checks verified 28 MCP and 17 HTTP unique pinned projections and exact
source/control correspondence. The focused host sources exercise production
phase, callback, bus, pool, socket, session and provider ownership primitives.
The standalone transport target additionally exercises actual bind refusal,
repeated Stop/Start, throwing/null HTTP queue creation, blocking Start/Stop,
stdio refusal, partial-route poison and either startup-thread creation failure.
Successful owned fixture startup is a throwing prerequisite before any public
OPTIONS request, so failed startup never authorizes contacting another listener.

Compilation is not assertion execution. The owner has not run these candidates;
the requester owns bounded host/transport execution and runtime acceptance.
The pure host suite does not compile the transport fixture translation unit.
Neither source checks nor compile receipts prove SKSE hooks, game behavior,
historical capacity-refusal cause, session-holder identity or scientific results.

Lifecycle/retirement APIs are owner-thread operations: never call them from
their own admitted callbacks or tasks. A callback, task or request that never
returns can still block safe drain. This correction introduces no bounded-drain
promise, unsafe detach, foreign-session deletion, global reset or saturation
experiment. Atomic/mutex coordination does not prove latency or fairness.

This isolated lineage excludes later observe and MCP logging changes. It is not
an installable replacement for the current experimental integration. A future
coherent composition must preserve those features and receive its own exact
source, build, review and requester qualification treatment. No compile result
implies review PASS, promotion, upstream publication or runtime acceptance.
