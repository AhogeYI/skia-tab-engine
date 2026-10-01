// Replayable DOM harness for the real panel application. Loads the actual
// tools/devtools/panel.js against a minimal DOM stub and a controlled
// delayed transport, then drives user-shaped flows: default readonly gate,
// failed connect, target switching with late results, capture ownership,
// session lifecycle. No framework, no network - run:
//   node tools/devtools/panel_dom_test.mjs



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
  removeAttribute(name) { delete this.attributes[name]; delete this[name]; }
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
      if (pattern.startsWith(".")) return node.classList.contains(pattern.slice(1)) || node.className.split(" ").includes(pattern.slice(1));
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
globalThis.prompt = () => null;
globalThis.fetch = () => Promise.reject(new Error("unrouted"));
// Use actual cancellable timers, unreferenced so toast timers cannot hold
// the test process alive. Network delays are explicit deferred responses.
const realSetTimeout = globalThis.setTimeout;
globalThis.setTimeout = (handler, delay) => realSetTimeout(handler, delay).unref();
globalThis.addEventListener = () => {};

const RPC_PATH = "/automation/v1/rpc";
const SESSION_PATH = "/automation/v1/session";
const SCHEMA_PATH = "/automation/v1/schema";
const calls = [];
const held = [];
let holdMethod = null;
let failConnect = false;
let sessionSerial = 0;
const schemaBody = JSON.stringify({methods: [
  ["System.getInfo", false], ["Window.list", false], ["Tab.list", false],
  ["Tab.select", true], ["UI.snapshot", false], ["Capture.screenshot", false],
  ["Input.click", true], ["Input.typeText", true], ["Input.key", true],
  ["Input.wheel", true], ["Trace.start", true], ["Trace.status", false],
  ["Trace.stop", true], ["Log.read", false], ["Operation.get", false],
].map(([name, mutating]) => ({name, mutating, params:{type:"object", properties:{}}}))});

function fetchImpl(path, init = {}) {
  const payload = init.body ? JSON.parse(init.body) : {};
  const method = payload.method || (path.includes("/artifacts/") ? "Artifact.get" : "");
  const call = {path, method, verb:init.method || "GET", params:payload.params || {},
    session:(init.headers || {})["X-TabEngine-Session"] || ""};
  calls.push(call);
  return new Promise((resolve, reject) => {
    const complete = () => {
      if (init.signal && init.signal.aborted) { reject(new Error("aborted")); return; }
      let status = 200;
      let body = {};
      if (path.endsWith(SESSION_PATH)) {
        body = init.method === "POST" ? {sessionId:"session-" + (++sessionSerial)} : {ended:true};
      } else if (path.endsWith(SCHEMA_PATH)) {
        resolve({status, text:async()=>schemaBody}); return;
      } else if (path.endsWith(RPC_PATH)) {
        const windowId = String(call.params.windowId || "1");
        let result = {};
        switch (method) {
          case "System.getInfo":
            if (failConnect) status = 500;
            result = {identity:{application:"Harness", instanceId:"fixture", engineVersion:"0.3.0"}};
            break;
          case "Window.list": result = {windows:[{id:"1",title:"A",scale:1},{id:"2",title:"B",scale:1}]}; break;
          case "Tab.list": result = {tabs:[{tabId:"10",active:true,title:"Page"}]}; break;
          case "UI.snapshot": result = {revision:10,layoutRevision:20,nodes:[{
            node:windowId === "1" ? "101" : "201", parent:"0", stableKey:"edit-" + windowId,
            role:"edit",name:"Window " + windowId, visible:true,enabled:true,actions:["click","type"],
            bounds:{x:10,y:10,width:100,height:30},clip:{x:10,y:10,width:100,height:30},
          }]}; break;
          case "Capture.screenshot": result = {artifactId:"shot-"+windowId,width:100,height:100,
            scale:1,bounds:{x:0,y:0,width:100,height:100}}; break;
        }
        body = {jsonrpc:"2.0",id:payload.id,result};
      }
      resolve({status, text:async()=>JSON.stringify(body),
        arrayBuffer:async()=>new Uint8Array([1,2,3,4]).buffer});
    };
    if (method && method === holdMethod) held.push({complete, call}); else complete();
  });
}
globalThis.fetch = fetchImpl;

// A temporary import copy resolves the service's mount placeholder without
// leaving patched sources in the repository or the SDK resource directory.
{
  const {readFileSync,writeFileSync,mkdtempSync,rmSync} = await import("node:fs");
  const {join,dirname,resolve,basename} = await import("node:path");
  const {tmpdir} = await import("node:os");
  const {pathToFileURL,fileURLToPath} = await import("node:url");
  const here = dirname(fileURLToPath(import.meta.url));
  const staging = mkdtempSync(join(tmpdir(),"tabengine-panel-"));
  if (dirname(resolve(staging)) !== resolve(tmpdir()) ||
      !basename(staging).startsWith("tabengine-panel-")) throw new Error("unsafe harness path");
  try {
    const source = readFileSync(join(here,"panel.js"),"utf8")
      .split("{{MOUNT}}/assets/panel_core.js").join(pathToFileURL(join(here,"panel_core.js")).href)
      .split("{{MOUNT}}").join(".");
    const target = join(staging,"panel.mjs");
    writeFileSync(target,source);
    await import(pathToFileURL(target).href);
  } finally { rmSync(staging,{recursive:true,force:true}); }
}
let failures = 0;
function check(condition,label) {
  if (!condition) { ++failures; console.error("FAIL: "+label); }
  else console.log("ok: "+label);
}
async function flush() { for (let i=0;i<150;++i) await Promise.resolve(); }
async function click(id) { byId(id).click(); await flush(); }
async function switchWindow(id) {
  byId("window-select").value=id;
  byId("window-select").dispatch("change",{target:byId("window-select")});
  await flush();
}
function setWrite(allowed) {
  byId("allow-write").checked=allowed;
  byId("allow-write").dispatch("change",{target:byId("allow-write")});
}
function treeKey() { return byId("tree").children[0]?.children[0]?.textContent; }
async function connect() {
  byId("token").value="harness-token-0123456789";
  await click("connect");
}
async function releaseHeld() {
  const responses=held.splice(0);
  responses.forEach(({complete})=>complete());
  await flush();
}
function selectEdit() {
  const row=byId("tree").children[0];
  if (!row) throw new Error("missing real snapshot row");
  row.click();
  const button=byId("node-actions").children.find((n)=>n.textContent === "type");
  if (!button) throw new Error("missing real type action");
  return button;
}

await connect();
check(byId("conn-status").textContent === "connected","real panel connects");
check(treeKey() === "edit-1","transport supplies an actual window A tree");
const before=calls.length;
await click("trace-start");
check(calls.length === before,"default write gate sends zero mutating requests");
await click("disconnect");
failConnect=true;
await connect();
check(byId("conn-status").textContent !== "connected","failed Connect is reported");
failConnect=false;
await connect();

// A late real snapshot from A cannot repaint B's tree.
holdMethod="UI.snapshot";
await click("refresh-tree");
check(held.length === 1,"window A snapshot is genuinely deferred");
holdMethod=null;
await switchWindow("2");
check(treeKey() === "edit-2","window B tree is painted before A responds");
await releaseHeld();
check(treeKey() === "edit-2","late A snapshot cannot replace B's tree");

// Drive the real focus-then-type action, with the focus response deferred.
await switchWindow("1");
setWrite(true);
globalThis.prompt=()=>"fixture text";
holdMethod="Input.click";
selectEdit().click();
await flush();
check(held.length === 1 && held[0].call.params.windowId === "1","focus belongs to A");
const typeBefore=calls.filter((c)=>c.method === "Input.typeText").length;
holdMethod=null;
await switchWindow("2");
await releaseHeld();
check(calls.filter((c)=>c.method === "Input.typeText").length === typeBefore,
      "target switch cancels remaining type steps, with zero text writes");
check(treeKey() === "edit-2","cancelled focus flow preserves B's tree");

// A's pixels remain private to A even if the artifact arrives after B.
await switchWindow("1");
holdMethod="Artifact.get";
await click("capture-shot");
check(held.length === 1,"screenshot artifact is genuinely deferred");
holdMethod=null;
await switchWindow("2");
await releaseHeld();
const clicksBefore=calls.filter((c)=>c.method === "Input.click").length;
byId("shot").dispatch("click",{clientX:20,clientY:20});
await flush();
check(calls.filter((c)=>c.method === "Input.click").length === clicksBefore,
      "late old screenshot has no clickable image on B");

// A held key uses one session; switching releases it before new input.
await switchWindow("1");
byId("input-key").value="enter";
await click("input-key-down");
const down=calls.filter((c)=>c.method === "Input.key").at(-1);
check(down && down.params.windowId === "1" && down.session,"held key is session scoped on A");
const start=calls.length;
await switchWindow("2");
check(calls.slice(start).some((c)=>c.path.endsWith(SESSION_PATH) &&
  c.verb === "DELETE" && c.session === down.session),"target switch releases A's held input session");
await click("input-key-up");
const up=calls.filter((c)=>c.method === "Input.key").at(-1);
check(up.params.windowId === "2" && up.session !== down.session,"B input uses a fresh session");
await click("disconnect");
check(calls.at(-1).verb === "DELETE" && calls.at(-1).session === up.session,
      "disconnect releases the current session");
if (failures) { console.error(failures+" panel DOM checks failed");process.exit(1); }
console.log("panel DOM harness: all checks passed");
