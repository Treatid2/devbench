# Catalogue schema comparison fixture

The exact 3d requester transport run failed the public schema equality check in
the inverted replacement-callback test. Its registry description, public name
and description, and final handler/value checks did not report failures. The
original log does not contain the differing schema JSON; retain the failed run
and do not claim a measured discrepancy or production race cause.

The pinned MCP type is nlohmann::ordered_json; ToolRegistry uses nlohmann::json.
The expected fixture schema inserts type before properties. Registry storage
sorts object members, then the adapter copies it back into the ordered MCP type.
The old ordered comparison therefore tests insertion order as well as content.
This is a source-supported fixture explanation, not a replay of the old run.

Compare both schemas as ordinary json objects. All fields, values, nested types
and array positions remain significant; only object-member order is ignored.
A new local fixture case covers the differently ordered roundtrip, rejection
of the older string type, a missing field and reversed positional array schema.
The public assertion now includes actual/expected fixture schemas on failure.
This is bounded test output, not permanent production diagnostic instrumentation.

No registry, adapter, dependency type, transport serialization or production
binding policy changes. The callback inversion and final dispatch assertions
remain intact. This direct descendant preserves the separate occupied-port
fixture correction; its compile request and original receipts are not rewritten.
New exact compilation/assertions and focused review are still required.

Reference: [nlohmann object-order documentation](https://json.nlohmann.me/features/object_order/).
