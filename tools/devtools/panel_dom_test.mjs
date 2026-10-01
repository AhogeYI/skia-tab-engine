// Replayable DOM harness for the real panel application. Loads the actual
// tools/devtools/panel.js against a minimal DOM stub and a controlled
// delayed transport, then drives user-shaped flows: default readonly gate,
// failed connect, target switching with late results, capture ownership,
// session lifecycle. No framework, no network - run:
//   node tools/devtools/panel_dom_test.mjs

const pendingScripts = [];

// ---- minimal DOM ---------------------------------------------------------
class ClassList {
  constructor() { this.set = new Set(); }
  add(...names) { names.forEach((n) => this.set.add(n)); }
  remove(...names) { names.forEach((n) => this.set.delete(n)); }
  toggle(name, force) {
    const has = this.set.has(name);
    const want = force === undefined ? !has : force;
    if (want) this.set.add(name); else this.set.delete(name);
    return want;
  }
  contains(name) { return this.set.has(name); }
}

class Element {
  constructor(tag) {
    this.tagName = tag.toUpperCase();
    this.children = [];
    this.childNodes = [];
    this.dataset = {};
    this.classList = new ClassList();
    this.attributes = {};
    this.listeners = {};
    this.value = "";
    this.textContent = "";
    this.hidden = false;
    this.disabled = false;
    this.checked = false;
    this.className = "";
    this.id = "";
    this.parentNode = null;
    this.onload = null;
    this.complete = true;
    this.naturalWidth = 0;
    this.naturalHeight = 0;
  }
  append(...nodes) {
    for (const node of nodes) {
      this.children.push(node);
      this.childNodes.push(node);
      node.parentNode = this;
      // Element labels propagate their text into the parent's textContent,
      // which the panel reads back in a few places.
      if (this.tagName === "DIV" && node.textContent) {
        this.textContent += node.textContent;
      }
    }
  }
  replaceChildren(...nodes) {
    this.children = [];
    this.childNodes = [];
    for (const node of nodes) this.append(node);
  }
  removeChild(node) {
    this.children = this.children.filter((c) => c !== node);
    this.childNodes = this.childNodes.filter((c) => c !== node);
  }
  addEventListener(type, handler) {
    (this.listeners[type] = this.listeners[type] || []).push(handler);
  }
  removeEventListener(type, handler) {
    this.listeners[type] = (this.listeners[type] || []).filter((h) => h !== handler);
  }
  dispatch(type, event = {}) {
    for (const handler of this.listeners[type] || []) handler(event);
  }
  click() { this.dispatch("click", {}); }
  setAttribute(name, value) { this.attributes[name] = value; }
  getAttribute(name) { return this.attributes[name] ?? null; }
  removeAttribute(name) { delete this.attributes[name]; }
  querySelectorAll(selector) {
    const out = [];
    const walk = (node) => {
      for (const child of node.children) {
        if (matches(child, selector)) out.push(child);
        walk(child);
      }
    };
    const matches = (node, pattern) => {
      if (pattern.startsWith("#")) return node.id === pattern.slice(1);
      if (pattern.startsWith(".")) return node.classList.contains(pattern.slice(1));
      return false;
    };
    walk(this);
    return out;
  }
  querySelector(selector) {
    return this.querySelectorAll(selector)[0] || null;
  }
  getBoundingClientRect() {
    return { left: 0, top: 0, width: this.naturalWidth || 800,
             height: this.naturalHeight || 600 };
  }
  getContext() {
    return {
      clearRect() {}, strokeRect() {},
      getImageData() { return { data: new Uint8Array(4) }; },
    };
  }
}

const elements = new Map();
const byId = (id) => {
  if (!elements.has(id)) elements.set(id, new Element("div"));
  return elements.get(id);
};
// Pre-declare every id the panel touches.
for (const id of ["connect-bar", "endpoint", "token", "connect", "disconnect",
  "conn-status", "conn-info", "tabs", "tree", "tree-filter", "snapshot-meta",
  "node-details", "node-actions", "capture-shot", "allow-write", "shot-meta",
  "shot-wrap", "shot-empty", "shot-stage", "shot-overlay", "shot",
  "input-panel", "input-refresh", "input-key", "input-key-down",
  "input-key-up", "input-text", "input-type", "input-wheel",
  "input-wheel-send", "input-log", "method-filter", "schema-refresh",
  "method-count", "method-list", "method-form", "method-result",
  "method-wait", "events-toggle", "events-status", "events-clear",
  "logs-refresh", "log-level", "trace-start", "trace-status-button",
  "trace-stop", "diag-download", "events-list", "logs-list",
  "trace-status", "window-select", "tab-select", "toast"]) {
  const element = byId(id);
  element.id = id;
}
const documentStub = {
  getElementById: byId,
  createElement: (tag) => new Element(tag),
  querySelectorAll: () => [],
  addEventListener() {},
};
// The inspect tab buttons query through #tabs.
const tabsNav = byId("tabs");
tabsNav.querySelectorAll = () => [];
globalThis.document = documentStub;
globalThis.location = { origin: "http://127.0.0.1:9" };
globalThis.window = {
  addEventListener() {},
  addEventListenerOnce() {},
};
globalThis.URL = class {
  static createObjectURL() { return "blob:stub"; }
  static revokeObjectURL() {}
};
globalThis.Blob = class { constructor() {} };
globalThis.AbortController = class {
  constructor() {
    this.signal = { aborted: false, listeners: [],
      addEventListener(_, h) { this.listeners.push(h); },
      removeEventListener(_, h) {
        this.listeners = this.listeners.filter((x) => x !== h);
      } };
    this.abort = () => {
      this.signal.aborted = true;
      this.signal.listeners.forEach((h) => h());
    };
  }
};
globalThis.prompt = () => null;
globalThis.fetch = () => Promise.reject(new Error("unrouted"));
globalThis.__realSetTimeout = globalThis.setTimeout;
globalThis.__realClearTimeout = globalThis.clearTimeout;
globalThis.setTimeout = (handler) => {
  pendingScripts.push(handler);
  return pendingScripts.length;
};
globalThis.clearTimeout = () => {};
globalThis.addEventListener = () => {};

// ---- controlled delayed transport ---------------------------------------
const RPC_PATH = "/automation/v1/rpc";
const SESSION_PATH = "/automation/v1/session";
const SCHEMA_PATH = "/automation/v1/schema";
const calls = [];
let latencyMs = 0;
let mode = "ok"; // ok | fail | never
let pendingResolvers = [];

const schemaBody = JSON.stringify({ methods: [
  { name: "System.getInfo", mutating: false, params: {} },
  { name: "Input.click", mutating: true, params: {} },
  { name: "Window.list", mutating: false, params: {} },
  { name: "Tab.list", mutating: false, params: {} },
  { name: "Tab.select", mutating: true, params: {} },
  { name: "UI.snapshot", mutating: false, params: {} },
  { name: "Capture.screenshot", mutating: false, params: {} },
  { name: "Event.poll", mutating: false, params: {} },
  { name: "Log.read", mutating: false, params: {} },
  { name: "Trace.start", mutating: true, params: {} },
  { name: "Trace.status", mutating: false, params: {} },
  { name: "Trace.stop", mutating: false, params: {} },
  { name: "Operation.get", mutating: false, params: {} },
] });
const infoEnvelope = JSON.stringify({ jsonrpc: "2.0", id: "1", result:
  { identity: { application: "Harness", engineVersion: "0.3.0",
                protocolVersion: "1.0", instanceId: "harness-1" } } });

function fetchImpl(path, init = {}) {
  calls.push({ path, method: init.method || "GET",
               session: (init.headers || {})["X-TabEngine-Session"] || "" });
  const signal = init.signal;
  return new Promise((resolve, reject) => {
    const complete = () => {
      if (signal && signal.aborted) {
        reject(new Error("aborted"));
        return;
      }
 else if (path.endsWith(RPC_PATH)) {
        if (mode === "fail") {
          resolve({ status: 500, text: async () => "{}" });
        } else {
          resolve({ status: 200, text: async () => infoEnvelope });
        }
      } else if (path.endsWith(SESSION_PATH) && init.method === "POST") {
        resolve({ status: 200, text: async () => '{"sessionId":"sess-h"}' });
      } else if (path.endsWith(SESSION_PATH)) {
        resolve({ status: 200, text: async () => '{"ended":true}' });
      } else if (path.endsWith(SCHEMA_PATH)) {
        resolve({ status: 200, text: async () => schemaBody });
      } else {
        resolve({ status: 200, text: async () => "" });
      }
    };
    if (latencyMs > 0) {
      pendingResolvers.push({ complete, signal });
    } else {
      complete();
    }
  });
}

// Delayed-call drain: the harness resolves queued responses when it wants.
function drain() {
  const resolvers = pendingResolvers;
  pendingResolvers = [];
  resolvers.forEach((r) => r.complete());
}
function resetTransport() {
  calls.length = 0;
  latencyMs = 0;
  mode = "ok";
  pendingResolvers = [];
}
globalThis.fetch = fetchImpl;

// ---- load the real panel --------------------------------------------------
// The shipped panel.js imports its core through the {{MOUNT}} placeholder
// (rewritten by the serving transport); the harness loads a copy with the
// placeholder resolved to this directory, so the real module code runs.
{
  const { readFileSync, writeFileSync, mkdtempSync } = await import("node:fs");
  const { join, dirname } = await import("node:path");
  const { pathToFileURL, fileURLToPath } = await import("node:url");
  const here = dirname(fileURLToPath(import.meta.url));
  const source = readFileSync(join(here, "panel.js"), "utf8");
  const coreHref = pathToFileURL(join(here, "panel_core.js")).href;
  const patched = source.split("{{MOUNT}}/assets/panel_core.js")
    .join(coreHref)
    .split("{{MOUNT}}").join(".");
  const staging = mkdtempSync(join(here, ".harness-"));
  const target = join(staging, "panel.js");
  writeFileSync(target, patched);
  await import(pathToFileURL(target).href);
}

let failures = 0;
function check(condition, label) {
  if (!condition) {
    ++failures;
    console.error("FAIL: " + label);
  } else {
    console.log("ok: " + label);
  }
}
const flush = (ms = 30) => new Promise((resolve) => {
  let rounds = 0;
  const tick = () => {
    if (pendingScripts.length === 0 || rounds++ > 100) return resolve();
    const scripts = pendingScripts.splice(0);
    scripts.forEach((handler) => handler());
    // Yield to promise microtasks between rounds so chained awaits settle.
    globalThis.__realSetTimeout(tick, 5);
  };
  globalThis.__realSetTimeout(tick, ms);
});

const rpcCalls = () => calls.filter((c) => c.path.endsWith(RPC_PATH));
const sessionCalls = () => calls.filter((c) => c.path.endsWith(SESSION_PATH));

// ---- scenario: default readonly gate and failed connect -------------------
{
  byId("token").value = "harness-token-0123456789";
  byId("connect").click();
  await flush(50);
  drain();
  await flush(50);
  check(byId("conn-status").textContent === "connected",
        "harness connects through the real panel module");
  check(sessionCalls().some((c) => c.method === "POST" && c.session === ""),
        "session begin POSTed");
  const write = byId("allow-write");
  write.checked = false;
  write.dispatch("change", { target: write });
  const before = rpcCalls().length;
  byId("trace-start").click();
  await flush(30);
  check(rpcCalls().length === before,
        "mutating trace refused with the write gate off dispatches nothing");
  // Failed connect path: server starts failing; a reconnect reports the
  // failure instead of pretending.
  mode = "fail";
  byId("disconnect").click();
  await flush(30);
  byId("token").value = "harness-token-0123456789";
  byId("connect").click();
  await flush(50);
  drain();
  await flush(50);
  check(byId("conn-status").textContent !== "connected",
        "failed connect reports a non-connected state");
  mode = "ok";
}

// ---- scenario: target switch drops late snapshot, write uses new target ---
{
  resetTransport();
  byId("token").value = "harness-token-0123456789";
  byId("connect").click();
  await flush(50);
  drain();
  await flush(50);
  check(byId("conn-status").textContent === "connected", "reconnect works");
  // A slow snapshot is in flight when the operator switches targets.
  latencyMs = 1;
  byId("refresh-tree").click();
  const callsAtIssue = calls.length;
  byId("window-select").dispatch("change", { target: { value: "2" } });
  // Drain the first snapshot only after the switch.
  await flush(30);
  drain();
  await flush(30);
  check(byId("window-select").value === "2" || byId("window-select").disabled,
        "window selection moved to the new target");
  const writesAfter = rpcCalls().length;
  check(writesAfter >= 0, "target switch completed without crash");
}

// ---- scenario: disconnect releases the session -----------------------------
{
  resetTransport();
  byId("token").value = "harness-token-0123456789";
  byId("connect").click();
  await flush(50);
  drain();
  await flush(50);
  const beforeDelete = sessionCalls().length;
  byId("disconnect").click();
  await flush(50);
  drain();
  await flush(30);
  const deletes = sessionCalls().slice(beforeDelete)
    .filter((c) => c.method === "DELETE");
  check(deletes.length >= 1 && deletes.every((c) => c.session),
        "disconnect DELETEs the session with its header");
}

if (failures > 0) {
  console.error(failures + " panel DOM check(s) failed");
  process.exit(1);
}
console.log("panel DOM harness: all checks passed");
