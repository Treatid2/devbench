# Genuine New Game in Skyrim VR

`game` with `action: "newGame"` drives the ordinary VR main-menu
confirmation and fade, rather than using `coc` or loading a save. The adapter
is VR-only and fails closed if the active movie's fields, entry list or native
callbacks differ from the supported menu contract.

The route is source-grounded in the original VR `interface/startmenu.swf`:
`_root.MenuHolder.Menu_mc` owns `MainList`; New has semantic entry ID 1,
not a fixed displayed row. The native `NEW` callback requests confirmation.
`onAcceptPress` in `MainConfirm` checks the selected entry and schedules
`StartNewGame` through the normal fade completion callback. The adapter does
not invoke native `StartNewGame` directly or answer `MessageBoxMenu`.

## Sequence

1. Inspect readiness:

   ```json
   { "action": "newGame", "phase": "inspect" }
   ```

   Require `mainMenuOpen: true`, `state: "Main"` and `readyToRequest: true`.

2. Request using a fresh caller-generated UUID:

   ```json
   {
     "action": "newGame",
     "phase": "request",
     "requestId": "caller-generated-unique-uuid"
   }
   ```

   This selects the unique enabled New entry through the list's setter,
   verifies the selected semantic ID, and calls `NEW` with the correct
   response-ID argument. Retain the request ID and receipt.

3. Inspect until the same menu reaches `state: "MainConfirm"` and
   `readyToConfirm: true`, within a bounded deadline. Intermediate animation
   states are not readiness. Do not repeat the request while waiting.

4. Confirm once:

   ```json
   {
     "action": "newGame",
     "phase": "confirm",
     "requestId": "caller-generated-unique-uuid",
     "confirmNewGame": true
   }
   ```

   The request expires after 60 seconds. A replaced or closed menu invalidates
   the pending request, even if the same movie is reopened between tool calls.
   An intervening MessageBoxMenu also invalidates it (`menuInterrupted`).
   Confirmation requires that request's movie, ready
   confirmation state and still-selected New entry. It invokes normal
   acceptance and verifies `strFadeOutCallback == "StartNewGame"`.

5. Verify actual initialization separately: observe the expected character
   creation menu and loaded player/cell state appropriate to the modlist.
   `accepted: true`, `phase: "dispatched"` is dispatch evidence only;
   `completed` remains false. Do not require a transient lifecycle event that
   a client could miss.

## Response loss and failure

Inspect a specific request receipt with `phase: "inspect", requestId: <id>`.
Repeated known request IDs return their receipt without repeating selection,
`NEW` or acceptance. A mutation is marked `dispatchUncertain` before entering
GFx; that ID cannot be replayed after an exception or lost response. Inspect
actual menu/game state; an unresolved receipt prohibits a different ID. Unknown confirmations,
unrelated dialogs, expired requests and unavailable callbacks do not dispatch.

Uncertainty is a process-lifetime barrier to **all fresh request IDs**, not just
same-ID replay protection. `dispatchUncertain` and `unresolvedDispatch:true`
survive expiry, closed/replaced movies and intervening modals; cleanup adds
`invalidationReason` without rewriting the phase. General inspection reports
`unresolvedRequestId` and `newRequestsBlocked`, including when the Main Menu
is closed. It reports neither request nor confirmation readiness for an
unresolved mutation. The barrier begins before selection enters GFx, so partial
selection/NEW failure is covered as well as partial confirmation failure.

Only definitive completion and post-dispatch verification by that same running
task can clear its uncertainty. If it cannot complete definitively, restart the
game process through your owned session workflow. There is no force/reset or
fresh-ID escape hatch. Menu changes and elapsed time are not proof that no
mutation occurred. A merely `requested` confirmation that has not begun
acceptance can still expire normally. `accepted:true` remains dispatch evidence,
never proof of world entry.

The process retains at most 64 request receipts; when full it rejects new
requests instead of forgetting identities and risking replay. Only one pending
movie is retained. Request IDs are correlation/idempotency metadata, not
authentication; DevBench remains a local-only development service.

## Qualification

Source inspection is not runtime qualification. Verify save-present and
save-absent starts, intermediate-animation rejection, duplicate request and
confirmation receipts, unrelated/closed-menu rejection, and genuine character
creation using the exact compiled candidate and owned test profile. Keyboard
and controller changes are independent; this action injects neither.
