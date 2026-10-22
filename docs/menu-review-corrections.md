# Initial menu inventory review corrections

This successor addresses the two P2 findings in
`DEVBENCH-INITIAL-MENU-839CBE1-20261019` (CHANGES_REQUIRED), atop the actual
`edbbe464a01feee530946b05ae51247f31e97c4a` experimental composition. The original
839 six-case host PASS and later loader/retirement/timeout/bridge evidence retain
their exact source scope; none disproves these reader/installation gaps.

## Blocking reader

`Tools.cpp::BlockingMenus` uses the production `ReadBlockingMenus` seam. An
initialized positive name whose current UI lookup is null yields a name-bound
`<unresolved-menu:NAME>` blocker. Only resolved benign flags (or the existing
console exemption) can be omitted. The sentinel is deliberately not the exact
`MessageBoxMenu` name, so the modal-close convenience cannot act on an unresolved
object. `noBlockingMenu`, replay prechecks and scenario assertions all consume
the same nonempty result; the ToolError fallback remains conservative.

## Subscription and completeness

`MenuInventory::Install` is the shared production installer, called only on the
existing qualified main-thread startup/milestone tasks. It registers the sink
before collecting a seed, never holds the inventory mutex while calling engine
callbacks, and records subscription separately from successful revision-checked
seed completion. Re-entry skips the registered sink, retries an incomplete seed,
and never overwrites a complete inventory. Event names remain owned and readers
remain synchronized. A snapshot without a subscription cannot establish absence.

The startup Publish path tries installation; the existing deferred input/data
milestone drain calls `RefreshMenuEventInventory` again. No worker, loader-callback
UI access, new polling loop, synthetic menu event or additional cell sink is
introduced. If UI is still unavailable at every existing milestone, recovery is
not promised: `inventoryEventsSubscribed=false`, `inventoryInitialized=false`,
`inventorySource="ui-unavailable-no-sink"` and negative guards stay unknown. A
subscribed but raced/failed seed instead reports `events-only-incomplete`.
Recording captures both fields and source provenance at start.

## Affected checks and limits

Thirteen `Menu inventory:` cases exercise the actual production container,
serializer, installer and blocking-reader seam: the original six adapted to
subscription, unavailable-to-available/duplicate install, seed-race retry without
duplicate sink, seed without subscription refusal, unresolved positive lookup,
resolved flag policy/console exemption, unavailable reader and collection-error
recovery. These are authored source until the owner admits a newly compiled host
artifact and executes the selected prefix. They do not execute engine UI,
full Tools handlers, actual SKSE callbacks or live menu traversal.

Compile only affected default-OFF native and pure-host targets through the Broker;
retain recipes, all recursive pins, overlays and unrelated topics unchanged. No
loader/retirement/transport/bridge test replay or native installation follows.
Requester-controlled live checks at a genuinely useful adoption boundary should
distinguish successful seed, seed race, unresolved object and initially absent UI.
Compilation, host assertions, source review and live acceptance remain separate.
