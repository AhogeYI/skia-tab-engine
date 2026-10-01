#!/usr/bin/env node
// TabEngine MCP adapter: exposes an instance's automation JSON-RPC surface
// as Model Context Protocol tools over stdio.
//
// Connection (pick one):
//   TABENGINE_ENDPOINT + TABENGINE_TOKEN          attach to a running host
//   TABENGINE_DESCRIPTOR <file> [TABENGINE_TOKEN | TABENGINE_TOKEN_FILE]
//                                                  attach by descriptor file
//   TABENGINE_LAUNCH <command> + TABENGINE_LAUNCH_DESCRIPTOR <file>
//                                                  explicit launch; the
//                                                  command must write the
//                                                  descriptor (and a token
//                                                  file when TABENGINE_TOKEN_FILE
//                                                  points at it)
//
// Safety: mutating tools (clicks, keys, arbitrary mutating methods) are
// refused unless TABENGINE_ALLOW_MUTATIONS=1. The token never appears in
// tool output, logs, or resource URIs.

import { readFileSync, existsSync } from "node:fs";
import { spawn } from "node:child_process";
import { setTimeout as delay } from "node:timers/promises";
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";

const PROTOCOL_PREFIX = "/automation/v1";
const allowMutations = process.env.TABENGINE_ALLOW_MUTATIONS === "1";

function fail(message) {
  console.error("tabengine-mcp: " + message);
  process.exit(2);
}

let endpoint = process.env.TABENGINE_ENDPOINT || "";
let token = process.env.TABENGINE_TOKEN || "";
let launched = null;

if (!endpoint && process.env.TABENGINE_DESCRIPTOR) {
  try {
    const descriptor = JSON.parse(
      readFileSync(process.env.TABENGINE_DESCRIPTOR, "utf-8"));
    endpoint = descriptor.endpoint || "";
  } catch (error) {
    fail("cannot read descriptor: " + error.message);
  }
}
if (!token && process.env.TABENGINE_TOKEN_FILE) {
  try {
    token = readFileSync(process.env.TABENGINE_TOKEN_FILE, "utf-8").trim();
  } catch (error) {
    fail("cannot read token file: " + error.message);
  }
}
if (!endpoint && process.env.TABENGINE_LAUNCH) {
  const descriptorPath = process.env.TABENGINE_LAUNCH_DESCRIPTOR;
  if (!descriptorPath) fail("launch mode needs TABENGINE_LAUNCH_DESCRIPTOR");
  launched = spawn(process.env.TABENGINE_LAUNCH, {
    shell: true,
    stdio: ["ignore", "inherit", "inherit"],
    env: process.env,
  });
  launched.on("exit", (code) => {
    if (code !== null && code !== 0) {
      console.error(`tabengine-mcp: launched host exited with ${code}`);
    }
  });
  const deadline = Date.now() + 20000;
  while (!existsSync(descriptorPath) && Date.now() < deadline) {
    await delay(100);
  }
  if (!existsSync(descriptorPath)) fail("launched host wrote no descriptor");
  try {
    const descriptor = JSON.parse(readFileSync(descriptorPath, "utf-8"));
    endpoint = descriptor.endpoint || "";
  } catch (error) {
    fail("cannot read launched descriptor: " + error.message);
  }
}
if (!endpoint) {
  fail("no connection: set TABENGINE_ENDPOINT+TOKEN, TABENGINE_DESCRIPTOR, " +
       "or TABENGINE_LAUNCH+TABENGINE_LAUNCH_DESCRIPTOR");
}
if (!token) fail("no token: set TABENGINE_TOKEN or TABENGINE_TOKEN_FILE");
endpoint = endpoint.replace(/\/+$/, "");

let nextRequestId = 1;

async function rpc(method, params) {
  const response = await fetch(endpoint + PROTOCOL_PREFIX + "/rpc", {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      "Authorization": "Bearer " + token,
    },
    body: JSON.stringify({
      jsonrpc: "2.0",
      id: String(nextRequestId++),
      method,
      params: params || {},
    }),
    signal: AbortSignal.timeout(30000),
  });
  if (!response.ok) throw new Error(`HTTP ${response.status}`);
  const envelope = await response.json();
  if (envelope.error) {
    throw new Error(`${envelope.error.message} [code ${envelope.error.code}]`);
  }
  return envelope.result;
}

async function artifactBytes(artifactId) {
  const response = await fetch(
    endpoint + PROTOCOL_PREFIX + "/artifacts/" + encodeURIComponent(artifactId), {
      headers: { "Authorization": "Bearer " + token },
      signal: AbortSignal.timeout(30000),
    });
  if (!response.ok) throw new Error(`artifact HTTP ${response.status}`);
  return Buffer.from(await response.arrayBuffer());
}

// Method registry (mutating flags drive the permission filter).
let methodRegistry = null;

async function registry() {
  if (!methodRegistry) {
    const schema = await rpc("System.getSchema");
    methodRegistry = new Map();
    for (const method of schema.methods || []) {
      methodRegistry.set(method.name, method);
    }
  }
  return methodRegistry;
}

function text(value) {
  return { content: [{ type: "text", text: typeof value === "string" ? value : JSON.stringify(value, null, 2) }] };
}

function refused(message) {
  return { content: [{ type: "text", text: "REFUSED: " + message }], isError: true };
}

const server = new McpServer({ name: "tabengine-mcp", version: "0.1.0" });

server.registerTool("get_info", {
  title: "Instance info",
  description: "Identity, versions and capabilities of the attached instance.",
}, async () => text(await rpc("System.getInfo")));

server.registerTool("list_methods", {
  title: "Method catalogue",
  description: "Every automation method with its grants, mutating flag and " +
    "parameter schema (engine domains and application domains).",
}, async () => text([...(await registry()).values()]));

server.registerTool("call_method", {
  title: "Call an automation method",
  description: "Invoke one JSON-RPC method by name. Mutating methods need " +
    "TABENGINE_ALLOW_MUTATIONS=1 on the adapter.",
  inputSchema: {
    method: z.string().describe("Method name from list_methods"),
    params: z.record(z.string(), z.unknown()).optional().describe("Method parameters"),
  },
}, async ({ method: name, params }) => {
  const catalogue = await registry();
  const descriptor = catalogue.get(name);
  if (!descriptor) return refused(`unknown method '${name}'`);
  if (descriptor.mutating && !allowMutations) {
    return refused(`'${name}' is mutating; set TABENGINE_ALLOW_MUTATIONS=1 to enable it`);
  }
  return text(await rpc(name, params));
});

server.registerTool("snapshot", {
  title: "Semantic snapshot",
  description: "Merged semantic tree of one window (engine chrome plus " +
    "application provider nodes with stable business keys).",
  inputSchema: {
    windowId: z.string().describe("Window (decimal id string)"),
    maxNodes: z.number().int().positive().optional(),
  },
}, async ({ windowId, maxNodes }) => {
  const params = { windowId };
  if (maxNodes) params.maxNodes = maxNodes;
  return text(await rpc("UI.snapshot", params));
});

server.registerTool("click", {
  title: "Click",
  description: "Click at client coordinates or on a stable business key. " +
    "Mutating; needs TABENGINE_ALLOW_MUTATIONS=1.",
  inputSchema: {
    windowId: z.string(),
    x: z.number().optional(),
    y: z.number().optional(),
    stableKey: z.string().optional(),
    count: z.number().int().min(1).max(2).optional(),
  },
}, async (args) => {
  if (!allowMutations) return refused("click is mutating; set TABENGINE_ALLOW_MUTATIONS=1");
  const params = { windowId: args.windowId };
  if (args.stableKey) {
    params.stableKey = args.stableKey;
  } else {
    params.x = args.x;
    params.y = args.y;
  }
  if (args.count) params.count = args.count;
  return text(await rpc("Input.click", params));
});

server.registerTool("type_text", {
  title: "Type text",
  description: "Deliver text input to a window. Mutating; needs " +
    "TABENGINE_ALLOW_MUTATIONS=1.",
  inputSchema: {
    windowId: z.string(),
    text: z.string(),
  },
}, async ({ windowId, text: value }) => {
  if (!allowMutations) return refused("type_text is mutating; set TABENGINE_ALLOW_MUTATIONS=1");
  return text(await rpc("Input.typeText", { windowId, text: value }));
});

server.registerTool("screenshot", {
  title: "Screenshot",
  description: "Capture the window's current frame as a PNG image.",
  inputSchema: {
    windowId: z.string().optional().describe("Defaults to the first window"),
  },
}, async ({ windowId }) => {
  let target = windowId;
  if (!target) {
    const windows = await rpc("Window.list", {});
    const first = (windows.windows || [])[0];
    if (!first) return refused("no windows to capture");
    target = String(first.windowId ?? first.id ?? "");
    if (!target) return refused("cannot determine the first window id");
  }
  const result = await rpc("Capture.screenshot", { windowId: target });
  const artifactId = result.artifactId;
  if (!artifactId) return refused("capture returned no artifact");
  const bytes = await artifactBytes(artifactId);
  return {
    content: [
      { type: "text", text: `Captured ${bytes.length} bytes (artifact ${artifactId}).` },
      { type: "image", data: bytes.toString("base64"), mimeType: "image/png" },
    ],
  };
});

server.registerTool("wait", {
  title: "Wait for a condition",
  description: "Non-blocking engine wait: activityQuiet over a registered " +
    "activity scope, nodeAppears, or operationDone.",
  inputSchema: {
    condition: z.enum(["activityQuiet", "nodeAppears", "operationDone"]),
    scope: z.string().optional().describe("Activity scope for activityQuiet"),
    windowId: z.string().optional(),
    stableKey: z.string().optional().describe("For nodeAppears"),
    operationId: z.string().optional().describe("For operationDone"),
    timeoutS: z.number().positive().optional(),
  },
}, async (args) => {
  const params = { condition: args.condition };
  if (args.scope) params.scope = args.scope;
  if (args.windowId) params.windowId = args.windowId;
  if (args.stableKey) params.stableKey = args.stableKey;
  if (args.operationId) params.operationId = args.operationId;
  if (args.timeoutS) params.timeoutS = args.timeoutS;
  return text(await rpc("Wait.condition", params));
});

server.registerTool("poll_events", {
  title: "Poll events",
  description: "Automation events since a cursor, optionally filtered by window.",
  inputSchema: {
    after: z.number().int().optional().describe("Cursor from a previous poll"),
    windowId: z.string().optional(),
  },
}, async ({ after, windowId }) => {
  const params = {};
  if (after !== undefined) params.after = after;
  if (windowId) params.windowId = windowId;
  return text(await rpc("Event.poll", params));
});

process.on("exit", () => {
  // Explicit-launch cleanup: the host owns its windows; taking it down with
  // the adapter prevents orphaned controlled instances.
  if (launched && !launched.killed) launched.kill();
});

await server.connect(new StdioServerTransport());
console.error(`tabengine-mcp: serving ${endpoint} (mutations ${
  allowMutations ? "allowed" : "refused"})`);
