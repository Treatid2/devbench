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
   the pending request. Confirmation requires that request's movie, ready
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
actual menu/game state before considering a different ID. Unknown confirmations,
unrelated dialogs, expired requests and unavailable callbacks do not dispatch.

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
