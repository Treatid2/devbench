# Loader startup review corrections

The files-only review of `86e1a29` returned two required corrections. These are
applied on the later experimental composition, preserving its loader-safe phase
split, retirement, timeout, menu and bridge work. This is not a passing verdict
for either the original or successor source and does not select deployment.

## Early registry handoff

`ToolRegistry::InstallRegistrationListenerAndSnapshot` snapshots committed tools
and installs the listener under the same registry mutex. Snapshot allocation
finishes before listener replacement, so an allocation failure does not publish
a partially installed handoff. A registration commit is covered by the snapshot
or the installed callback (possibly both); no callback runs under that mutex.

`McpAdapter::Wire` uses this operation, retaining serialized current-descriptor
reads for out-of-order replacements and lifetime-protected handlers. By-name
registration makes snapshot/callback overlap idempotent. Initial notifications
remain suppressed, and readiness is still required before announcements.

The focused real-adapter test holds the actual callback publication lease,
starts wiring, observes installed-listener state under the registry mutex, and
commits a new tool from another thread while initial snapshot publication is
blocked. The old gap ordering cannot reach the required installed state. After
release the public catalogue must have exactly one current entry and its public
handler must return the registered value. The existing inverted replacement
regression remains applicable and is selected as an affected check.

## Exact loader rollback cause

The Windows fixture marks the second startup-worker construction injection
immediately before throwing. That rollback case requires the exact marker,
factory count two, no unrelated first-worker marker, no bind failure, revoked
readiness, quarantine, no publication and owned drain. A port collision or any
earlier failure cannot satisfy the case.

A fourth load-context case deliberately fails the first startup-worker factory
through the same production startup path. It must observe generic failure and
owned cleanup while rejecting the exact second-worker predicate. This negative
control cannot pass merely because a bind or unrelated earlier startup failed.
The existing pristine startup and exact preparation-failure cases are retained.
All worker completion and cleanup remain outside `DllMain`; no detach or unload
with unproved active ownership is introduced.

Compilation, actual focused assertion results, actual SKSE scheduling and live
acceptance are separate evidence stages. No test result is asserted by this doc.
