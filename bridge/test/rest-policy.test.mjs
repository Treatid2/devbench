import assert from "node:assert/strict";
import { createServer } from "node:http";
import { once } from "node:events";
import { test } from "node:test";
import { parseBridgeArgs, validateRestTimeout, fetchToolJson, RestCallError,
  DEFAULT_REST_TIMEOUT_MS, MAX_REST_TIMEOUT_MS } from "../src/rest-policy.ts";

async function fixture(handler, run) {
  const server = createServer(handler);
  server.listen(0, "127.0.0.1");
  await once(server, "listening");
  try { await run(`http://127.0.0.1:${server.address().port}`); }
  finally { server.closeAllConnections(); await new Promise((resolve) => server.close(resolve)); }
}

test("rest policy: defaults and explicit long-call envelopes remain bounded", () => {
  assert.equal(DEFAULT_REST_TIMEOUT_MS, 30000);
  assert.deepEqual(parseBridgeArgs(["--game", "vr"]), { setup: false, game: "vr" });
  for (const [native, bridge, client] of [[120000,124000,125000], [60000,64000,65000], [30000,34000,35000]]) {
    const parsed = parseBridgeArgs(["--game", "vr", "--rest-timeout-ms", String(bridge)]);
    assert.equal(parsed.restTimeoutMs, bridge);
    assert.ok(native < bridge && bridge < client && client < 600000);
  }
  assert.equal(validateRestTimeout(MAX_REST_TIMEOUT_MS), 540000);
});

test("rest policy: invalid and duplicate CLI deadlines fail before any call", () => {
  for (const value of ["0", "-1", "540001", "NaN", "1.2", "1e5", "Infinity", "9007199254740993", ""]) {
    assert.throws(() => parseBridgeArgs(["--rest-timeout-ms", value]));
  }
  assert.throws(() => parseBridgeArgs(["--rest-timeout-ms"]));
  assert.throws(() => parseBridgeArgs(["--rest-timeout-ms", "--game", "vr"]));
  assert.throws(() => parseBridgeArgs(["--rest-timeout-ms", "30000", "--rest-timeout-ms", "124000"]));
  assert.throws(() => parseBridgeArgs(["--rest-timeot-ms", "124000"]));
});

test("rest policy: one unchanged POST and successful native JSON pass through", async () => {
  let calls = 0;
  const args = { operation: "qualification_wait", timeoutMs: 120000, nested: [null, false, "x"] };
  const response = { ok: false, reason: "native-declared", value: { typed: 1 } };
  await fixture(async (req, res) => {
    calls++;
    assert.equal(req.method, "POST");
    assert.equal(req.url, "/api/tool/qualification_wait");
    let body = "";
    for await (const chunk of req) body += chunk;
    assert.deepEqual(JSON.parse(body), args);
    res.end(JSON.stringify(response));
  }, async (url) => {
    assert.deepEqual(await fetchToolJson(`${url}/api/tool/qualification_wait`, {
      method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(args),
    }, 124000), response);
  });
  assert.equal(calls, 1);
});

test("rest policy: HTTP error preserves status and original JSON without retry", async () => {
  let calls = 0;
  const body = { error: "owned refusal", operationId: "fixture-only", retryAfter: 12 };
  await fixture((_req, res) => { calls++; res.writeHead(503); res.end(JSON.stringify(body)); }, async (url) => {
    await assert.rejects(fetchToolJson(url, { method: "POST" }, 1000), (error) => {
      assert.ok(error instanceof RestCallError);
      assert.equal(error.code, "REST_HTTP_ERROR");
      assert.equal(error.httpStatus, 503);
      assert.deepEqual(error.responseBody, body);
      assert.equal(error.outcome, "unknown");
      assert.equal(error.retrySafe, false);
      return true;
    });
  });
  assert.equal(calls, 1);
});

test("rest policy: deadline before headers is uncertain and never replayed", async () => {
  let calls = 0;
  await fixture((_req, _res) => { calls++; }, async (url) => {
    await assert.rejects(fetchToolJson(url, { method: "POST" }, 100), (error) => {
      assert.equal(error.code, "REST_TIMEOUT");
      assert.equal(error.timeoutMs, 100);
      assert.equal(error.retrySafe, false);
      assert.match(error.message, /native outcome unknown/);
      assert.doesNotMatch(error.message, /game not running/);
      return true;
    });
  });
  assert.equal(calls, 1);
});

test("rest policy: deadline covers a stalled response body, not just headers", async () => {
  let calls = 0;
  await fixture((_req, res) => { calls++; res.writeHead(200); res.flushHeaders(); res.write('{"partial":'); }, async (url) => {
    await assert.rejects(fetchToolJson(url, { method: "POST" }, 150), (error) => {
      assert.equal(error.code, "REST_TIMEOUT");
      assert.equal(error.outcome, "unknown");
      return true;
    });
  });
  assert.equal(calls, 1);
});

test("rest policy: malformed response retains raw text and uncertain semantics", async () => {
  await fixture((_req, res) => { res.end("not JSON"); }, async (url) => {
    await assert.rejects(fetchToolJson(url, { method: "POST" }, 1000), (error) => {
      assert.equal(error.code, "REST_INVALID_JSON");
      assert.equal(error.responseBody, "not JSON");
      assert.equal(error.httpStatus, 200);
      assert.equal(error.retrySafe, false);
      return true;
    });
  });
});

test("rest policy: explicit bridge duration controls owned delayed reply", async () => {
  let calls = 0;
  await fixture((_req, res) => {
    calls++;
    const timer = setTimeout(() => res.end('{"completed":true}'), 200);
    res.on("close", () => clearTimeout(timer));
  }, async (url) => {
    assert.deepEqual(await fetchToolJson(url, { method: "POST" }, 1500), { completed: true });
  });
  assert.equal(calls, 1);
});

test("rest policy: disconnected response is uncertain, not a missing game claim", async () => {
  let calls = 0;
  await fixture((req) => { calls++; req.socket.destroy(); }, async (url) => {
    await assert.rejects(fetchToolJson(url, { method: "POST" }, 1000), (error) => {
      assert.equal(error.code, "REST_TRANSPORT_ERROR");
      assert.equal(error.retrySafe, false);
      assert.doesNotMatch(error.message, /game not running/);
      return true;
    });
  });
  assert.equal(calls, 1);
});
