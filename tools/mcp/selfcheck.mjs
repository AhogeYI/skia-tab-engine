#!/usr/bin/env node
// Self-check: drives the MCP adapter through a real stdio handshake and
// one live instance (endpoint + token from the environment), the same
// contract an MCP client uses. Exit 0 = pass.
//
//   TABENGINE_ENDPOINT=http://127.0.0.1:PORT TABENGINE_TOKEN=... node selfcheck.mjs
//
// Additional env: TABENGINE_ALLOW_MUTATIONS=1 also verifies a mutating
// tool; otherwise the check verifies the refusal.

import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

class McpClient {
  constructor(process_) {
    this.process = process_;
    this.nextId = 1;
    this.pending = new Map();
    let buffer = "";
    this.process.stdout.on("data", (chunk) => {
      buffer += chunk.toString("utf-8");
      let index;
      while ((index = buffer.indexOf("\n")) >= 0) {
        const line = buffer.slice(0, index);
        buffer = buffer.slice(index + 1);
        if (!line.trim()) continue;
        const message = JSON.parse(line);
        if (message.id !== undefined && this.pending.has(message.id)) {
          const settle = this.pending.get(message.id);
          settle(message);
        }
      }
    });
  }

  request(method, params) {
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      const timeout = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error("timeout on " + method));
      }, 30000);
      this.pending.set(id, (message) => {
        clearTimeout(timeout);
        this.pending.delete(id);
        if (message.error) reject(new Error(message.error.message));
        else resolve(message.result);
      });
      this.process.stdin.write(
        JSON.stringify({ jsonrpc: "2.0", id, method, params }) + "\n");
    });
  }

  notify(method, params) {
    this.process.stdin.write(
      JSON.stringify({ jsonrpc: "2.0", method, params }) + "\n");
  }
}

function fail(message) {
  console.error("SELFCHECK FAIL: " + message);
  process.exitCode = 1;
}

const endpoint = process.env.TABENGINE_ENDPOINT;
const token = process.env.TABENGINE_TOKEN;
if (!endpoint || !token) {
  console.error("SELFCHECK: set TABENGINE_ENDPOINT and TABENGINE_TOKEN");
  process.exit(2);
}

const adapter = spawn(process.execPath, [fileURLToPath(new URL("server.mjs", import.meta.url))], {
  env: process.env,
  stdio: ["pipe", "pipe", "inherit"],
});

const client = new McpClient(adapter);
try {
  const initialized = await client.request("initialize", {
    protocolVersion: "2025-06-18",
    capabilities: {},
    clientInfo: { name: "tabengine-selfcheck", version: "0.3.0" },
  });
  if (!initialized.serverInfo || initialized.serverInfo.name !== "tabengine-mcp") {
    fail("unexpected serverInfo: " + JSON.stringify(initialized.serverInfo));
  }
  client.notify("notifications/initialized", {});

  const tools = await client.request("tools/list", {});
  const names = tools.tools.map((tool) => tool.name);
  for (const expected of ["get_info", "list_methods", "call_method", "snapshot",
                          "click", "type_text", "screenshot", "wait", "poll_events"]) {
    if (!names.includes(expected)) fail(`tools/list misses '${expected}'`);
  }
  console.log("tools/list:", names.join(", "));

  const info = await client.request("tools/call", { name: "get_info", arguments: {} });
  const infoText = (info.content || []).map((part) => part.text || "").join("");
  if (!infoText.includes("instanceId")) fail("get_info lacks instanceId: " + infoText);
  console.log("get_info ok");

  const events = await client.request("tools/call",
    { name: "poll_events", arguments: {} });
  if (events.isError) fail("poll_events failed");
  console.log("poll_events ok");

  const shot = await client.request("tools/call", { name: "screenshot", arguments: {} });
  const image = (shot.content || []).find((part) => part.type === "image");
  if (!image || !image.data || image.mimeType !== "image/png") {
    fail("screenshot returned no PNG image content");
  } else {
    console.log("screenshot ok:", Math.round(image.data.length * 3 / 4 / 1024) + " KiB png");
  }

  const click = await client.request("tools/call", {
    name: "click",
    arguments: { windowId: "1", x: 10, y: 10 },
  });
  if (process.env.TABENGINE_ALLOW_MUTATIONS === "1") {
    if (click.isError) fail("click failed under allow-mutations: " + JSON.stringify(click.content));
    console.log("click ok (mutations allowed)");
  } else {
    if (!click.isError) fail("click ran without TABENGINE_ALLOW_MUTATIONS");
    console.log("click refused without TABENGINE_ALLOW_MUTATIONS ok");
  }

  if (process.exitCode !== 1) console.log("SELFCHECK PASS");
} catch (error) {
  fail(error.message);
} finally {
  adapter.stdin.end();
}
