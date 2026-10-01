// Headless logic regression for the devtools panel core (no browser, no
// dependencies). Run: node tools/devtools/panel_test.mjs
// Covers: token policy (memory only, never in a URL), JSON-RPC client
// error surfaces, event cursor gap semantics, DPI coordinate mapping,
// the write gate, and diagnostics redaction.
import {
  createRpcClient, createEventCursor, imageToClient, createMutationGate,
  redact, diagnosticsSnapshot, RPC_PATH, ARTIFACT_PATH,
} from "./panel_core.js";

let failures = 0;
function check(condition, label) {
  if (!condition) {
    ++failures;
    console.error("FAIL: " + label);
  } else {
    console.log("ok: " + label);
  }
}

// ---- fake transport recording every request -----------------------------
function makeTransport(responses) {
  const requests = [];
  const fetchText = async (path, options) => {
    requests.push({ path, options });
    const key = options.method + " " + path;
    const responder = responses[key] || responses.default ||
      (() => ({ status: 404, body: "" }));
    const out = typeof responder === "function" ? responder(options) : responder;
    return out;
  };
  return { requests, fetchText };
}

// ---- token policy --------------------------------------------------------
{
  const transport = makeTransport({
    default: { status: 200, body: '{"jsonrpc":"2.0","id":"1","result":{}}' },
  });
  const client = createRpcClient(transport.fetchText);
  check(!client.hasToken(), "starts without a token");
  const missing = await client.call("System.getInfo");
  check(!missing.ok && missing.error.code === 0, "refuses calls without a token");
  client.setToken("secret-token-1234567890");
  await client.call("System.getInfo");
  await client.fetchArtifact("a1");
  for (const request of transport.requests) {
    check(request.path.indexOf("secret-token") < 0,
          "token absent from URL " + request.path);
    const header = request.options.headers.Authorization || "";
    check(header === "Bearer secret-token-1234567890",
          "token travels only in the auth header");
  }
  check(transport.requests[0].path === RPC_PATH, "rpc hits the engine route");
  check(transport.requests[1].path === ARTIFACT_PATH + "a1", "artifact path is id-only");
}

// ---- rpc error surfaces --------------------------------------------------
{
  const transport = makeTransport({
    default: { status: 401, body: '{"error":"unauthorized"}' },
  });
  const client = createRpcClient(transport.fetchText);
  client.setToken("wrong-token-value-xyz");
  const result = await client.call("System.getInfo");
  check(!result.ok && result.unauthorized, "401 surfaces as unauthorized");
}
{
  const transport = makeTransport({
    default: { status: 200, body: '{"jsonrpc":"2.0","id":"1","error":{"code":1003,"message":"stale node"}}' },
  });
  const client = createRpcClient(transport.fetchText);
  client.setToken("token");
  const result = await client.call("UI.resolveNode", {});
  check(!result.ok && result.error.code === 1003 && result.error.message === "stale node",
        "business errors keep code and message");
}
{
  const transport = makeTransport({
    default: { status: 200, body: "not json" },
  });
  const client = createRpcClient(transport.fetchText);
  client.setToken("token");
  const result = await client.call("System.getInfo");
  check(!result.ok && result.error.code === -32700, "garbage body surfaces as parse error");
}

// ---- event cursor ---------------------------------------------------------
{
  const cursor = createEventCursor();
  let applied = cursor.apply({ events: [
    { sequence: 5, name: "tab.created" }, { sequence: 6, name: "tab.closed" },
  ], nextCursor: 7 });
  check(applied.events.length === 2 && !applied.gapped, "first batch accepted");
  check(cursor.since === 7, "cursor advanced past the last sequence");
  applied = cursor.apply({ events: [{ sequence: 7, name: "input.dispatched" }], nextCursor: 8 });
  check(applied.events.length === 1, "next batch accepted");
  applied = cursor.apply({ gap: true, nextCursor: 40, events: [] });
  check(applied.gapped && applied.events.length === 0 && applied.dropped,
        "gap flags discontinuity and drops the batch");
  check(cursor.since === 40 && cursor.gapCount === 1, "cursor reset to server cursor on gap");
  applied = cursor.apply({ events: [{ sequence: 41, name: "x" }], nextCursor: 42 });
  check(applied.events.length === 1 && !applied.gapped, "stream resumes after the gap");
}

// ---- coordinate mapping (DPI) ---------------------------------------------
{
  const point = imageToClient(300, 150, { scale: 1.5, bounds: { x: 10, y: 20 } });
  check(point.x === 10 + 200 && point.y === 20 + 100,
        "image px maps back through scale and origin");
  const identity = imageToClient(50, 60, { scale: 1, bounds: { x: 0, y: 0 } });
  check(identity.x === 50 && identity.y === 60, "scale 1 is the identity");
  let threw = false;
  try { imageToClient(1, 1, { scale: 0 }); } catch (_) { threw = true; }
  check(threw, "invalid scale throws instead of mapping silently");
}

// ---- write gate -----------------------------------------------------------
{
  const gate = createMutationGate();
  check(!gate.allowed, "write actions default off");
  check(gate.check({ mutating: false }).permitted, "reads always permitted");
  const blocked = gate.check({ mutating: true });
  check(!blocked.permitted && /write actions are off/.test(blocked.reason),
        "mutations blocked with a reason");
  gate.setAllowed(true);
  check(gate.check({ mutating: true }).permitted, "opt-in unblocks mutations");
}

// ---- redaction / diagnostics ----------------------------------------------
{
  const text = redact("bearer secret-token-1234567890 at /rpc", ["secret-token-1234567890"]);
  check(text.indexOf("secret-token") < 0 && text.indexOf("«redacted»") >= 0,
        "secrets are stripped from displayed text");
  const snapshot = diagnosticsSnapshot({
    token: "never-appears", accessToken: "nor-this",
    info: { engineVersion: "0.3.0", nested: { token: "nope" } },
  }, ["secret-token-1234567890"]);
  check(snapshot.indexOf("never-appears") < 0 && snapshot.indexOf("nor-this") < 0,
        "credential-looking fields are dropped from diagnostics");
  check(snapshot.indexOf("0.3.0") >= 0, "ordinary data survives");
}

if (failures > 0) {
  console.error(failures + " panel core check(s) failed");
  process.exit(1);
}
console.log("panel core: all checks passed");
