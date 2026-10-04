# Bounded calendar-rate control

`calendar` is a separate tool from `game.setTimeScale`. It writes only the
engine calendar's `TimeScale` global; it never calls the engine timer's speed
setter. It does not change hour/date, weather, camera, exposure or rendering.
The current scope holds the calendar values **observed when the hold applies**,
not values previously read by a client. Absolute time/date adjustment is absent.

## Calls and source binding

1. `{"action":"status"}` returns fresh main-thread calendar readback when
   available. Check `readbackFresh`, `available`, `worldLoaded`, `frame` and
   current runtime/artifact binding before mutation.
2. `hold` requires `owner`, `commandId`, the exact returned `binding` object,
   and integer `holdMs` in 1..300000. There is no renewal or implicit extension.
3. `release` requires the same owner and exact source binding, `leaseId` from
   the hold receipt, and a caller-selected `commandId`. A matching release of
   the most recently completed lease is repeatable without another write.

```python
hold_args = {
    "action": "hold",
    "owner": "mapping-task",
    "commandId": "unique-hold-command",
    "binding": status_result["binding"],
    "holdMs": 60000,
}
release_args = {
    "action": "release",
    "owner": "mapping-task",
    "commandId": "unique-release-command",
    "binding": hold_result["lease"]["binding"],
    "leaseId": hold_result["lease"]["id"],
}
```

The objects above use payloads returned through the caller's selected transport;
never substitute guessed IDs. The live tool schema lists every required field.
`binding` has
processSession (PID + native process creation time), PID, loadGeneration,
cellFormId and six globalFormIds, ordered year/month/day/gameHour/daysPassed/
calendarRate. Months are zero-based engine values. Internally, source storage
identities are also compared; raw pointers are never returned.

MCP ownership additionally binds its actual transport session. Stateless REST
has only the explicit cooperative owner/lease/source binding; these are audit
and coordination identifiers, **not authentication**. REST clients must not
claim transport-disconnect cleanup. No adapter disconnect callback is used.

## Receipts and failure semantics

Check `ok` and `status`, not HTTP/tool success alone. A successful hold reports
`held`, `lease.applied`, captured full calendar values and prior rate, deadline,
source binding and owner. `holdValid` additionally checks the still-identical
calendar, engine multiplier, scene and unexpired lease at the current read.
This is main-thread game-state evidence, **not an atomic rendered-frame snapshot**.
Runtime artifact identity/CSX render attribution remain separate qualifications.

`outstanding` is retained custody, whereas `leaseActive` is unexpired policy.
`expiryDue`/`cleanupPending` can be true without restoration having run. Cleanup
uses monotonic wall time and runs before the GameClock frame-counter gate, so a
serviced task can restore even if that counter has not moved. A stalled engine
main thread cannot run cleanup; it is not safe to write globals off-thread to
pretend otherwise. Disconnect recovery is therefore expiry-bounded **subject to
main-thread service**, not an instant or unconditional wall-clock guarantee.

Release restores the captured prior rate only if the same process/load/global
storage is still present and its current rate is still the owned zero. It never
rewinds calendar values. A different rate from another writer is left untouched.
Detected calendar or engine-multiplier changes invalidate scientific hold
continuity and release the owned rate without undoing those external changes.
The most recent cleanup is retained under `lastTransition`, independently of
the current status action's result. Undetected same-value/ABA writes cannot be
attributed; this is a cooperative single-owner contract, not an engine-wide lock.

Pre-load/loading-menu cleanup is attempted while old globals remain identified.
New-game/post-load generation changes invalidate old custody without writing an
old baseline into the newer session. Scene loss restores only still-identified
same-generation rate storage. Server stop requests pump cleanup; process crash,
forced termination or DLL unload cannot promise a restoration receipt.

Failed restoration stays tracked for an explicit matching release; the pump
and repeated cleanup events do not enter an automatic retry loop. A failed
save/loading-menu/pre-load/service-stop cleanup does not silently retire
uncertain custody. Unavailable readback is uncertainty, not proven source
replacement. Retry needs the same owner/session/lease and a still-matching
identified source; an actual new-load generation invalidates old custody
without writing its baseline into the new globals. Retention cannot make a
stopped endpoint, stalled thread or superseded load releasable. A not-yet-started queued request is
abandoned at its deadline. An already-started call may complete late; the
receipt says so, and any resulting hold has a finite deadline and owner/command
identity. Reconcile with status rather than blindly replay an ambiguous hold.

## Supported operating boundary

Use only a loaded, identified fixture with finite valid calendar values, a
positive calendar rate and positive engine multiplier. Missing globals/player/
cell, loading menus, stale bindings, conflicts and unsupported parameters are
refused. Engine game speed stays unchanged, but a calendar hold does not prove
all mods or weather/lighting transitions are invariant. Preserve those nuisances
and use the existing CSX weather snapshot and renderer probes independently.

**Do not save during a hold.** DevBench save admission/calendar-jump entry points
(`game.advanceTime`, `wait`, `sleep`) refuse observed outstanding custody,
including unverified cleanup. Wait/sleep check before queueing and again on
the main thread before entering their engine path, returning 409 without
starting their wait/sleep ticks. This is not an interception of a save
already queued in the VM or every engine/Papyrus/third-
party save or calendar writer. A save event ends the hold best-effort, but event
ordering does not certify that a save never serialized a zero calendar rate.
Manual/third-party saves during a hold and their persisted-state recovery are
unsupported. Release and confirm restoration before saving. No automatic
rewind, reload, weather reset or game-speed repair is performed.

The implementation uses the existing pinned CommonLibVR Calendar/TESGlobal
surface; no build recipes, lock or dependency pins change. Host-independent
policy/queued-task tests use a fake backend. Build Broker owns production and
test-executable compilation. Requesters own finite live qualification of hour/
date tolerance, unchanged animation/physics speed, restoration and actual scene
lighting; compilation and offline tests alone are not runtime acceptance.
