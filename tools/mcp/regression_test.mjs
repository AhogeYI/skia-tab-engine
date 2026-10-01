// Protocol regressions without windows, user data, or a graphics device.
import assert from "node:assert/strict";
import { createServer } from "node:http";
import { spawn } from "node:child_process";
import { mkdtempSync, writeFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

const token = "fixture-token-with-enough-entropy";
const calls = [];
let mutate = false;
let sessions = 0;
let closes = 0;
const host = createServer(async (req, res) => {
  assert.equal(req.headers.authorization, "Bearer " + token);
  res.setHeader("Content-Type", "application/json");
  if (req.url.endsWith("/session")) {
    if (req.method === "DELETE") ++closes;
    else ++sessions;
    res.end(JSON.stringify({ sessionId: "fixture-session" }));
    return;
  }
  let body = "";
  for await (const chunk of req) body += chunk;
  const request = JSON.parse(body);
  calls.push({ ...request, session: req.headers["x-tabengine-session"] });
  let result = {};
  if (request.method === "System.getInfo") result = { instanceId: "fixture" };
  if (request.method === "System.getSchema") result = { methods: [
    { name: "app.fixture.dynamic", mutating: mutate },
    { name: "Input.pointer", mutating: true },
  ] };
  if (request.method === "Event.poll") {
    const since = request.params.since || 0;
    result = { events: since < 7 ? [{ sequence: 7 }] : [], next: 7 };
  }
  res.end(JSON.stringify({ jsonrpc: "2.0", id: request.id, result }));
});
await new Promise((resolve) => host.listen(0, "127.0.0.1", resolve));
const endpoint = `http://127.0.0.1:${host.address().port}`;

if (process.argv.includes("--host")) {
  writeFileSync(process.env.TABENGINE_TOKEN_FILE, token);
  writeFileSync(process.env.TABENGINE_LAUNCH_DESCRIPTOR, JSON.stringify({ endpoint }));
  console.log("This host stdout must never enter the MCP stream.");
} else {
  const root = mkdtempSync(join(tmpdir(), "tabengine-mcp-"));
  const children = [];
  async function connect(extra) {
    const env = { ...process.env };
    for (const key of Object.keys(env)) if (key.startsWith("TABENGINE_")) delete env[key];
    const child = spawn(process.execPath, [fileURLToPath(new URL("server.mjs", import.meta.url))], {
      env: { ...env, ...extra }, stdio: ["pipe", "pipe", "pipe"], windowsHide: true,
    });
    children.push(child);
    const pending = new Map();
    let buffer = "", next = 1;
    child.stdout.on("data", (chunk) => {
      buffer += chunk;
      while (buffer.includes("\n")) {
        const index = buffer.indexOf("\n");
        const message = JSON.parse(buffer.slice(0, index));
        buffer = buffer.slice(index + 1);
        if (pending.has(message.id)) pending.get(message.id)(message);
      }
    });
    const request = (method, params) => new Promise((resolve, reject) => {
      const id = next++;
      const timeout = setTimeout(() => reject(new Error(`timeout: ${method}`)), 25000);
      pending.set(id, (message) => {
        clearTimeout(timeout); pending.delete(id);
        if (message.error) reject(new Error(message.error.message));
        else resolve(message.result);
      });
      child.stdin.write(JSON.stringify({ jsonrpc: "2.0", id, method, params }) + "\n");
    });
    await request("initialize", { protocolVersion: "2025-06-18", capabilities: {},
      clientInfo: { name: "regression", version: "1" } });
    child.stdin.write(JSON.stringify({ jsonrpc: "2.0", method: "notifications/initialized" }) + "\n");
    return { child, call: (name, args = {}) => request("tools/call", { name, arguments: args }) };
  }
  async function disconnect(child) {
    const exited = new Promise((resolve) => child.once("exit", resolve));
    child.stdin.end();
    await exited;
  }
  try {
    const readOnly = await connect({ TABENGINE_ENDPOINT: endpoint, TABENGINE_TOKEN: token });
    const first = await readOnly.call("poll_events");
    const cursor = JSON.parse(first.content[0].text).next;
    const second = await readOnly.call("poll_events", { after: cursor });
    assert.deepEqual(JSON.parse(second.content[0].text).events, []);
    assert.equal(calls.at(-1).params.since, 7);
    assert.ok(!(await readOnly.call("call_method", { method: "app.fixture.dynamic" })).isError);
    mutate = true; // A provider changes the catalogue during the same session.
    assert.ok((await readOnly.call("call_method", { method: "app.fixture.dynamic" })).isError);
    await disconnect(readOnly.child);

    const input = await connect({ TABENGINE_ENDPOINT: endpoint, TABENGINE_TOKEN: token,
      TABENGINE_ALLOW_MUTATIONS: "1" });
    await input.call("type_text", { windowId: "1", text: "hello" });
    await input.call("call_method", { method: "Input.pointer", params: { windowId: "1" } });
    const inputCalls = calls.filter((call) => call.method.startsWith("Input."));
    assert.equal(inputCalls[0].method, "Input.typeText");
    assert.equal(inputCalls[0].session, "fixture-session");
    assert.equal(inputCalls[1].session, "fixture-session");
    assert.equal(sessions, 1);
    await disconnect(input.child);
    assert.equal(closes, 1);

    const launched = await connect({ TABENGINE_LAUNCH: process.execPath,
      TABENGINE_LAUNCH_ARGS: JSON.stringify([fileURLToPath(import.meta.url), "--host"]),
      TABENGINE_LAUNCH_DESCRIPTOR: join(root, "descriptor.json"),
      TABENGINE_TOKEN_FILE: join(root, "token") });
    assert.ok(!(await launched.call("get_info")).isError);
    const descriptor = JSON.parse((await import("node:fs")).readFileSync(join(root, "descriptor.json")));
    await disconnect(launched.child);
    await assert.rejects(fetch(descriptor.endpoint, { signal: AbortSignal.timeout(2000) }));
    console.log("MCP regressions passed: cursor, dynamic grants, sessions, launch and cleanup");
  } finally {
    for (const child of children) if (child.exitCode === null) child.kill();
    host.closeAllConnections();
    await new Promise((resolve) => host.close(resolve));
    rmSync(root, { recursive: true, force: true });
  }
}
