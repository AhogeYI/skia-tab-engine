# TabEngine MCP adapter

Exposes a running TabEngine automation surface (the engine's own host or a
product embedding it) as [Model Context Protocol](https://modelcontextprotocol.io)
tools over stdio, for AI clients that speak MCP.

From a TabEngine SDK package the adapter is ready to run - production
dependencies are vendored inside `tools/mcp/node_modules`, no install step:

```powershell
$env:TABENGINE_ENDPOINT = "http://127.0.0.1:53123"
$env:TABENGINE_TOKEN = "<token>"
node <sdk>\tools\mcp\server.mjs
```

From a source checkout, install the pinned dependencies first:

```powershell
npm ci                                   # pinned SDK, lockfile committed
node tools/mcp/server.mjs
```

## Connection modes

| Mode | Environment | Notes |
| ---- | ----------- | ----- |
| Attach | `TABENGINE_ENDPOINT` + `TABENGINE_TOKEN` | Direct HTTP to a running instance. |
| Descriptor | `TABENGINE_DESCRIPTOR` (file) + `TABENGINE_TOKEN`/`TABENGINE_TOKEN_FILE` | Reads the endpoint from the host's connection descriptor; the token never travels in the file next to it unless you choose `TABENGINE_TOKEN_FILE`. |
| Explicit launch | `TABENGINE_LAUNCH` (command) + `TABENGINE_LAUNCH_DESCRIPTOR` (file the command writes) [+ `TABENGINE_TOKEN_FILE`] | The adapter spawns the host, waits for the descriptor, and terminates it with itself. |

For direct executable launch, set `TABENGINE_LAUNCH_ARGS` to a JSON array
of strings (including `[]` for no arguments). Without it, the command runs
through a shell. Use a fresh descriptor path; a leftover descriptor is
rejected. The host must publish the token before the descriptor. Host stdout
is discarded to preserve the MCP stdio stream. On Windows cleanup terminates
the owned process tree; shell commands must keep their host in the foreground.

Input tools share a logical session, including `call_method` for `Input.*`.
Normal RPC traffic refreshes its lease. After idle expiry a call reports an
error without replaying input; start a new gesture on the next call. Closing
the adapter explicitly releases the session and any held input.

## Tools

- `get_info` - instance identity, versions, capabilities.
- `list_methods` - the full method catalogue (engine + application domains)
  with grants and mutating flags.
- `call_method` - invoke any listed method; mutating methods are refused
  unless the adapter runs with `TABENGINE_ALLOW_MUTATIONS=1`.
- `snapshot` - merged semantic tree of a window.
- `click`, `type_text` - input synthesis (mutating, gated as above).
- `screenshot` - captures the current frame and returns it as PNG image
  content (the artifact bytes never hit a URL with the token in it).
- `wait` - `activityQuiet` / `nodeAppears` / `operationDone` conditions.
- `poll_events` - automation event stream with cursors.

## Safety

- Mutating tools are off unless `TABENGINE_ALLOW_MUTATIONS=1`.
- The token is only used in the `Authorization` header; it never appears in
  tool output, logs, or resource URIs.
- The adapter talks to the loopback transport only; it does not open the
  engine to other networks.

## Self-check

`npm test` runs window-free protocol regressions for event cursors, dynamic
method permissions, input session ownership, and launch discovery/cleanup.

With a live instance (for example a headless product or the automation
demo), run the handshake and tool smoke checks:

```powershell
$env:TABENGINE_ENDPOINT = "http://127.0.0.1:PORT"
$env:TABENGINE_TOKEN = "<token>"
npm run selfcheck                        # add TABENGINE_ALLOW_MUTATIONS=1 to verify clicks too
```
