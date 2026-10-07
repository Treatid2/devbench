# Startup transaction correction — partial source checkpoint

This branch starts at actual experimental integration
`16dac4fc0653e1cc3c7f1275242949083b4697c2`. It preserves the original
`abc57702208817423a411d3ce868a0fb8fcf99f8` handoff in ancestry/history.
The accepted focused PR107 finding requires a complete startup transaction,
not a widened catch. This checkpoint is not compiled, submitted, delivered,
integrated, passed or runtime accepted.

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
They are source only, not compiled or executed. No controls/pins/locks changed.

## Dependency findings and remaining source work

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
- `EventBus.h` copies subscriber callbacks before taking its delivery barrier.
  A copied batch may survive an unsubscribe barrier. The adapter lease closes
  the owner-dereference hazard, but subscriber exceptions still need an explicit
  no-throw delivery boundary; do not claim full EventBus teardown qualification.

Required next design/implementation:

1. Establish synchronous bind/listener readiness, safe partial-start cleanup,
   stopped/retry invariants and no detached worker borrowing destroyed state.
   Keep exact fail-closed overlays against the pinned dependency; do not edit
   the submodule or assume `is_running()` certifies binding. Build Broker must
   assess changed controls before compilation; this is not local-build authority.
2. Wrap the entire PostLoad path (configuration, construction, registration,
   consumer wiring, listening, discovery and reporting) in a production-used
   no-throw transaction with narrowly injectable phase operations. Treat optional
   discovery/reporting failure distinctly from required readiness failure.
3. Gate dependent input/menu/lifecycle/host exposure on explicit readiness, not
   non-null `g_server`. Do not install irreversible hooks before required stages
   commit; do not claim hook rollback without an actual API.
4. Retain process-lifetime registry/bus ownership once published to Capture,
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
