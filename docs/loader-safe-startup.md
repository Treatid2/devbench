# Loader-safe startup successor to 3d9791f

Mapping's exact loaded3d live failure has a matched-PDB main-thread join chain:
SKSE PostLoad -> PostLoadStartup -> Server::Start -> mcp::server::start ->
stop_locked -> core-pool shutdown -> std::thread::join, while new worker entries
wait at loader initialization. This is strong loader-serialization evidence,
not proof from !locks of a named lock owner. Old ordinary-host/source PASSs do
not qualify that load context. Current change starts from actual31e9 coherent
integration, excluding the independent unqualified d4 closed-entry correction.

## Thread-free callback, later owned startup

PostLoadStartup now has Prepare and Complete halves. PostLoad runs only config,
retained owner/registry preparation and interface registration. Server's EventBus
is explicitly constructed with delivery disabled; it has no thread to join on
preparation failure. Early events are retained only in the bounded recent ring,
not an unbounded pending-delivery queue or a claim of subscriber delivery.

The existing SKSE main-task interface owns deferred Complete. Its callback
retains process-lifetime operations and calls Server::Start only after the
loader callback returns. Inline PostLoad task reentry fails closed before worker
construction; a task starting at/after its five-minute admission deadline also
fails closed. A never-drained queue remains observably prepared/not-ready, not
startup success. There is no polling worker, detached bootstrap, wait/join,
listener construction or eager core pool in the loader callback.

Complete owns bus worker startup, transport construction, listener waits and
failure rollback in the safe phase. Existing retained generation/stop/drain and
publication guards are preserved, not skipped to work around the join. Hook
publication still follows listener success. Partial publication is quarantined;
escaped callback/registry owners stay retained. No new DLL teardown cleanup or
unbounded-stop guarantee is claimed for existing application handlers.

## Consumers and lifecycle milestones

Prepared HostAPI registration is available before transport readiness, so a
DataLoaded consumer can register tools even if the task queue has not drained.
No public C-ABI signatures/layout change. Existing runtime SetTimeScale is denied
until the same retained server admission token is ready. InputLoaded/DataLoaded
are latched before the readiness gate; their main-thread-only input/UI installs
run once in safe tasks, including when the notifications preceded Complete.
Observed milestones install before initial readiness publication. Later milestone
failure revokes readiness without reinstalling partial hooks or destroying
escaped owners. This is not permission to call arbitrary engine APIs on a worker.

## Qualification and limits

New Windows-only devbench-loader-fixture DLL executes the actual controller's
Prepare and thread-free EventBus mode from real DllMain. Its loader host performs
LoadLibrary, then invokes exported Complete outside loader serialization. Complete
uses the actual patched mcp::server, exercising pristine startup and injected
second-worker construction failure/rollback; explicit owned cleanup precedes
FreeLibrary. It owns only isolated loopback37395; a collision fails, never kills
another owner. The fixture does not compile SKSE main.cpp or model every consumer;
actual engine task/milestone ordering still requires source review and requester
live acceptance. Ordinary host tests cover phase split/scheduling failure and
deferred EventBus behavior, not the loader context by themselves.

All new artifacts must be exact committed Broker outputs. Compile-only results
are not assertions. Run loader host with exact issued fixture path, hard45s,
once per new binary pair; it reports two cases, pristine and rollback. Preserve
old3d19-case suite and live failure, no source-verdict transfer or live retry here.
Broker must assess the two new nondefault offline targets and lazy-bus/native
source, retain xmake/pin/generated mirror/source/artifact identities. No install,
game/VR/session action, capacity change or shared/main promotion selected.
