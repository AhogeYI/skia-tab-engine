// Devtools panel DOM application. All dynamic text goes through
// textContent (never innerHTML with data), the token lives in the RPC
// client's memory only, and every request the panel makes flows through
// the authenticated routes of the transport that served this page.
import {
  createRpcClient, createEventCursor, createMutationGate,
  imageToClient, redact, diagnosticsSnapshot,
} from "./panel_core.js";

const $ = (id) => document.getElementById(id);
const el = (tag, text, cls) => {
  const node = document.createElement(tag);
  if (text !== undefined) node.textContent = String(text);
  if (cls) node.className = cls;
  return node;
};

// ---- transport adapter -------------------------------------------------
// The panel is usually served by the target itself; typing another loopback
// endpoint retargets the same page (data requests then cross an origin,
// which the transport refuses for non-loopback hosts by design).
let endpoint = location.origin;
function fetchText(path, options) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), (options.timeoutS || 10) * 1000);
  return fetch(endpoint + path, {
    method: options.method,
    headers: options.headers,
    body: options.body || undefined,
    signal: controller.signal,
  }).then(async (response) => {
    const body = await response.text();
    return { status: response.status, body };
  }).finally(() => clearTimeout(timer));
}

const rpc = createRpcClient(fetchText);
const gate = createMutationGate();
const events = createEventCursor();
const state = {
  info: null, schema: null, connected: false,
  windows: [], windowId: null, scale: 1,
  snapshot: null, selectedNode: null,
  shot: null, // {meta, bytes, objectUrl, imgSize}
  polling: null, logs: [],
};

function secrets() {
  return [rpc.hasToken() ? null : "", $("token").value].filter(Boolean);
}
function toast(message, isError) {
  const box = $("toast");
  box.hidden = false;
  box.textContent = redact(String(message), secrets());
  box.className = isError ? "error" : "";
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => { box.hidden = true; }, 4200);
}

// ---- connection ---------------------------------------------------------
function setStatus(text, cls) {
  const node = $("conn-status");
  node.textContent = text;
  node.className = "status " + (cls || "");
}
async function connect() {
  endpoint = $("endpoint").value.trim() || location.origin;
  rpc.setToken($("token").value);
  if (!rpc.hasToken()) { toast("enter the control token first", true); return; }
  const info = await rpc.call("System.getInfo");
  if (!info.ok) {
    state.connected = false;
    setStatus("auth failed", "bad");
    toast("connect failed: " + (info.error && info.error.message), true);
    return;
  }
  const schema = await fetchSchema();
  if (!schema.ok) { toast("schema fetch failed: " + schema.error, true); return; }
  state.info = info.result;
  state.schema = schema.result;
  state.connected = true;
  $("token").value = ""; // typed value already moved into memory
  setStatus("connected", "ok");
  const identity = state.info.identity || state.info;
  $("conn-info").textContent =
    (identity.application || "app") + " " + (identity.appVersion || "") +
    " · engine " + (identity.engineVersion || "?") +
    " · " + (identity.protocolVersion ? "protocol " + identity.protocolVersion : "") +
    " · " + (identity.instanceId || "");
  $("connect").disabled = true;
  $("disconnect").disabled = false;
  await refreshWindows();
  renderMethodList();
}
async function fetchSchema() {
  const response = await fetchText("/automation/v1/schema", {
    method: "GET", headers: { "Authorization": "Bearer " + $("token").value }, body: "",
  });
  if (response.status !== 200) return { ok: false, error: "http " + response.status };
  try { return { ok: true, result: JSON.parse(response.body) }; }
  catch (_) { return { ok: false, error: "invalid schema JSON" }; }
}
function disconnect() {
  stopPolling();
  rpc.clearToken();
  state.connected = false;
  state.info = null; state.schema = null; state.snapshot = null; state.shot = null;
  setStatus("disconnected");
  $("connect").disabled = false;
  $("disconnect").disabled = true;
  $("conn-info").textContent = "";
  $("tree").replaceChildren();
  $("node-details").textContent = "select a node";
  $("method-list").replaceChildren();
  $("shot-empty").hidden = false;
}

// ---- inspect ------------------------------------------------------------
async function refreshWindows() {
  const result = await rpc.call("Window.list");
  if (!result.ok) { toast("Window.list failed: " + result.error.message, true); return; }
  state.windows = result.result.windows || [];
  if (!state.windows.some((w) => String(w.id) === String(state.windowId))) {
    const first = state.windows[0];
    state.windowId = first ? String(first.id) : null;
    state.scale = first && first.scale ? Number(first.scale) : 1;
  }
  await refreshTree();
}
async function refreshTree() {
  if (!state.windowId) { $("tree").replaceChildren(el("div", "no windows", "muted")); return; }
  const params = { windowId: state.windowId };
  const result = await rpc.call("UI.snapshot", params);
  if (!result.ok) {
    $("tree").replaceChildren(el("div", "snapshot failed: " + result.error.message, "muted"));
    return;
  }
  state.snapshot = result.result;
  $("snapshot-meta").textContent =
    "rev " + (result.result.revision ?? "?") +
    " · nodes " + (result.result.nodes ? result.result.nodes.length : 0) +
    (result.result.truncated ? " · truncated" : "");
  renderTree();
}
function nodeTitle(node) {
  return (node.stableKey || node.name || node.role || "node");
}
function renderTree() {
  const host = $("tree");
  host.replaceChildren();
  const filter = $("tree-filter").value.trim().toLowerCase();
  for (const node of state.snapshot.nodes || []) {
    const haystack = [node.stableKey, node.name, node.role, node.details && node.details.id]
      .filter(Boolean).join(" ").toLowerCase();
    if (filter && haystack.indexOf(filter) < 0) continue;
    const row = el("div", null, "node");
    row.append(el("span", nodeTitle(node)));
    row.append(el("span", node.role, "role"));
    if (!node.visible) row.classList.add("node-hidden");
    if (node.enabled === false) row.classList.add("node-disabled");
    row.dataset.ref = String(node.node);
    row.addEventListener("click", () => selectNode(node, row));
    host.append(row);
  }
}
function selectNode(node, row) {
  state.selectedNode = node;
  for (const other of $("tree").querySelectorAll(".selected")) other.classList.remove("selected");
  if (row) row.classList.add("selected");
  const details = $("node-details");
  details.replaceChildren();
  const rows = [
    ["key", node.stableKey], ["role", node.role], ["name", node.name],
    ["ref", node.node], ["bounds", node.bounds && [node.bounds.x, node.bounds.y,
      node.bounds.width, node.bounds.height].join(", ")],
    ["visible", node.visible], ["enabled", node.enabled],
    ["focused", node.focused], ["selected", node.selected],
    ["actions", (node.actions || []).join(", ") || "none"],
  ];
  for (const [key, value] of Object.entries(node.details || {})) rows.push([key, value]);
  for (const [key, value] of rows) {
    if (value === undefined || value === null || value === "") continue;
    const line = el("div");
    line.append(el("span", key, "k"));
    line.append(el("span", typeof value === "object" ? JSON.stringify(value) : value));
    details.append(line);
  }
  const actions = $("node-actions");
  actions.replaceChildren();
  for (const action of ["click", "type"].filter((a) => (node.actions || []).includes(a))) {
    const button = el("button", action);
    button.addEventListener("click", () => performNodeAction(node, action));
    actions.append(button);
  }
  drawOverlay();
}
async function performNodeAction(node, action) {
  const check = gate.check({ mutating: action !== "click" || false });
  if (!check.permitted) { toast(check.reason, true); return; }
  if (action === "click") {
    const params = { windowId: state.windowId, node: String(node.node),
                     snapshotId: state.snapshot && state.snapshot.revision, strict: true };
    const result = await rpc.call("Input.click", params);
    if (result.ok) { toast("clicked node " + nodeTitle(node)); await refreshTree(); }
    else toast("click failed: " + result.error.message, true);
  } else if (action === "type") {
    const text = prompt("text to type");
    if (text === null) return;
    const result = await rpc.call("Input.typeText",
      { windowId: state.windowId, text, node: String(node.node) });
    if (!result.ok) toast("type failed: " + result.error.message, true);
  }
}

// ---- capture & input ----------------------------------------------------
async function capture() {
  const result = await rpc.call("Capture.screenshot",
    { windowId: state.windowId, frame: "fresh" });
  if (!result.ok) { toast("capture failed: " + result.error.message, true); return; }
  const artifact = await rpc.fetchArtifact(result.result.artifactId);
  if (!artifact.ok) { toast(artifact.error, true); return; }
  if (state.shot && state.shot.objectUrl) URL.revokeObjectURL(state.shot.objectUrl);
  const bytes = new Uint8Array(artifact.body.length);
  for (let i = 0; i < artifact.body.length; ++i) bytes[i] = artifact.body.charCodeAt(i) & 0xff;
  state.shot = {
    meta: result.result,
    objectUrl: URL.createObjectURL(new Blob([bytes], { type: "image/png" })),
  };
  const img = $("shot");
  img.src = state.shot.objectUrl;
  img.onload = () => { sizeOverlay(); drawOverlay(); };
  $("shot-empty").hidden = true;
  $("shot-meta").textContent =
    result.result.width + "x" + result.result.height + " · " + result.result.backend +
    " · frame " + result.result.frameSerial + " · scale " + state.scale;
}
function sizeOverlay() {
  const img = $("shot");
  const canvas = $("shot-overlay");
  canvas.width = img.naturalWidth || img.clientWidth;
  canvas.height = img.naturalHeight || img.clientHeight;
}
function drawOverlay() {
  const canvas = $("shot-overlay");
  if (!state.shot || !canvas.width || !state.selectedNode) return;
  const context = canvas.getContext("2d");
  context.clearRect(0, 0, canvas.width, canvas.height);
  const bounds = state.selectedNode.bounds;
  if (!bounds) return;
  // Node bounds are client logical px; the image is client physical px.
  const scale = state.scale || 1;
  context.strokeStyle = "#4da3ff";
  context.lineWidth = Math.max(2, canvas.width / 480);
  context.strokeRect(bounds.x * scale, bounds.y * scale,
                      bounds.width * scale, bounds.height * scale);
}
async function imageClick(event) {
  if (!state.shot) return;
  const rect = $("shot").getBoundingClientRect();
  const px = (event.clientX - rect.left) * ($("shot").naturalWidth / rect.width);
  const py = (event.clientY - rect.top) * ($("shot").naturalHeight / rect.height);
  const point = imageToClient(px, py, { scale: state.scale, bounds: { x: 0, y: 0 } });
  const gateCheck = gate.check({ mutating: true });
  if (!gateCheck.permitted) { toast(gateCheck.reason, true); return; }
  const result = await rpc.call("Input.click",
    { windowId: state.windowId, x: point.x, y: point.y });
  logInput("click -> " + point.x.toFixed(1) + "," + point.y.toFixed(1) +
           ": " + (result.ok ? "ok" : result.error.message));
}
function logInput(text) {
  const box = $("input-log");
  box.append(el("div", text));
  while (box.childNodes.length > 40) box.removeChild(box.firstChild);
  box.scrollTop = box.scrollHeight;
}
async function sendKey(kind) {
  const key = $("input-key").value.trim();
  if (!key) { toast("name a key first", true); return; }
  const gateCheck = gate.check({ mutating: true });
  if (!gateCheck.permitted) { toast(gateCheck.reason, true); return; }
  const result = await rpc.call("Input.key",
    { windowId: state.windowId, key, action: kind });
  logInput("key " + key + " " + kind + ": " + (result.ok ? "ok" : result.error.message));
}
async function sendText() {
  const text = $("input-text").value;
  if (!text) return;
  const gateCheck = gate.check({ mutating: true });
  if (!gateCheck.permitted) { toast(gateCheck.reason, true); return; }
  const result = await rpc.call("Input.typeText", { windowId: state.windowId, text });
  logInput("typeText: " + (result.ok ? "ok" : result.error.message));
}
async function sendWheel() {
  const notches = Number($("input-wheel").value) || 0;
  const gateCheck = gate.check({ mutating: true });
  if (!gateCheck.permitted) { toast(gateCheck.reason, true); return; }
  const result = await rpc.call("Input.wheel",
    { windowId: state.windowId, notches });
  logInput("wheel " + notches + ": " + (result.ok ? "ok" : result.error.message));
}

// ---- methods & tasks ----------------------------------------------------
function renderMethodList() {
  const host = $("method-list");
  host.replaceChildren();
  const filter = $("method-filter").value.trim().toLowerCase();
  const methods = (state.schema && state.schema.methods) || [];
  let shown = 0;
  for (const method of methods) {
    if (filter && method.name.toLowerCase().indexOf(filter) < 0) continue;
    ++shown;
    const row = el("div", null, method.mutating ? "mutating" : "");
    row.append(el("span", method.name));
    row.addEventListener("click", () => selectMethod(method, row));
    host.append(row);
  }
  $("method-count").textContent = shown + " / " + methods.length + " methods";
}
function selectMethod(method, row) {
  for (const other of $("method-list").querySelectorAll(".selected")) {
    other.classList.remove("selected");
  }
  if (row) row.classList.add("selected");
  const form = $("method-form");
  form.replaceChildren();
  form.append(el("div", method.name));
  form.append(el("div", method.summary || "", "muted"));
  form.append(el("div", (method.mutating ? "mutating" : "read-only") +
                           (method.grants ? " · grants: " + method.grants.join(", ") : ""), "muted"));
  const area = document.createElement("textarea");
  area.rows = 6;
  area.spellcheck = false;
  const properties = method.params && method.params.properties;
  area.value = properties && Object.keys(properties).length
    ? JSON.stringify(Object.fromEntries(
        Object.keys(properties).map((key) => [key, sampleValue(properties[key], method)])), null, 2)
    : "{}";
  form.append(area);
  const run = el("button", "invoke");
  run.addEventListener("click", () => invokeMethod(method, area.value));
  form.append(run);
  $("method-result").textContent = "";
  $("method-wait").replaceChildren();
}
function sampleValue(property, method) {
  const required = (method.params && method.params.required) || [];
  const name = property.name || "";
  if (property.type === "number" || property.type === "integer") {
    return required.includes(name) ? 0 : undefined;
  }
  if (property.type === "boolean") return required.includes(name) ? false : undefined;
  if (property.default !== undefined) return property.default;
  return required.includes(name) ? "" : undefined;
}
async function invokeMethod(method, paramsText) {
  let params;
  try { params = JSON.parse(paramsText); }
  catch (error) { toast("params are not valid JSON: " + error.message, true); return; }
  const gateCheck = gate.check(method);
  if (!gateCheck.permitted) { toast(gateCheck.reason, true); return; }
  const result = await rpc.call(method.name, params, 30);
  $("method-result").textContent = result.ok
    ? JSON.stringify(result.result, null, 2)
    : "error " + result.error.code + ": " + result.error.message;
  const operationId = result.ok && result.result && result.result.operationId;
  if (operationId) trackOperation(String(operationId));
}
async function trackOperation(operationId) {
  const waitHost = $("method-wait");
  waitHost.replaceChildren();
  const status = el("span", "operation " + operationId + ": …");
  const cancel = el("button", "cancel");
  cancel.addEventListener("click", async () => {
    const result = await rpc.call("Operation.cancel", { operationId });
    status.textContent = "cancel: " + (result.ok
      ? JSON.stringify(result.result) : result.error.message);
  });
  waitHost.append(status, cancel);
  for (let i = 0; i < 600; ++i) {
    await new Promise((resolve) => setTimeout(resolve, 250));
    const result = await rpc.call("Operation.get", { operationId });
    if (!result.ok) { status.textContent = "operation query failed: " + result.error.message; return; }
    const info = result.result;
    status.textContent = "operation " + operationId + ": " + info.state +
      (info.cancellationRequested ? " (cancel requested)" : "") +
      (info.progress ? " " + info.progress.done + "/" + info.progress.total : "");
    if (["completed", "failed", "cancelled"].includes(info.state)) return;
  }
  status.textContent = "operation tracking stopped (still running)";
}

// ---- events / logs / diagnostics ----------------------------------------
function stopPolling() {
  if (state.polling) { clearTimeout(state.polling.timer); state.polling = null; }
  $("events-toggle").textContent = "Start polling";
}
async function pollOnce() {
  const result = await rpc.call("Event.poll",
    { since: String(events.since), max: 100 }, 15);
  if (!state.polling) return;
  if (!result.ok) {
    $("events-status").textContent = "poll failed: " + result.error.message;
  } else {
    const applied = events.apply(result.result);
    if (applied.gapped) {
      $("events-list").append(el("div",
        "-- events trimmed: cursor reset (missed history) --", "gap"));
    }
    for (const event of applied.events) appendEvent(event);
    $("events-status").textContent = "cursor " + events.since +
      (events.gapCount ? " · gaps " + events.gapCount : "");
  }
  if (state.polling) {
    state.polling.timer = setTimeout(pollOnce, state.polling.interval);
  }
}
function appendEvent(event) {
  const box = $("events-list");
  box.append(el("div", event.sequence + " " + event.name + " " +
    JSON.stringify(event.details || {})));
  while (box.childNodes.length > 400) box.removeChild(box.firstChild);
  box.scrollTop = box.scrollHeight;
}
async function refreshLogs() {
  const result = await rpc.call("Log.read", { afterSequence: 0, max: 200 });
  if (!result.ok) { toast("log read failed: " + result.error.message, true); return; }
  state.logs = [];
  const box = $("logs-list");
  box.replaceChildren();
  const sources = (result.result && result.result.sources) || {};
  for (const records of Object.values(sources)) {
    for (const record of records.records || []) state.logs.push(record);
  }
  state.logs.sort((a, b) => a.sequence - b.sequence);
  for (const record of state.logs) {
    box.append(el("div", record.sequence + " [" + record.level + "] " + record.message));
  }
}
function downloadDiagnostics() {
  const text = diagnosticsSnapshot({
    info: state.info,
    windows: state.windows,
    snapshotMeta: state.snapshot && {
      revision: state.snapshot.revision, layoutRevision: state.snapshot.layoutRevision,
      documentGeneration: state.snapshot.documentGeneration,
    },
    eventsCursor: { since: events.since, gaps: events.gapCount },
    logs: state.logs,
  }, secrets());
  const url = URL.createObjectURL(new Blob([text], { type: "application/json" }));
  const link = document.createElement("a");
  link.href = url;
  link.download = "tabengine-diagnostics.json";
  link.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

// ---- wiring --------------------------------------------------------------
for (const button of document.querySelectorAll("#tabs button")) {
  button.addEventListener("click", () => {
    for (const other of document.querySelectorAll("#tabs button")) {
      other.classList.toggle("active", other === button);
    }
    for (const section of document.querySelectorAll(".tab")) {
      section.classList.toggle("active", section.id === "tab-" + button.dataset.tab);
    }
  });
}
$("connect").addEventListener("click", connect);
$("disconnect").addEventListener("click", disconnect);
$("endpoint").value = location.origin;
$("refresh-tree").addEventListener("click", refreshTree);
$("tree-filter").addEventListener("input", () => state.snapshot && renderTree());
$("capture-shot").addEventListener("click", capture);
$("input-refresh").addEventListener("click", capture);
$("shot").addEventListener("click", imageClick);
$("allow-write").addEventListener("change", (event) => {
  gate.setAllowed(event.target.checked);
  toast(event.target.checked ? "write actions enabled for this session" : "write actions off");
});
$("input-key-down").addEventListener("click", () => sendKey("down"));
$("input-key-up").addEventListener("click", () => sendKey("up"));
$("input-type").addEventListener("click", sendText);
$("input-wheel-send").addEventListener("click", sendWheel);
$("method-filter").addEventListener("input", renderMethodList);
$("events-toggle").addEventListener("click", () => {
  if (state.polling) { stopPolling(); return; }
  if (!state.connected) { toast("connect first", true); return; }
  state.polling = { timer: 0, interval: 700 };
  $("events-toggle").textContent = "Stop polling";
  pollOnce();
});
$("events-clear").addEventListener("click", () => $("events-list").replaceChildren());
$("logs-refresh").addEventListener("click", refreshLogs);
$("diag-download").addEventListener("click", downloadDiagnostics);
window.addEventListener("beforeunload", stopPolling);
