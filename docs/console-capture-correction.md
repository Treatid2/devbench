# Focused console correction candidate

This is source correction of DEVBENCH-PR106-CONSOLE-2C836DA-20261001
(report19114B/SHA ce2d54f711ceff0a16101919edd9fdeb7618f8fac0ec0915dfe8811e85f7c09c).
It is not a review pass, compiled delivery, test execution or runtime acceptance.
Original handoff2c836da and partial F2/F4 checkpointb3e7ed1 remain retained.

## F1: identity, admission, close and read

Each shared Window owns its immutable generation/fence, collector, sampler,
buffer baseline, source choice, diagnostics and fallback snapshot.
PrintDetour retains that Window before entering the production WindowAdmission
gate and formatting. Main-thread command/look/init closures also retain it and
enter the same gate; no closure borrows listener stack results. RunAndWait
returns LookView by owned JSON value. Look data is protected by the per-window
data mutex; listener source/timeout/fault flags are independent atomics so a
started late look cannot make the listener block acquiring its data mutex.

Close stops new admissions and waits at most2000ms for admitted work to drain.
Queued late commands are refused. A running command cannot be interrupted; it
retains the old window and prevents a successor until actual drain. A close
timeout preserves that closed active window and reports completion uncertainty,
not success or permission to replay. Read/new capture may finalize it only once
the actual gate is drained. Freeze copies its final data into a const Result;
read never mixes live engine fields or resets a collector. Generation checks
prevent an obsolete Freeze publishing into a successor. Every capture/read
receipt returns windowId; optional read windowId rejects a changed latest
snapshot. This is latest-snapshot retention, not historical query.

The source host regression blocks an actual production gate admission across
close/read eligibility/reopen, verifies rejected late admissions and immutable
copied output, and checks exception-safe lease release. It exercises the pure
production synchronization seam, NOT an actual engine queue/hook or game assay.

## F2/F4: formatting, memory and honest loss

Every vsnprintf traversal has independent va_copy/va_end, checks its return,
and rejects formatted results above65536 bytes before allocation. Payload
retention is20000 lines/1048576 bytes independent of the two control records.
The256-byte minimum formatting allowance preserves a recognizable end control
at full payload capacity; publication still enforces the exact payload budget.
Fallback slicing also retains bounded recent data and marks omitted data as
lossPossible. diag.printLoss separates lineLimit, byteLimit, format, allocation
and oversize. Limit counters count rejected payload lines; the other counters
count failed print events. printDropped aggregates events, NOT exact missing
lines. Sampler repeated/intermediate lines remain inherently lossy.

## F3: nonce and exact observed framing

A new128-bit lowercase hex nonce comes from Windows BCryptGenRandom with the
system-preferred RNG and a null algorithm handle. bcrypt.dll resolves only from
System32 during that call; no link-recipe change or retained borrowed pointer.
Unavailable/failed RNG has no deterministic clock/counter fallback. Begin/end
commands carry it. All sources require the
entire observed English unknown-command line, with CR/LF delimiting; substring,
old nonce, combined controls on one line, duplicate controls and offset-within-
line suffixes cannot restart or complete a window. ConsoleHook suppresses only
the exact active control commands, never a prefix. Conservative prologue
relocation checks and failed-hook fallback remain unchanged.

Windows RNG parameters and failure semantics follow the primary
[BCryptGenRandom contract](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcryptgenrandom).

Framing is deliberately based on the retained observed unknown-command response,
not an invented console printing command. Localisation or changed engine
formatting must fail closed before the payload; no engine ABI/runtime acceptance
is claimed from fixture strings. A cryptographic nonce is collision avoidance
and accidental control separation, not authentication or engine-wide exclusivity.

## Validation boundary and next steps

Static diff/descriptor parity and bounded source inspection only so far.
Existing and added host cases are source only: NOT compiled/executed at this
checkpoint. Broker owns exact production DLL and devbench-tests EXE/PDB compile;
the receiver owns assertion execution and engine framing/hook acceptance.
Source lineage must preserve actual canonical session9a93 and full f4
composition, not replace it with this worktree's older base. Keep one focused
console carrier/correction review separate from session, startup and orbit.
Inspect then ensure at the authoritative30-minute creation gate only after the
candidate/package is coherent. No polling or duplication of session review;
initial-tranche heartbeat remains PAUSED. No modlist/game/runtime action.
