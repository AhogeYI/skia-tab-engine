// Devtools panel DOM application. All dynamic text goes through
// textContent (never innerHTML with data), the token lives in the RPC
// client's memory only, and every request the panel makes flows through
// the authenticated routes of the transport that served this page.
// Raw input gestures (pointer down/move/up, key down/up, IME) ride one
// logical session created at connect and released at disconnect; a
// connection epoch discards every in-flight result from a previous
// session so a reconnect never mixes instances.
import {
  createRpcClient, createEventCursor, createLogCursor, createMutationGate,
  imageToClient, redact, diagnosticsSnapshot, normalizeKeyName,
  nodeClickParams, wheelParams, SESSION_HEADER,
} from "{{MOUNT}}/assets/panel_core.js";

const $ = (id) => document.getElementById(id);
const el = (tag, text, cls) => {
  const node = document.createElement(tag);
  if (text !== undefined && text !== null) node.textContent = String(text);
  if (cls) node.className = cls;
  return node;
};

// ---- transport adapter -------------------------------------------------
// The panel is usually served by the target itself; typing another loopback
// endpoint retargets the same page (data requests then cross an origin,
// which the transport refuses for non-loopback hosts by design). The core
// hands us its own abort signal per request; the timeout aborts too, and
// either source cancels the fetch.
let endpoint = location.origin;
function fetchText(path, options) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), (options.timeoutS || 10) * 1000);
  const external = options.signal;
  const onAbort = () => controller.abort();
  if (external) {
    if (external.aborted) controller.abort();
    else external.addEventListener("abort", onAbort);
  }
  return fetch(endpoint + path, {
    method: options.method,
    headers: options.headers,
    body: options.body || undefined,
    signal: controller.signal,
  }).then(async (response) => {
    const body = await response.text();
    return { status: response.status, body };
  }).finally(() => {
    clearTimeout(timer);
    if (external) external.removeEventListener("abort", onAbort);
  });
}
// Artifacts are binary (PNG); text decoding would corrupt them.
function fetchBytes(path, options) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), (options.timeoutS || 15) * 1000);
  const external = options.signal;
  const onAbort = () => controller.abort();
  if (external) {
    if (external.aborted) controller.abort();
    else external.addEventListener("abort", onAbort);
  }
  return fetch(endpoint + path, {
    method: options.method,
    headers: options.headers,
    signal: controller.signal,
  }).then(async (response) => {
    const bytes = await response.arrayBuffer();
    return { status: response.status, bytes };
  }).finally(() => {
    clearTimeout(timer);
    if (external) external.removeEventListener("abort", onAbort);
  });
}

const gate = createMutationGate();
const rpc = createRpcClient({ fetchText, fetchBytes }, {
  authorize(method) {
    const descriptor = state.schema && state.schema.methods.find((item) => item.name === method);
    return gate.check(descriptor || { mutating: method !== "System.getInfo" });
  },
});
const events = createEventCursor();
const logs = createLogCursor();
let activeToken = "";
const state = {
  info: null, schema: null, connected: false,
  windows: [], windowId: null, scale: 1,
  snapshot: null, selectedNode: null,
  shot: null, // {meta, bytes, objectUrl}
  polling: null,
  tracking: null, // {operationId, epoch}
};

function secrets() {
  return [activeToken, $("token").value].filter(Boolean);
}
function toast(message, isError) {
  const box = $("toast");
  box.hidden = false;
  box.textContent = redact(String(message), secrets());
  box.className = isError ? "error" : "";
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => { box.hidden = true; }, 4200);
}

// ---- target switching ----------------------------------------------------
// Every target change drops the data of the old target first: tree,
// selection, and screenshot belong to one window at a time.
function clearTargetData() {
  state.snapshot = null;
  state.selectedNode = null;
  $("tree").replaceChildren();
  $("node-details").textContent = "select a node";
  $("node-actions").replaceChildren();
  $("snapshot-meta").textContent = "";
  if (state.shot && state.shot.objectUrl) URL.revokeObjectURL(state.shot.objectUrl);
  state.shot = null;
  $("shot").removeAttribute("src");
  $("shot-empty").hidden = false;
  $("shot-meta").textContent = "";
  sizeOverlay();
}
function idOf(window) { return String(window.windowId ?? window.id); }

// ---- connection ----------------------------------------------------------
function setStatus(text, cls) {
  const node = $("conn-status");
  node.textContent = text;
  node.className = "status " + (cls || "");
}
async function connect() {
  endpoint = $("endpoint").value.trim() || location.origin;
  // A new connection is a new epoch: in-flight results from the previous
  // session resolve as stale and are discarded, never rendered.
  rpc.newEpoch();
  const connectionEpoch = rpc.epoch;
  stopPolling();
  stopTracking("connection changed");
  gate.setAllowed(false);
  $("allow-write").checked = false;
  state.connected = false;
  state.schema = null;
  clearTargetData();
  activeToken = $("token").value;
  rpc.setToken(activeToken);
  if (!rpc.hasToken()) { toast("enter the control token first", true); return; }
  const info = await rpc.call("System.getInfo", {}, 10, { retryOnNetworkError: true });
  if (rpc.epoch !== connectionEpoch) return;
  if (!info.ok) {
    state.connected = false;
    setStatus("auth failed", "bad");
    toast("connect failed: " + (info.error && info.error.message), true);
    return;
  }
  // One logical session for the whole panel: raw input gestures keep one
  // connection identity across down/move/up, and disconnect releases it.
  const session = await rpc.beginSession();
  if (rpc.epoch !== connectionEpoch) return;
  if (!session.ok) {
    setStatus("session failed", "bad");
    toast(session.error, true);
    return;
  }
  const schema = await rpc.fetchSchema();
  if (rpc.epoch !== connectionEpoch) return;
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
    " · " + (identity.instanceId || "") +
    (session.ok ? " · session live" : " · no input session (" + session.error + ")");
  $("connect").disabled = true;
  $("disconnect").disabled = false;
  events.reset();
  logs.reset();
  await refreshWindows();
  renderMethodList();
}
function disconnect() {
  stopPolling();
  stopTracking("disconnected");
  // Release the logical session first so the server drops held input
  // while the token is still set; then advance the epoch.
  rpc.endSession();
  rpc.newEpoch();
  rpc.clearToken();
  gate.setAllowed(false);
  $("allow-write").checked = false;
  $("token").value = "";
  activeToken = "";
  state.connected = false;
  state.info = null; state.schema = null;
  clearTargetData();
  setStatus("disconnected");
  $("connect").disabled = false;
  $("disconnect").disabled = true;
  $("conn-info").textContent = "";
  $("method-list").replaceChildren();
  $("window-select").replaceChildren(el("option", "no window", "", ""));
  $("tab-select").replaceChildren(el("option", "-", "", ""));
  $("window-select").disabled = true;
  $("tab-select").disabled = true;
  state.windowId = null;
  state.windows = [];
  logs.reset();
  events.reset();
  $("logs-list").replaceChildren();
  $("events-list").replaceChildren();
}

// ---- inspect -------------------------------------------------------------
async function refreshWindows() {
  const result = await rpc.call("Window.list", {}, 10, { retryOnNetworkError: true });
  if (!result.ok) { toast("Window.list failed: " + result.error.message, true); return; }
  state.windows = result.result.windows || [];
  const select = $("window-select");
  select.replaceChildren();
  for (const window of state.windows) {
    const label = "#" + idOf(window) + " " + (window.title || "");
    const option = el("option", label);
    option.value = idOf(window);
    select.append(option);
  }
  select.disabled = state.windows.length === 0;
  const keep = state.windows.some((w) => idOf(w) === String(state.windowId));
  if (!keep) {
    const first = state.windows[0];
    state.windowId = first ? idOf(first) : null;
    state.scale = first && first.scale ? Number(first.scale) : 1;
    if (first) select.value = idOf(first);
  } else if (state.windowId !== null) {
    select.value = String(state.windowId);
  }
  await refreshTabs();
  await refreshTree();
}
async function refreshTabs() {
  const select = $("tab-select");
  select.replaceChildren();
  if (!state.windowId) {
    select.disabled = true;
    return;
  }
  // The listing belongs to the target captured here; a switch during the
  // await must not repaint the dropdown with the old window's tabs.
  const targetEpoch = rpc.targetEpoch;
  const windowId = state.windowId;
  const result = await rpc.call("Tab.list", { windowId });
  if (rpc.targetEpoch !== targetEpoch) return;
  if (!result.ok) {
    select.disabled = true;
    return;
  }
  const tabs = result.result.tabs || [];
  for (const tab of tabs) {
    const option = el("option", "#" + String(tab.tabId ?? tab.id) + " " + (tab.title || ""));
    option.value = String(tab.tabId ?? tab.id);
    select.append(option);
  }
  select.disabled = tabs.length === 0;
  const active = result.result.activeTabId ?? result.result.activeTab;
  if (active !== undefined && active !== null) select.value = String(active);
}
async function refreshTree() {
  if (!state.windowId) { $("tree").replaceChildren(el("div", "no windows", "muted")); return; }
  // Snapshot ownership: a late snapshot from the previous target resolves
  // stale and never replaces the newly selected target's tree.
  const targetEpoch = rpc.targetEpoch;
  const params = { windowId: state.windowId };
  const result = await rpc.call("UI.snapshot", params, 10, { retryOnNetworkError: true });
  if (result.stale || rpc.targetEpoch !== targetEpoch) return;
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
async function switchWindow(windowId) {
  if (String(state.windowId) === String(windowId)) return;
  clearTargetData(); // old window's tree/shot/selection never leak
  // A target change advances the target generation: in-flight results from
  // the old window/tab resolve stale at their await points and every
  // multi-step flow stops instead of writing into the new target.
  rpc.newTargetEpoch();
  stopTracking("target changed");
  state.windowId = windowId;
  const window = state.windows.find((w) => idOf(w) === String(windowId));
  state.scale = window && window.scale ? Number(window.scale) : 1;
  await refreshTabs();
  await refreshTree();
}
async function switchTab(tabId) {
  // Selecting a tab is a mutating engine method: the write gate applies.
  const gateCheck = gate.check({ mutating: true });
  if (!gateCheck.permitted) { toast(gateCheck.reason, true); await refreshTabs(); return; }
  const result = await rpc.call("Tab.select",
    { windowId: state.windowId, tabId: String(tabId) });
  if (!result.ok) {
    toast("tab select failed: " + result.error.message, true);
    await refreshTabs();
    return;
  }
  clearTargetData();
  rpc.newTargetEpoch();
  stopTracking("target changed");
  await refreshTree();
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
    const depth = Number(node.parent) ? "  " : "";
    row.append(el("span", depth + nodeTitle(node)));
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
    ["ref", node.node], ["parent", node.parent],
    ["bounds", node.bounds && [node.bounds.x, node.bounds.y,
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
  const check = gate.check({ mutating: true });
  if (!check.permitted) { toast(check.reason, true); return; }
  // The whole multi-step flow belongs to the target captured here: every
  // await re-checks the generation and every step addresses the same
  // windowId, so a target switch mid-flow stops the remaining steps
  // instead of typing into the newly selected window.
  const targetEpoch = rpc.targetEpoch;
  const windowId = state.windowId;
  const stopIfTargetMoved = (step) => {
    if (rpc.targetEpoch !== targetEpoch) {
      toast("target changed - " + step + " cancelled", true);
      return true;
    }
    return false;
  };
  if (action === "click") {
    const params = nodeClickParams(windowId, node, state.snapshot);
    const result = await rpc.call("Input.click", params);
    if (result.stale || stopIfTargetMoved("click")) return;
    if (result.ok) { toast("clicked node " + nodeTitle(node)); await refreshTree(); }
    else toast("click failed: " + result.error.message, true);
  } else if (action === "type") {
    const text = prompt("text to type");
    if (text === null) return;
    // The engine routes typeText to the focused element: click the node
    // first (strict, layout-revision-pinned), then deliver the text - both
    // addressed to the captured window, both re-checked after each await.
    const focused = await rpc.call("Input.click",
      nodeClickParams(windowId, node, state.snapshot));
    if (focused.stale || stopIfTargetMoved("focus")) return;
    if (!focused.ok) { toast("focus failed: " + focused.error.message, true); return; }
    const result = await rpc.call("Input.typeText", { windowId, text });
    if (result.stale || stopIfTargetMoved("type")) return;
    if (!result.ok) toast("type failed: " + result.error.message, true);
  }
}

// ---- capture & input ----------------------------------------------------
async function capture() {
  if (!state.windowId) { toast("select a window first", true); return; }
  // The screenshot belongs to the target captured here: if the target or
  // connection moved between the two awaits, the image is dropped instead
  // of showing window A's pixels over a window B selection.
  const targetEpoch = rpc.targetEpoch;
  const windowId = state.windowId;
  const result = await rpc.call("Capture.screenshot",
    { windowId, frame: "fresh" });
  if (!result.ok) { toast("capture failed: " + result.error.message, true); return; }
  const artifact = await rpc.fetchArtifact(result.result.artifactId);
  if (!artifact.ok) { toast(artifact.error, true); return; }
  if (artifact.stale || rpc.targetEpoch !== targetEpoch) return; // moved mid-capture
  if (state.shot && state.shot.objectUrl) URL.revokeObjectURL(state.shot.objectUrl);
  state.shot = {
    meta: result.result,
    objectUrl: URL.createObjectURL(new Blob([artifact.bytes], { type: "image/png" })),
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
  const origin = state.shot.meta.bounds || { x: 0, y: 0 };
  context.strokeStyle = "#4da3ff";
  context.lineWidth = Math.max(2, canvas.width / 480);
  context.strokeRect(bounds.x - origin.x, bounds.y - origin.y,
                      bounds.width, bounds.height);
}
async function imageClick(event) {
  if (!state.shot) return;
  // The click belongs to the shot's target: capture the generation and
  // window here so a target switch between hover and dispatch never sends
  // window A's coordinates to window B.
  const targetEpoch = rpc.targetEpoch;
  const windowId = state.windowId;
  const rect = $("shot").getBoundingClientRect();
  const px = (event.clientX - rect.left) * ($("shot").naturalWidth / rect.width);
  const py = (event.clientY - rect.top) * ($("shot").naturalHeight / rect.height);
  const point = imageToClient(px, py, state.shot.meta);
  const gateCheck = gate.check({ mutating: true });
  if (!gateCheck.permitted) { toast(gateCheck.reason, true); return; }
  if (rpc.targetEpoch !== targetEpoch) {
    logInput("click dropped: target changed after capture");
    return;
  }
  const result = await rpc.call("Input.click",
    { windowId, x: point.x, y: point.y });
  if (result.stale || rpc.targetEpoch !== targetEpoch) {
    logInput("click result discarded: target changed");
    return;
  }
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
  const normalized = normalizeKeyName($("input-key").value);
  if (!normalized.ok) { toast(normalized.reason, true); return; }
  const gateCheck = gate.check({ mutating: true });
  if (!gateCheck.permitted) { toast(gateCheck.reason, true); return; }
  const result = await rpc.call("Input.key", {
    windowId: state.windowId,
    key: normalized.key,
    kind,
    ctrl: normalized.ctrl,
    shift: normalized.shift,
    alt: normalized.alt,
  });
  logInput("key " + normalized.key + (normalized.ctrl ? "+ctrl" : "") +
           (normalized.shift ? "+shift" : "") + (normalized.alt ? "+alt" : "") +
           " " + kind + ": " + (result.ok ? "ok" : result.error.message));
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
  const bounds = state.selectedNode && state.selectedNode.bounds;
  const point = bounds ? { x: bounds.x + bounds.width / 2, y: bounds.y + bounds.height / 2 }
    : { x: 0, y: 0 };
  const result = await rpc.call("Input.wheel", wheelParams(state.windowId, notches, point));
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
        Object.keys(properties).map((key) => [key, sampleValue(key, properties[key], method)])), null, 2)
    : "{}";
  form.append(area);
  const run = el("button", "invoke");
  run.addEventListener("click", () => invokeMethod(method, area.value));
  form.append(run);
  $("method-result").textContent = "";
  $("method-wait").replaceChildren();
}
function sampleValue(name, property, method) {
  const required = (method.params && method.params.required) || [];
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
  if (result.stale) return;
  $("method-result").textContent = result.ok
    ? JSON.stringify(result.result, null, 2)
    : "error " + result.error.code + ": " + result.error.message;
  const operationId = result.ok && result.result && result.result.operationId;
  if (operationId) trackOperation(String(operationId));
}
function stopTracking(reason) {
  if (state.tracking) {
    state.tracking.stopped = true;
    if (state.tracking.statusNode && reason) {
      state.tracking.statusNode.textContent = "operation tracking stopped: " + reason;
    }
    state.tracking = null;
  }
}
async function trackOperation(operationId) {
  stopTracking("new operation started");
  const waitHost = $("method-wait");
  waitHost.replaceChildren();
  const status = el("span", "operation " + operationId + ": …");
  const cancel = el("button", "cancel");
  cancel.addEventListener("click", async () => {
    const check = gate.check({ mutating: true });
    if (!check.permitted) { toast(check.reason, true); return; }
    const result = await rpc.call("Operation.cancel", { operationId });
    status.textContent = "cancel: " + (result.ok
      ? JSON.stringify(result.result) : result.error.message);
  });
  waitHost.append(status, cancel);
  const tracking = { operationId, epoch: rpc.epoch, statusNode: status, stopped: false };
  state.tracking = tracking;
  for (let i = 0; i < 600; ++i) {
    await new Promise((resolve) => setTimeout(resolve, 250));
    if (tracking.stopped) return;
    if (rpc.epoch !== tracking.epoch) {
      // The connection changed: old-session results must not overwrite the
      // new view, and the loop must not outlive its epoch.
      status.textContent = "operation tracking stopped: connection changed";
      return;
    }
    const result = await rpc.call("Operation.get", { operationId });
    if (result.stale || tracking.stopped) return;
    if (!result.ok) {
      status.textContent = "operation query failed: " + result.error.message;
      return;
    }
    const info = result.result;
    status.textContent = "operation " + operationId + ": " + info.state +
      (info.cancellationRequested ? " (cancel requested)" : "") +
      (info.progress ? " " + info.progress.done + "/" + info.progress.total : "");
    if (["completed", "failed", "cancelled", "providerUnavailable"].includes(info.state)) return;
  }
  status.textContent = "operation tracking stopped (still running)";
}

// ---- events / logs / trace / diagnostics --------------------------------
function stopPolling() {
  if (state.polling) { clearTimeout(state.polling.timer); state.polling = null; }
  $("events-toggle").textContent = "Start polling";
}
async function pollOnce() {
  const result = await rpc.call("Event.poll",
    { since: events.since, max: 100 }, 15);
  if (!state.polling) return;
  if (result.stale) { stopPolling(); return; }
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
  if (!state.connected) { toast("connect first", true); return; }
  const result = await rpc.call("Log.read", logs.nextParams(200), 15,
                                { retryOnNetworkError: true });
  if (!result.ok) { toast("log read failed: " + result.error.message, true); return; }
  const applied = logs.apply(result.result);
  const box = $("logs-list");
  for (const entry of applied.records) {
    box.append(el("div", entry.source + " " + entry.record.sequence +
      " [" + entry.record.level + "] " + entry.record.message));
  }
  while (box.childNodes.length > 800) box.removeChild(box.firstChild);
  box.scrollTop = box.scrollHeight;
  state.logs = Array.from(box.childNodes).map((node) => node.textContent);
}
function reapplyLogLevel() {
  logs.setMinLevel($("log-level").value);
  // The per-source cursors already consumed old records; restarting from
  // zero re-reads history so the new filter applies to everything.
  logs.reset();
  $("logs-list").replaceChildren();
  refreshLogs();
}
async function traceControl(action) {
  if (!state.connected) { toast("connect first", true); return; }
  const result = await rpc.call("Trace." + action, {});
  $("trace-status").textContent = result.ok
    ? JSON.stringify(result.result)
    : "Trace." + action + " failed: " + result.error.message;
}
function downloadDiagnostics() {
  const text = diagnosticsSnapshot({
    info: state.info,
    windows: state.windows,
    selectedWindow: state.windowId,
    snapshotMeta: state.snapshot && {
      revision: state.snapshot.revision, layoutRevision: state.snapshot.layoutRevision,
      documentGeneration: state.snapshot.documentGeneration,
    },
    eventsCursor: { since: events.since, gaps: events.gapCount },
    logCursors: logs.positions,
    logLevel: logs.minLevel,
    recentLogs: state.logs,
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
$("window-select").addEventListener("change", (event) => switchWindow(event.target.value));
$("tab-select").addEventListener("change", (event) => switchTab(event.target.value));
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
$("schema-refresh").addEventListener("click", async () => {
  if (!state.connected) { toast("connect first", true); return; }
  const schema = await rpc.fetchSchema();
  if (schema.stale) return;
  if (!schema.ok) { toast("schema refresh failed: " + schema.error, true); return; }
  state.schema = schema.result;
  renderMethodList();
  toast("method catalogue refreshed");
});
$("events-toggle").addEventListener("click", () => {
  if (state.polling) { stopPolling(); return; }
  if (!state.connected) { toast("connect first", true); return; }
  state.polling = { timer: 0, interval: 700 };
  $("events-toggle").textContent = "Stop polling";
  pollOnce();
});
$("events-clear").addEventListener("click", () => $("events-list").replaceChildren());
$("logs-refresh").addEventListener("click", refreshLogs);
$("log-level").addEventListener("change", reapplyLogLevel);
$("trace-start").addEventListener("click", () => traceControl("start"));
$("trace-status-button").addEventListener("click", () => traceControl("status"));
$("trace-stop").addEventListener("click", () => traceControl("stop"));
$("diag-download").addEventListener("click", downloadDiagnostics);
window.addEventListener("beforeunload", () => {
  stopPolling();
  stopTracking("page closed");
  rpc.endSession();
});
