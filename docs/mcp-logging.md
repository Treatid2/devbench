# MCP transport logging

DevBench's pinned server-side MCP logger writes through the retained DevBench
file sink, tagged `[mcp]`. It does not redirect process/CRT stderr, capture other
plugins' output, add a diagnostic endpoint, or record an earlier unretained run.
The logger replacement is generated into the build mirror only after comparing
the entire original logger against the unchanged dependency pin.

Set `mcpLogLevel` in `Data/SKSE/Plugins/devbench/config.json` before the next
ordinary owned game launch. Configuration is read once before transport startup;
this option does not authorize restarting a game or changing another owner's
session. Old configurations gain the new default through normal migration.

```json
{
  "logLevel": "info",
  "mcpLogLevel": "warn"
}
```

`warn` is the default; `error`, `info`, `debug`, and `off` are also supported.
Unknown strings and non-string values fall back to `warn` without resetting
unrelated configuration. Startup configuration diagnostics use the ordinary
logger at its current threshold. In particular, invalid-string and
retained-sink-unavailable warnings occur after ordinary `logLevel` is applied,
so ordinary `error` or `off` suppresses them; the fallback still takes effect.
MCP event filtering is independent of the ordinary `logLevel` (including ordinary
`off`). Disabling MCP detail filters
before message formatting. It does not eliminate argument evaluation at a C++
call site, change transport semantics, or guarantee zero overhead.

Warnings/errors include session-capacity refusals with transport/count/limit,
protocol failures and maintenance/cleanup exceptions. `info` also enables existing
client/protocol initialization, server lifecycle, HTTP method/status and session
retirement messages. It is verbose: ordinary calls already generate several
library messages. `debug` includes the existing library's additional detail; it
does not enable packet capture or raw request/response payload tracing.

Output uses the normal SKSE `devbench.log` location, shared with ordinary logs.
DevBench disables CommonLib's automatic file logger before installing its own
sink: two owners must not open/truncate/rotate the same file at startup. The
candidate logger's level, flush policy and pattern are configured before it is
published as the ordinary logger; MCP then shares that one sink.
Retention is now bounded: current file up to5MiB, plus three rotated archives
(`devbench.1.log` through `devbench.3.log`), rotating at startup instead of
truncating the previous run. This is about20MiB, not a guarantee that every run is
retained. Record entries are capped at2048 bytes before the `[mcp]` prefix and
logging metadata. Rotation boundaries may include formatting overhead. Preserve
needed files through the requesting task's existing evidence workflow.

Messages are single-line printable ASCII; controls/ANSI and non-ASCII peer bytes
are replaced, so Unicode client metadata is intentionally lossy. Canonical UUID
session handles are masked, including possible prefixes at the truncation edge.
There is no reversible session identifier or new foreign-session close handle.
Request-parser and request-handler exception text that can quote payload fragments
is omitted; boundary and available error code remain. Other existing operational
exception text is bounded, not a universal secrets classifier: do not include
credentials or sensitive values in client names or operational exception messages.

Transport logging is best effort. Formatting and sink failures cannot escape
through the MCP logger; failures increment an in-process atomic counter, not a
new public diagnostic API. Failure to initialize retained disk logging does not
abort plugin load. Missing logs do not establish no traffic or a healthy reaper.
Initialization failures distinguish `path`, `sink`, `logger` and `publish` stages
with a fixed error class, without raw exception text or private paths. A numeric
code is supplied only by `std::system_error` (its category is not exported, so
the number alone is not a portable OS error identity); other failures use zero.
When no retained sink exists, its failure cannot be written into that file.
The warning uses the surviving default logger subject to ordinary filtering,
and best-effort Windows debugger output independent of that threshold. Neither
is a guarantee of persistent capture. No unbounded fallback file is created.
The synchronous shared sink can impose disk-I/O latency; this is not an async
collector, hard-time-bounded logging, session inventory, or proof of expiry,
worker quiescence, connection correctness or resolution of any current incident.
