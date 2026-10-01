# TabEngine Automation guide

TabEngine ships an in-process control runtime plus an optional loopback
HTTP transport that lets external tools drive a running application the
same way a user does: real input through the shell's own dispatch,
semantic snapshots of the merged UI, frame-accurate screenshots, typed
wait conditions, operations and bounded diagnostics.

This guide covers enabling it, the threading and lifetime contracts,
security posture, coordinate and frame semantics, and how an application
extends the engine with its own methods, providers and policy. The
machine-readable protocol lives in `/automation/v1/schema`; the header
reference is in [AUTOMATION_API_REFERENCE.md](AUTOMATION_API_REFERENCE.md).

The runtime is deliberately generic. TabEngine knows windows, tabs, input,
frames and observation primitives; it knows nothing about files, URLs,
terminals, RML or any product concept. Applications register those as
their own `app.<owner>.*` domains and providers.

## What this is not

- It is not a browser automation protocol: no DOM, no JavaScript, no
  network or heap inspection, no second rendering path.
- It is not remote administration: the service is loopback-only and
  token-authenticated; every route is refused without the token.
- It is not a sandbox. "Isolated" modes are host policy (see below), not
  OS-level containment, and granting input means a client can trigger
  whatever the application's own UI can trigger - including file or
  process side effects. Treat the token like a debugger attachment.

## Enabling the transport

Applications compose the runtime from four pieces (all in the SDK):

```cpp
#include "tabengine/automation/runtime.h"
#include "tabengine/automation/shell_bridge.h"
#include "tabengine/automation/transport.h"

// 1. The runtime: registries, queues, events, operations, artifacts.
automation::AutomationRuntime runtime(
    {},                                    // RuntimeBudgets (defaults are the v1 limits)
    {"1.0", "0.1.0", "my_app", "1.0",      // protocol, engine, app, app version
     "build-123", "instance-1"},           // sdk build id, instance id ("" generates one)
    [&platform] { return platform->monotonic_seconds(); });

// 2. The bridge binds one Shell and registers Window/Tab/Input/UI/Capture/Trace.
automation::ShellAutomationBridge bridge({shell, *platform, *renderer, runtime});
bridge.set_renderer_backend(raster ? "raster" : "d3d12");

// 3. Application extension: methods, providers, policy (see below).

// 4. Wakes: the pump is the single owner of the platform wake handler.
automation::UiTaskPump pump;
pump.bind_platform(*platform);
runtime.set_wake([&platform] { platform->wake(); });
runtime.set_deferred_wake([&platform](double delay) {
    platform->request_delayed_wake(delay);   // bounded wait polling
});
auto subscription = pump.subscribe([&] {
    runtime.drain(platform->monotonic_seconds());   // UI thread, budgeted
});

// 5. The optional transport (opt-in, never started by the engine itself).
automation::TransportConfig config;
config.token = read_secret_from_env_or_file();   // required; empty refuses to start
automation::AutomationTransport transport(runtime, config);
if (transport.start()) {
    publish_connection_descriptor(transport.describe());  // endpoint, pid, instance id
}
```

`runtime.drain()` runs on the UI thread and executes queued requests under
a time budget. Everything product-visible happens on that thread, which is
why the contract below matters.

## Threading contract

| Surface | Thread |
| --- | --- |
| `drain`, `shutdown`, registration, providers, policy | UI thread |
| `receive`, `connection_closed`, response sink, `set_wake` | any thread |
| `emit_event`, `start_operation`, `finish_operation`, `log` | UI thread |
| `ArtifactStore` | internally synchronized |
| Provider callbacks (`provide_semantic`, `activity`, `read_logs`) | UI thread |
| Completion sinks handed to handlers | any thread, exactly once |

A handler either settles its request synchronously or defers; a deferred
completion arriving after the runtime, the provider, or the connection is
gone is dropped. Queue limits, deadlines and cancellation are enforced by
the runtime, not by handlers: a request whose deadline passes **before it
starts** is answered `DeadlineExceeded` and is _never executed_.

## Protocol profile

- `POST /automation/v1/rpc` - one JSON-RPC 2.0 request per HTTP request,
  single-request profile: no batches, no notifications, `id` required
  (string or integer). Responses are `result`/`error` exclusive with the
  id echoed back. The connection closes after its one response.
- `GET /automation/v1/schema` - the schema document: methods with JSON
  Schema parameter/result contracts, grants, timeouts, events, error
  catalogue, budgets, activity scopes.
- `GET /automation/v1/artifacts/<id>` - artifact bytes with an ETag; 404
  once expired. The route is authorized through a real `Artifact.get`
  request: the session policy, the `engine.observe` grant and the TTL are
  enforced exactly as for the RPC route, so artifacts never leak past a
  denied or ungranted session.
- `POST /automation/v1/session` - begin a logical session; returns
  `{"sessionId": "..."}`.
- `DELETE /automation/v1/session` - end the session named by the
  `X-TabEngine-Session` header.

### Logical sessions and input lifetime

Every HTTP request without a session header is its own **ephemeral
connection**: the transport closes it right after the single response,
which cancels anything still queued for it and releases whatever input
that request left held (buttons, keys, IME) on the next UI drain. Atomic
actions (`Input.click`, `Input.drag`, `Input.typeText`) work fine that
way. Raw, multi-request gestures do not - each request would release the
previous one's state.

For those, `POST /automation/v1/session` once, then send the returned id
as the `X-TabEngine-Session` header on every request of the gesture. All
such requests share one connection identity, so `Input.pointer`
down/move/up, `Input.key` down/up and multi-segment `Input.ime`
sequences pair correctly, and `Input.releaseAll` releases the right
session. Sessions are bound to the Bearer token (authentication runs
before the header is consulted), expire after a configurable idle
timeout (`session_idle_timeout_s`, swept by the accept loop, not by
client traffic) and are capped by `max_sessions` (429 beyond the cap).
Ending a session - explicitly, by idle expiry, or when the transport
stops - cancels its in-flight requests and releases its held input.
An unknown or expired session id answers 404.

### Transport deadlines and cancellation

The transport's `response_timeout_s` is a hard cap on the whole exchange:
it bounds the HTTP wait **and** the runtime execution deadline of the
request. When the wait expires the transport answers 503 and cancels the
runtime request, so a queued mutation never executes after its client has
already seen a failure. Cancellation is precise but cooperative once a
handler started: work that already began may still finish (side effects
are not rolled back) - operations remain queryable through
`Operation.get`/`Operation.wait` for exactly that reason.

All identifiers on the wire (`windowId`, `tabId`, `contentId`, node refs,
sequence numbers, operation ids) are **decimal strings** so JavaScript
clients never lose 64-bit precision. Unknown request members, malformed
framing, chunked bodies and duplicate `Content-Length` are rejected before
dispatch. Standard JSON-RPC error codes cover transport/profile problems
(`-32700` parse, `-32600` invalid request, `-32601` method not found,
`-32602` invalid params, `-32603` internal); business errors use the
positive space documented in the schema: `Unsupported`, `PermissionDenied`,
`TargetNotFound`, `StaleNode`, `NotInteractable`, `AmbiguousTarget`,
`DeadlineExceeded`, `Cancelled`, `ProviderUnavailable`, `Overloaded`,
`SurfaceUnavailable`, `DeviceLost`, `OperationNotFound`,
`InvalidOperationState`, `BudgetExceeded`, `InvalidArgument`,
`SessionConflict`, `InvalidSession`.

## Security posture

- Loopback only, always. `Host` and `Origin` headers must resolve to
  loopback literals; anything else is refused before parsing.
- Bearer token on every route: `Authorization: Bearer <token>`. The token
  is delivered by the host through a controlled environment variable or a
  user-only secret file - it is never part of the connection descriptor,
  the schema, or the logs.
- Auth failures are rate-limited with a short lockout; every refusal is
  counted in `AutomationTransport::stats()`.
- No CORS: no `Access-Control-Allow-Origin`, ever. A browser page cannot
  be talked into reaching the service; tools proxy through their own
  process instead.
- Budgets are host-lowered, client-proof: request body, queue depth,
  per-session in-flight requests, event ring, poll batch, screenshot
  pixels, in-flight captures, artifact bytes, trace duration. Exceeding
  them yields `BudgetExceeded`/`Overloaded`.
- Sensitive content stays out: the engine logs methods/targets/status and
  never request payloads; artifact bytes and logs are the application's
  responsibility to sanitize.

### Isolation is host policy

There is no engine-level sandbox. A host that wants isolated test runs
installs an `IAutomationPolicy` that denies or narrows grants and refuses
methods, and gates its own services (file operations, process launch,
clipboard) behind the same policy. Note that granting `engine.input`
already allows a client to click product UI - policy authors must treat
input as a potent grant.

## Grants

Sessions receive grant names from the host policy (defaults:
`engine.observe`, `engine.input`, `engine.windowTab`). Every method
declares the grants it needs; the runtime refuses dispatch without them.
`test.*` domains are application test hooks and should only be granted in
explicitly isolated runs. A connection refused by `authorize_session` gets
`PermissionDenied` on every request; grants are re-evaluated per request
so hosts can tighten or widen a live session.

## Coordinates, targets and input

- Input coordinates are client pixels of the target window by default.
  Snapshots and captures report the same space; DPI scale is reported per
  window (`Window.list`); hiding (`Window.setPlacement` state `hide`)
  keeps the instance resident - surfaces, tabs and input sessions stay
  alive while off-screen.
- The semantic tree merges the engine's chrome (window, tab strip, tabs,
  close buttons, new-tab button, caption buttons/area, body) with every
  registered provider's subtree under the body node. Node refs embed the
  provider's **registration slot** - a stable identity that is never
  reused, so a ref minted for an unregistered provider fails with
  `StaleNode` instead of addressing whatever provider took its place. Refs
  are only meaningful together with the snapshot that produced them.
- `Input.click` accepts raw coordinates, a `node` ref, or a `stableKey`.
  Node clicks resolve through the **authoritative owner** - engine kinds
  through the shell, provider nodes through that provider's
  `resolve_node` - then check visibility/enabled/clip and click inside the
  visible portion. Animation movement is fine, but a removed, rebuilt or
  hidden node reports `StaleNode`/`NotInteractable` instead of clicking
  something else. Passing `snapshotId` with `strict: true` (node or
  stableKey mode, and `Capture.screenshot` node mode) additionally fails
  when the **merged layout revision** changed since the snapshot - that
  revision folds provider-reported revisions and attach/detach into the
  shell's own, so a provider DOM rebuild invalidates pinned refs even
  while the shell itself was idle.
- `Input.drag` synthesizes a full pointer gesture. Inside the tab strip it
  reorders; leaving the strip tears the tab into a stepwise move session
  through the same shell state machine a real mouse uses, so attaching
  over another window's strip, canceling, and floating all behave like the
  product. `Tab.transfer` is the command path and a different test
  surface; it is not a substitute for input verification.
- Keys use logical names (`Enter`, `Escape`, `a`, `F4`, ...) with
  `ctrl`/`shift`/`alt` modifiers; unknown names report `InvalidArgument`.
  `typeText` delivers text as `TextInput` events that never pass the
  shortcut hook.
- Pointer sessions are owned per connection: two clients cannot interleave
  half a gesture on one window (`SessionConflict`). `Input.releaseAll`,
  connection close, session end/expiry and transport stop all release
  buttons, keys and any open IME composition on the UI thread.

## Frames and capture

`Capture.screenshot` copies an independent pixel buffer at a safe frame
boundary, crops to the requested region (or a node's bounds), encodes PNG
and stores it as a bounded artifact with size, SHA-256, TTL and target
metadata. `frame: "fresh"` dispatches a real paint first; `"current"`
captures the last presented frame.

- Raster: the CPU surface holds the presented frame; capture copies it.
- D3D12: the capture waits on the present fence, snapshots the swapchain
  render target through Skia and performs a bounded synchronous GPU
  readback. A D3D12 window is either answered with real D3D12 pixels or
  fails with `SurfaceUnavailable` - it is never silently served raster
  pixels.
- Whether capture exists at all is `IRenderer::supports_capture()`;
  hidden windows can capture after a `fresh` paint on both backends;
  device loss reports `DeviceLost` and the renderer's raster fallback
  continues rendering (a subsequent capture reports `raster` truthfully).

System-composited surfaces (native title bar drop shadow, OLE drag
images, native menus, IME candidate windows) are never part of a
client-area capture.

## Waiting, operations and events

- `Wait.condition` stores a typed predicate and is evaluated on UI drains
  and registered notifications - it never sleeps, blocks the UI, or fakes
  a result. Conditions: `activityQuiet` (named scope has zero finite
  tasks), `nodeAppears`, `operationDone`, `animationSettled`,
  `frameStable`. Unknown scopes report `Unsupported`; watchers and
  long-lived background work must not be counted as pending, or waits
  would never settle. An `activityQuiet` wait binds to the providers that
  knew the scope when it started: if one of them unregisters (or stops
  recognizing the scope), the wait fails with `ProviderUnavailable` -
  "no observation" never proves "no work". Providers that join later
  contribute their pending counts, so new work can delay a wait but never
  fake quiet.
- Long work returns an `operationId`; `Operation.get/wait/cancel` query
  it. `Operation.wait` reports the operation's full terminal state -
  including `state: "failed"` with its error, or `state: "completed"`
  with its result - identically whether the wait started before or after
  the outcome; a failed operation never hides behind a satisfied wait.
  Cancellation is cooperative and never pretends to roll back side
  effects that already ran.
- `Event.poll` returns sequence-numbered events with explicit gap
  reporting (`gap`, `oldestAvailable`): after a gap, re-synchronize from a
  fresh snapshot instead of trusting the list.
- `Trace.start/stop/status` records bounded spans (input dispatch,
  presented frames, capture, and application spans emitted through the
  runtime) with request/window correlation.

## Extending the engine

### Methods

```cpp
std::vector<automation::MethodDescriptor> methods;
automation::MethodDescriptor descriptor;
descriptor.name = "app.myapp.openRecent";       // app.<owner>.<action>
descriptor.summary = "Open the Nth recent file.";
descriptor.mutating = true;
descriptor.params = schema_object({                     // subset of JSON Schema
    {"index", integer_property("Zero-based entry", 0)},
}, {"index"});
descriptor.result = object_schema({});
methods.push_back(descriptor);
auto registration = runtime.register_methods(
    "app.myapp", std::move(methods),
    [](const automation::RequestContext& context, automation::CompletionSink sink) {
        // Runs on the UI thread; parse context.params strictly, call real
        // product services, settle with success or an Error.
        sink(automation::MethodOutcome::success(result));
    });
// Keep `registration` alive; destroying it unregisters the domain, and
// queued requests then fail ProviderUnavailable.
```

Engine domains (`System`, `Window`, `Tab`, `Input`, `UI`, `Capture`,
`Event`, `Operation`, `Wait`, `Log`, `Trace`, `Artifact`) cannot be
overridden by applications; `app.`/`test.` prefixes are enforced.

### Providers

- `ISemanticProvider`: contribute your body subtree (rows, editors,
  menus) with stable business keys, current bounds and hit consistency,
  and resolve your own node refs; return `false` from `resolve_node` for
  stale refs.
- `IActivityProvider`: report finite pending work per named scope so waits
  can settle; return `false` for unknown scopes.
- `IDiagnosticProvider`: paged logs plus an optional snapshot object for
  support tooling.
- Localization: `SemanticNode::name` is display text; `stable_key` is the
  identity. Never key automation off translated text.

### Policy

```cpp
class MyPolicy final : public automation::IAutomationPolicy {
    bool authorize_session(const automation::SessionInfo& session) override;
    std::vector<std::string> grants(const automation::SessionInfo& session) override;
    bool allow_method(const automation::SessionInfo& session,
                      const automation::MethodDescriptor& method) override;
};
runtime.set_policy(&policy);
```

## Client tools

- **Python CLI** (`tools/automation/te.py` in every SDK package, standard
  library only): `info`, `windows`, `tabs`, `snapshot`, `click`, `key`,
  `type`, `wheel`, `drag`, `screenshot --out shot.png`, `wait`, `op-*`,
  `logs`, `events`, `trace-*`, `session begin|end`, and a raw `call`
  escape hatch. Connection comes from `--endpoint`/`--descriptor` plus
  `--token`/`--token-file`/`TABENGINE_TOKEN`; `--session <id>` routes raw
  input gestures through one logical session; failures exit non-zero with
  the RPC error on stderr.
- The packaged `examples/automation_demo` shows the whole host wiring and
  supports `--self-check`, which proves the packaged binary answers real
  protocol calls end to end.
- **Web devtools panel** (`tools/devtools` in the SDK package): plain
  HTML/CSS/JS, no web framework. Mount it before `transport.start()`:

  ```cpp
  tabengine::automation::DevtoolsPanel panel;
  tabengine::automation::DevtoolsPanel::Options options;
  options.resources_root = "<sdk>/tools/devtools";
  (void)panel.mount(transport, options);
  ```

  then open `<endpoint>/automation/v1/panel` in a browser. The static
  shell is served without a token (it contains no secrets and never
  leaves the loopback boundary); the page asks for the token, keeps it in
  memory only, and performs every data/action request through the
  authenticated routes. The panel covers connection status, the window /
  tab / node tree with per-node details and bounds-on-screenshot
  overlay, capture with DPI-correct click mapping, controlled key/wheel/
  text input behind an explicit write toggle, the schema-driven method
  catalog with structured results and operation tracking plus
  cooperative cancel, and bounded event/log polling with cursor-gap
  reporting and a redacted diagnostics download. `automation_demo
  --devtools-root tools/devtools` mounts the same panel.

## Testing your integration

- Drive the runtime directly in unit tests: build an `AutomationRuntime`
  with a fake clock, call `receive` with JSON strings, pump `drain`, and
  assert on the response sink. No sockets, no sleeps.
- For transport tests, bind port 0 and talk to the real loopback service;
  assert on HTTP status for auth/framing and on JSON-RPC codes for the
  rest.
- Keep a "product belief" test: after any input, assert the visible
  product state (listing, selection, title) changed, not just that the RPC
  returned success.
