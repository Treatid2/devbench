# Closed dispatcher retirement

This change is based on experimental composition `31e9c98`. It changes maintained cpp-mcp
overlays and the owned close-session fragment, never the pinned dependency.
It is a source correction, not proof of the cause of any observed live 503.

## Invariants

All table selection/removal uses the server owner mutex. Retirement then takes
the dispatcher's activity mutex, the same order as POST admission. A retirement
claim is terminal and single-winner. Explicit closure prevents new admission,
but neither idle nor explicit cleanup removes a dispatcher while an admitted
request remains active. Existing admitted handlers retain their dispatcher and
may finish; a later maintenance pass completes removal and callbacks.

Maintenance selects already-closed entries regardless of their last activity,
including when healthy idle expiry is disabled by timeout zero. Healthy entries
retain the existing idle interval and heartbeat updates. The existing ten-second
maintenance cadence is unchanged: eventual removal means a subsequent successful
maintenance pass after admitted requests drain, not immediate or bounded cleanup
of an indefinitely running handler. No maximum-age policy or capacity increase
is introduced.

Maintenance snapshots both ID and dispatcher identity. DELETE likewise retains
the instance it observed. The removal transaction rechecks that exact instance;
a stale provider/maintenance/DELETE selection cannot erase a newer same-ID
registration. Only the map-removal winner snapshots cleanup handlers. Callbacks
run after releasing both locks. Stop's existing post-worker-drain extraction
remains unchanged; failed-initialize exact-instance rollback is also unchanged.

## Offline qualification

The new `McpSessionRetirementFixture` is available only in a separately configured
offline variant (`devbench_retirement_tests=true`). The cpp-mcp target exports
`DEVBENCH_MCP_RETIREMENT_TESTS` PUBLIC so both the actual library and every
consumer see identical class definitions; no consumer-only macro/ODR mismatch.
The default production configuration has no active fixture declaration/friends
and excludes the fixture TU. Building the production DLL in the offline fixture
configuration is explicitly refused. Broker must qualify both option states in
separate managed build workspaces. No installed dependency copy is modified.

The fixture drives the actual production dispatcher, table,
`close_session`, maintenance, DELETE and POST handler. No parallel toy table,
listener, socket, game connection or native SDK is used. It controls fixture
activity timestamps and maintenance invocation instead of sleeping through real
idle intervals. The actual admitted-POST case synchronizes on handler entry,
limits its release wait to two seconds and verifies completion before retirement.
For sink-failure cases it deterministically primes the pending message generation
under the dispatcher mutex, then invokes production `wait_event`. This is a
disclosed private fixture precondition, not proof of all provider scheduling.
Both false-return and throwing sinks must be invoked exactly once with the exact
payload, report no timeout, close the dispatcher, retire with timeout zero, run
cleanup once and allow slot reuse. Sending before a zero-time wait alone cannot
exercise that pinned generation protocol; original d4 review finding is retained.

Run the issued **new exact transport-test binary** with the single argument:

```
--case-prefix=MCP closed retirement:
```

Seven focused declarations cover payload write false/throw, active POST lifetime,
cleanup races, stale instance protection, healthy activity and initialize/cap
regressions. The optional test prefix preserves no-argument full-suite behavior;
empty/unknown syntax is refused and a nonmatching prefix exits nonzero. A prefix
run is only those selected tests, never a claim that the old full suite ran.

As authored on 18 October 2026, compilation and assertions are pending Broker
owner assessment of changed maintained overlays and the transport target. Exact
literal projection is not configure, compiler or assertion evidence. No old PASS
is transferred to this code. No production DLL, install, runtime intervention,
capacity saturation experiment or clean-baseline promotion is selected here.
