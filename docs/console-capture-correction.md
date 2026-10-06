# Console capture correction status

This is a staged source correction of the focused PR106 console review, not a
complete correction candidate, review pass, or compiled/experimental delivery.

The format path now uses an independent `va_copy`/`va_end` for each traversal,
checks both return values, and rejects a result larger than65536 bytes before
allocation. The window retains at most20000 payload lines and1048576 payload
bytes, independent of its two control records. These are capture-memory limits,
not claims about engine output or its ABI. Control storage is additional and
will become exact nonce framing in the unfinished correction below.

`diag.printLoss` reports `lineLimit`, `byteLimit`, `format`, `allocation`, and
`oversize`. Limit counters count rejected payload lines; format/allocation/
oversize counters count failed print events, which might contain multiple
lines. `printDropped` is their aggregate, **not an exact missing-line count**.
Any recorded print loss sets `lossPossible`. Published payload line/byte counts
do not include markers. A small256-byte formatting allowance remains at full
payload capacity so an end-control print can still be recognized; publishing
more payload still fails its exact byte limit.

Production-used formatter/allocation seams permit host regressions of both
traversals and refused allocations. Added source cases cover long mixed varargs,
first/second-pass failure, inconsistent length, short/long allocation failure,
per-print/remaining-budget boundaries,19999/20000/20001 payload lines, byte cap,
control retention, counters and reset. These cases have **not run or compiled**.

## Unfinished before compile/review/delivery

The current staging commit does NOT resolve PR106-CONSOLE-F1 or F3. Keep the
whole console scope separate from session9a93 and startup/orbit work. Complete:

- An immutable per-window identity shared by print admission/publication,
  command/look tasks, sampler, buffer offset, source choice and frozen result.
  Close stops new admissions and drains admitted writers before publishing one
  immutable snapshot. Read cannot mix live globals or a successor's window.
- Main-thread task closures own their window and result values; the current
  `Look()` captures a stack reference and a started task may outlive timeout.
  Late queued/running tasks must not affect successor state or commands.
- Per-window unpredictable nonce with exact control-line framing, consistent
  in print, buffer, sampler, diagnostics and slicing. Substring payload, old
  marker, duplicate marker and both-tokens-on-one-line tests are required.
  Preserve fail-closed hook/prologue fallback and document engine framing/ABI
  evidence limits instead of guessing a new console command.
- A deterministic blocked admitted writer across end/close/read/reopen, using
  the actual production synchronization seam, not a disconnected model.

Only once this focused scope is coherent: commit exact final source, use Broker
for one exact production/test compilation, prepare a hash-complete focused
carrier, inspect then ensure at the service gate, and continue until pass.
Do not submit or deploy this partial checkpoint, rebuild f4/9a93, or execute
host/game tests as the manager. Requester owns runtime hook acceptance.
