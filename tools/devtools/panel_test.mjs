// Headless logic regression for the devtools panel core (no browser, no
// dependencies). Run: node tools/devtools/panel_test.mjs
// Covers: token policy (memory only, never in a URL), JSON-RPC client
// error surfaces, event cursor gap semantics, DPI coordinate mapping,
// the write gate, and diagnostics redaction.
import {
  createRpcClient, createEventCursor, imageToClient, createMutationGate,
  redact, diagnosticsSnapshot, RPC_PATH, ARTIFACT_PATH,
  nodeClickParams, keyParams, wheelParams,
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
  const fetchBytes = async (path, options) => {
    requests.push({ path, options, binary: true });
    const key = "BYTES " + options.method + " " + path;
    const responder = responses[key] || responses.default;
    const out = typeof responder === "function" ? responder(options) : responder;
    // The binary channel must yield ArrayBuffer bytes verbatim.
    return { status: out.status, bytes: out.bytes instanceof ArrayBuffer
      ? out.bytes : new TextEncoder().encode(out.body || "").buffer };
  };
  return { requests, fetchText, fetchBytes };
}

// ---- token policy --------------------------------------------------------
{
  const pngMagic = new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 1, 2, 3, 255]);
  const pngBuffer = pngMagic.buffer.slice(pngMagic.byteOffset,
                                          pngMagic.byteOffset + pngMagic.byteLength);
  const transport = makeTransport({
    default: { status: 200, body: '{"jsonrpc":"2.0","id":"1","result":{}}' },
    "BYTES GET /automation/v1/artifacts/a1": { status: 200, bytes: pngBuffer },
  });
  const client = createRpcClient(transport);
  check(!client.hasToken(), "starts without a token");
  const missing = await client.call("System.getInfo");
  check(!missing.ok && missing.error.code === 0, "refuses calls without a token");
  client.setToken("secret-token-1234567890");
  await client.call("System.getInfo");
  const artifact = await client.fetchArtifact("a1");
  check(artifact.ok, "artifact fetch succeeds over the binary channel");
  const round = new Uint8Array(artifact.bytes);
  check(round.length === pngMagic.length &&
        pngMagic.every((byte, index) => round[index] === byte),
        "artifact bytes survive byte-for-byte (no text decoding)");
  for (const request of transport.requests) {
    check(request.path.indexOf("secret-token") < 0,
          "token absent from URL " + request.path);
    const header = request.options.headers.Authorization || "";
    check(header === "Bearer secret-token-1234567890",
          "token travels only in the auth header");
  }
  check(transport.requests[0].path === RPC_PATH, "rpc hits the engine route");
  check(transport.requests[1].path === ARTIFACT_PATH + "a1", "artifact path is id-only");
  check(transport.requests[1].binary, "artifacts use the binary channel");
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
  ], next: 6 });
  check(applied.events.length === 2 && !applied.gapped, "first batch accepted");
  check(cursor.since === 6, "cursor is the last sequence seen, not the next one");
  applied = cursor.apply({ events: [{ sequence: 7, name: "input.dispatched" }], next: 7 });
  check(applied.events.length === 1, "next batch accepted");
  applied = cursor.apply({ gap: true, next: 40, oldestAvailable: 40,
    events: [{ sequence: 40, name: "retained" }] });
  check(applied.gapped && applied.events.length === 1 && !applied.dropped,
        "gap reports discontinuity without dropping retained events");
  check(cursor.since === 40 && cursor.gapCount === 1, "cursor matches server next on gap");
  applied = cursor.apply({ events: [{ sequence: 41, name: "x" }], next: 41 });
  check(applied.events.length === 1 && !applied.gapped, "stream resumes after the gap");
  applied = cursor.apply({ events: [{ sequence: 41, name: "x" }], next: 41 });
  check(applied.events.length === 0 && cursor.since === 41, "duplicates do not replay");
  applied = cursor.apply({ gap: true, events: [], next: 41, oldestAvailable: 60 });
  check(applied.gapped && cursor.since === 59, "fully trimmed ring advances beyond lost history");
}

// ---- coordinate mapping (DPI) ---------------------------------------------
{
  const point = imageToClient(300, 150, { scale: 1.5, bounds: { x: 10, y: 20 } });
  check(point.x === 310 && point.y === 170,
        "client pixels retain physical DPI scale and add crop origin");
  const identity = imageToClient(50, 60, { scale: 1, bounds: { x: 0, y: 0 } });
  check(identity.x === 50 && identity.y === 60, "scale 1 is the identity");
  let threw = false;
  try { imageToClient(NaN, 1, {}); } catch (_) { threw = true; }
  check(threw, "invalid coordinates throw instead of mapping silently");
  check(imageToClient(2.6, 3.4, {}).x === 3, "input coordinates use integer client pixels");
}

// Actual bridge schemas, not test-only payload shapes.
{
  const click = nodeClickParams("1", { node: "4" }, { revision: 99, layoutRevision: 7 });
  check(click.snapshotId === 7 && click.strict, "strict clicks use layoutRevision");
  const key = keyParams("1", "Enter", "down");
  check(key.kind === "down" && !("action" in key), "key uses required kind field");
  const wheel = wheelParams("1", -2, { x: 30.2, y: 50.8 });
  check(wheel.deltaY === -240 && wheel.x === 30 && wheel.y === 51,
        "wheel sends a client position and native delta");
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

{
  const gate = createMutationGate();
  const transport = makeTransport({ default: { status: 200,
    body: '{"result":{}}' } });
  const client = createRpcClient(transport.fetchText, {
    authorize: () => gate.check({ mutating: true }),
  });
  client.setToken("token");
  for (const method of ["Input.click", "Input.key", "Operation.cancel"]) {
    check(!(await client.call(method)).ok, method + " refused at the dispatch boundary");
  }
  check(transport.requests.length === 0, "blocked writes never reach HTTP");
  gate.setAllowed(true);
  check((await client.call("Input.click")).ok, "opt-in permits dispatch");
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
  check(snapshot.indexOf("never-appears") < 0 && snapshot.indexOf("nor-this") < 0 &&
        snapshot.indexOf("nope") < 0,
        "credential-looking fields are dropped from diagnostics");
  check(snapshot.indexOf("0.3.0") >= 0, "ordinary data survives");
}

if (failures > 0) {
  console.error(failures + " panel core check(s) failed");
  process.exit(1);
}
console.log("panel core: all checks passed");
