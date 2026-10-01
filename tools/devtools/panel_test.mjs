// Headless logic regression for the devtools panel core (no browser, no
// dependencies). Run: node tools/devtools/panel_test.mjs
// Covers: token policy (memory only, never in a URL), JSON-RPC client
// error surfaces, logical input sessions (begin, header attach, expiry
// replay, explicit end), connection epochs (stale results discarded,
// in-flight aborted), key-name normalization against the real bridge
// table, per-source log cursors, event cursor gap semantics, DPI
// coordinate mapping, the write gate at the dispatch boundary, and
// diagnostics redaction.
import {
  createRpcClient, createEventCursor, createLogCursor, imageToClient,
  createMutationGate, redact, diagnosticsSnapshot, normalizeKeyName,
  RPC_PATH, ARTIFACT_PATH, SESSION_PATH, SESSION_HEADER,
  nodeClickParams, wheelParams,
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
    if (out && out.never) {
      // Deferred response for in-flight/epoch scenarios; a real fetch
      // would reject when the caller's signal aborts, so mirror that.
      return new Promise((resolve, reject) => {
        out.resolve = resolve;
        const signal = options.signal;
        if (signal) {
          if (signal.aborted) reject(new Error("The operation was aborted"));
          else signal.addEventListener("abort",
            () => reject(new Error("The operation was aborted")), { once: true });
        }
      });
    }
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
  return { requests, fetchText, fetchBytes, responses };
}

const OK = (result) => ({ status: 200, body: JSON.stringify({ jsonrpc: "2.0", id: "1", result }) });

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

// ---- logical input session ------------------------------------------------
{
  const transport = makeTransport({
    "POST /automation/v1/session": { status: 200,
      body: '{"sessionId":"sess-123","idleTimeoutS":30}' },
    "DELETE /automation/v1/session": { status: 200, body: '{"ended":true}' },
    default: { status: 200, body: '{"jsonrpc":"2.0","id":"1","result":{}}' },
  });
  const client = createRpcClient(transport);
  client.setToken("token");
  check(client.sessionId === "", "no session before begin");
  const begun = await client.beginSession();
  check(begun.ok && client.sessionId === "sess-123", "session begin stores the id");
  check(transport.requests[0].path === SESSION_PATH &&
        transport.requests[0].options.method === "POST",
        "session begin hits the session route");
  check(transport.requests[0].options.headers.Authorization === "Bearer token",
        "session begin authenticates");
  await client.call("Input.key");
  const rpcRequest = transport.requests[1];
  check(rpcRequest.options.headers[SESSION_HEADER] === "sess-123",
        "rpc calls attach the session header");
  const ended = await client.endSession();
  check(ended.ok, "explicit session end succeeds");
  const deleteRequest = transport.requests.find((r) =>
    r.path === SESSION_PATH && r.options.method === "DELETE");
  check(deleteRequest.options.headers[SESSION_HEADER] === "sess-123",
        "session end carries the session header");
  check(client.sessionId === "", "session id cleared after end");
  await client.call("Input.key");
  const afterEnd = transport.requests[transport.requests.length - 1];
  check(afterEnd.options.headers[SESSION_HEADER] === undefined,
        "calls after end run without a session header");
}

// Session expiry: the transport validates sessions BEFORE dispatching, so
// the panel replays the exact request once after re-beginning (safe for
// mutations too - the request never ran).
{
  let sessionCount = 0;
  const transport = makeTransport({
    "POST /automation/v1/session": () => {
      sessionCount += 1;
      return { status: 200, body: '{"sessionId":"sess-' + sessionCount + '"}' };
    },
    default: (options) => {
      const header = options.headers[SESSION_HEADER] || "";
      if (header === "sess-1") {
        return { status: 404, body: '{"error":"session not found or expired"}' };
      }
      return { status: 200,
               body: '{"jsonrpc":"2.0","id":"1","result":{"replayed":true}}' };
    },
  });
  const client = createRpcClient(transport);
  client.setToken("token");
  await client.beginSession();
  const result = await client.call("Input.click", { windowId: "1" });
  check(result.ok && result.result.replayed, "expired session replays after renewal");
  check(sessionCount === 2, "exactly one renewal");
  const rpcs = transport.requests.filter((r) => r.path === RPC_PATH);
  check(rpcs.length === 2, "the same request ran twice (first hit a dead session)");
  check(client.sessionId === "sess-2", "client carries the renewed session");
}

// Concurrent session expiry: two 404-renewals racing share ONE begin
// (single-flight); no second session is created and the id is not
// overwritten by an older response.
{
  let sessionCount = 0;
  const transport = makeTransport({
    "POST /automation/v1/session": () => {
      sessionCount += 1;
      return { status: 200, body: '{"sessionId":"sess-' + sessionCount + '"}' };
    },
    default: (options) => {
      // Only the pre-renewal session id is dead; requests carrying the
      // renewed session dispatch normally.
      const header = options.headers[SESSION_HEADER] || "";
      if (header === "sess-1" || header === "") {
        return { status: 404, body: '{"error":"session not found or expired"}' };
      }
      return { status: 200,
               body: '{"jsonrpc":"2.0","id":"1","result":{"replayed":true}}' };
    },
  });
  const client = createRpcClient(transport);
  client.setToken("token");
  await client.beginSession(); // sess-1
  const first = client.call("Input.click", { windowId: "1" });
  const second = client.call("Input.key", {});
  const [a, b] = await Promise.all([first, second]);
  check(a.ok && b.ok, "both racing calls recover through the shared renewal");
  check(sessionCount === 2, "concurrent renewals create exactly one new session");
  check(client.sessionId === "sess-2", "no older session overwrites the renewed id");
}

// ---- target epoch: late results never land on the new target --------------
{
  const transport = makeTransport({
    default: { status: 200, never: true },
  });
  const client = createRpcClient(transport);
  client.setToken("token");
  check(client.targetEpoch === 0, "target generation starts at zero");
  const slowTree = client.call("UI.snapshot", { windowId: "1" });
  await new Promise((resolve) => setTimeout(resolve, 20));
  // The operator switches window: a NEW target generation, same connection.
  const generation = client.newTargetEpoch();
  check(client.targetEpoch === 1 && generation === 1 && client.epoch === 0,
        "target switch advances only the target generation");
  const settled = await slowTree;
  check(!settled.ok && settled.stale,
        "a tree result from the previous target resolves stale");
  // A result that completes after the switch carries its own generation so
  // multi-step flows can compare instead of guessing.
  transport.responses.default = { status: 200,
    body: '{"jsonrpc":"2.0","id":"1","result":{}}' };
  const late = await client.call("Capture.screenshot", { windowId: "1" });
  check(late.ok && late.targetEpoch === 1,
        "results carry the target generation they belong to");
  check(late.targetEpoch !== 0,
        "a generation-0 screenshot is detectably from the old target");
}

// ---- session renewal is single-flight -------------------------------------

// ---- connection epoch -----------------------------------------------------
{
  const transport = makeTransport({
    default: { status: 200, never: true },
  });
  const client = createRpcClient(transport);
  client.setToken("token");
  const pending = client.call("System.getInfo");
  await new Promise((resolve) => setTimeout(resolve, 20));
  const myAbort = transport.requests[0].options.signal;
  check(typeof myAbort !== "string", "core hands a native AbortSignal to the transport");
  client.newEpoch();
  check(client.sessionId === "" && client.epoch === 1, "epoch advance clears the session");
  check(myAbort.aborted, "epoch advance aborts the in-flight request");
  const settled = await pending;
  check(!settled.ok && settled.stale, "in-flight result resolves stale, never rendered");
  // A completed result from the old epoch is discarded the same way.
  const stale = { status: 200, never: false,
                  body: '{"jsonrpc":"2.0","id":"1","result":{"old":true}}' };
  transport.responses.default = stale;
  const oldCall = client.call("System.getInfo");
  // Simulate a reconnect that starts between dispatch and response.
  client.newEpoch();
  const outcome = await oldCall;
  check(!outcome.ok && outcome.stale, "late old-epoch response is discarded");
}

// Session and schema responses must obey the same epoch fence as RPC.
// Ignore cancellation in this transport to model a response already queued
// when the operator disconnects.
{
  let deliver;
  const client = createRpcClient({ fetchText: () => new Promise((resolve) => {
    deliver = resolve;
  }) });
  client.setToken("token");
  const pending = client.beginSession();
  client.newEpoch();
  deliver({ status: 200, body: '{"sessionId":"old-session"}' });
  const result = await pending;
  check(result.stale && !result.ok && client.sessionId === "",
        "late session creation cannot restore a disconnected session");
  const schema = client.fetchSchema();
  client.newEpoch();
  deliver({ status: 200, body: '{"methods":[{"name":"old"}]}' });
  const oldSchema = await schema;
  check(oldSchema.stale && !oldSchema.ok, "old schema cannot replace a new catalogue");
}
{
  const client = createRpcClient({ fetchText: async () => { throw new Error("offline"); } });
  client.setToken("token");
  check(!(await client.beginSession()).ok, "session network errors are structured failures");
  check(!(await client.fetchSchema()).ok, "schema network errors are structured failures");
}
{
  let textSignal, binarySignal, deliver;
  const client = createRpcClient({
    fetchText: async (path, options) => {
      textSignal = options.signal;
      return { status: 200, body: '{"sessionId":"session"}' };
    },
    fetchBytes: (path, options) => {
      binarySignal = options.signal;
      return new Promise((resolve) => { deliver = resolve; });
    },
  });
  client.setToken("token");
  await client.beginSession();
  const ending = client.endSession();
  client.newEpoch();
  await ending;
  check(!textSignal.aborted, "disconnect does not cancel its session release request");
  const artifact = client.fetchArtifact("image");
  client.newEpoch();
  check(binarySignal && binarySignal.aborted, "epoch advance aborts binary downloads too");
  deliver({ status: 200, bytes: new ArrayBuffer(0) });
  check((await artifact).stale, "late binary response belongs to the old epoch");
}

// ---- key-name normalization (real bridge table contract) ------------------
{
  const enter = normalizeKeyName("Enter");
  check(enter.ok && enter.key === "enter", "Enter normalizes to the table name");
  const f4 = normalizeKeyName("F4");
  check(f4.ok && f4.key === "f4", "F4 normalizes to lowercase");
  const chord = normalizeKeyName("Ctrl+Shift+S");
  check(chord.ok && chord.key === "s" && chord.ctrl && chord.shift && !chord.alt,
        "chords split into the key plus modifier flags");
  const left = normalizeKeyName("ArrowLeft");
  check(left.ok && left.key === "arrowleft", "ArrowLeft maps to the logical name");
  const modifier = normalizeKeyName("ControlLeft");
  check(!modifier.ok && /modifier flag/.test(modifier.reason),
        "standalone modifier keys report unsupported instead of faking success");
  const unknown = normalizeKeyName("F13");
  check(!unknown.ok, "unknown keys refuse");
  const digits = normalizeKeyName("5");
  check(digits.ok && digits.key === "5", "single digits pass through");
}

// ---- per-source log cursor ------------------------------------------------
{
  const cursor = createLogCursor();
  let params = cursor.nextParams(200);
  check(params.after === 0 && params.appAfter === 0, "first read starts at zero");
  const applied = cursor.apply({ sources: {
    engine: { records: [
      { sequence: 1, level: "info", message: "a" },
      { sequence: 3, level: "error", message: "b" },
    ], next: 3 },
    app: { records: [{ sequence: 2, level: "debug", message: "c" }], next: 2 },
  } });
  check(applied.records.length === 3, "all records accepted on first read");
  check(cursor.positions.engine === 3 && cursor.positions.app === 2,
        "cursors advance per source to the server next");
  params = cursor.nextParams(200);
  check(params.after === 3 && params.appAfter === 2,
        "next read resumes per source instead of after=0");
  const duplicate = cursor.apply({ sources: {
    engine: { records: [{ sequence: 3, level: "info", message: "dup" }], next: 3 },
  } });
  check(duplicate.records.length === 0, "replayed records do not duplicate");
  // Level filter: warnings and errors pass, debug is hidden, but the cursor
  // still advances past hidden records.
  cursor.setMinLevel("warning");
  const filtered = cursor.apply({ sources: {
    engine: { records: [
      { sequence: 4, level: "debug", message: "hidden" },
      { sequence: 5, level: "warning", message: "shown" },
    ], next: 5 },
  } });
  check(filtered.records.length === 1 && filtered.records[0].record.message === "shown",
        "level filter hides below-minimum records");
  check(cursor.positions.engine === 5, "hidden records still advance the cursor");
  cursor.reset();
  check(cursor.nextParams(1).after === 0, "reset restarts history for level changes");
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
  cursor.reset();
  check(cursor.since === 0 && cursor.gapCount === 0, "reset restarts the cursor on reconnect");
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
