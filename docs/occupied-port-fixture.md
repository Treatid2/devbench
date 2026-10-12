# Occupied-port startup fixture qualification

The original ed16 and aa5 requester runs each reported the same three failed
assertions in the occupied-port case: refusal, bind-failure classification and
not-running state. Keep those exact failed results; no passing test is inferred
from this correction. The original test body also survives in6fea and3d.

The test's ordinary httplib blocker inherited the dependency's reuse option.
On Windows, SO_REUSEADDR permits another reusable socket to bind a shared
endpoint; successful bind/listen is not the exclusive reservation the test
assumed. This is a source-supported explanation of the invalid fixture premise,
not a measured socket-option snapshot from the old runs or proof of every cause.

The corrected test owner explicitly disables SO_REUSEADDR (and SO_REUSEPORT
where defined), enables Windows SO_EXCLUSIVEADDRUSE, and verifies the effective
options with getsockopt before invoking the production start method. A bind or
option-setup/readback failure throws a distinct fixture-premise error, rather
than silently skipping the test or turning it into PASS. The setter callback
does not throw while the pinned socket creation function owns an unguarded socket;
the installed callback is cleared before its local capture goes out of scope.

The actual !start, bind_failed and !is_running assertions remain unchanged,
as do the explicit freed-port retry, real public OPTIONS check and Stop-Start
ownership assertions. Only the fixture's own socket37392 is reserved. No foreign
listener, process, session DELETE, alternate transport or game is involved.

Production socket options and startup behavior are unchanged. This fixture does
not qualify production exclusivity against a reusable competing listener, general
Winsock identity/routing guarantees or a new binding policy. It exercises the
actual OS-refused bind path against a non-shareable owned reservation. A broader
production exclusive-address policy would be a separate behavioral/port-reuse
decision and requires its own compatibility assessment, not a silent test fix.

References: [Microsoft SO_REUSEADDR/SO_EXCLUSIVEADDRUSE semantics](https://learn.microsoft.com/en-us/windows/win32/winsock/using-so-reuseaddr-and-so-exclusiveaddruse)
and [SO_EXCLUSIVEADDRUSE](https://learn.microsoft.com/en-us/windows/win32/winsock/so-exclusiveaddruse).
Exclusivity must be requested before bind. Production restart behavior is not
changed; the fixture's reserved socket accepts no client connections before release.

This changes only transport-test source and this document. Exact new transport
compilation and requester assertions are required. Old production/host sources,
recipes/pins/artifacts retain their actual identities; no redundant production
or host build, relabelled new-head build/test claim, source-review replacement,
installation, launch, saturation or runtime acceptance follows.
