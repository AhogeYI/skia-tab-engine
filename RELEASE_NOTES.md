# Release notes

## 0.3.1 (local development)

Protocol version stays `1.0`. The SDK version moves because the platform
contract grew a documented behavior: hardware media keys are now delivered
to clients as ordinary key events. This version is unpublished until the
maintainer publishes its release assets.

### Distribution checks

- SDK packages include the nlohmann/json MIT license and an inventory of
  bundled MCP dependency notices. Verification rejects incomplete notices.
- `tools/sdk.py --publication` requires a package from the current clean
  `main` commit; ordinary local development packages remain supported.
- Packaged-tool verification includes MCP regressions as well as the live
  demo handshake and panel logic/DOM tests.

### Media keys

- The Win32 backend translates the `WM_APPCOMMAND` playback subset
  (play/pause, stop, next track, previous track) to their standard
  `VK_MEDIA_*` virtual keys and emits `KeyDown` together with a
  synthesized `KeyUp`, since remote buttons carry no keyboard state. The
  handled command returns `TRUE` so it is not forwarded again; other app
  commands keep their default processing.
- Clients see no new event types: playback keys arrive as regular key
  events and ride the existing shortcut and body-event dispatch.

## 0.3.0

Protocol version stays `1.0`. The SDK version moves because the platform
contract grew: embedders can now delegate the Windows UI Automation root
object. Consumers recompile against the new package - there is no binary
ABI promise within a version line.

This release also restores a unique version-to-contract identity: 0.2.0
was never published, and two locally packaged 0.2.0 archives with
different headers circulated during development. Any 0.2.0 archive found
in the wild must be treated as an untracked development build; the
supported public contracts are 0.1.0 and 0.3.0.

### UI Automation root delegation

- `tabengine::IPlatform::set_uia_root_provider(WindowId, void*)` is a new
  optional virtual (default-absent). The Win32 backend forwards
  `WM_GETOBJECT` for `UiaRootObjectId` through
  `UiaReturnRawElementProvider` to the embedder's
  `IRawElementProviderSimple`, and clears the delegation when the window
  or platform is destroyed. Embedders answer `WM_GETOBJECT` only through
  this hook - no subclassing of engine-owned windows.
- `Shell::body_bounds(WindowId)` is now public: side-frame hosts need the
  body rectangle to map client-relative control geometry to screen
  coordinates for UI Automation providers.
### Web devtools panel

- The transport serves an unauthenticated static entry for the panel shell
  (GET only, still behind the loopback/Origin boundary; the shell carries no
  secrets) while every data and action request keeps using the authenticated
  routes. `DevtoolsPanel::mount` wires `tools/devtools` from the SDK onto
  `<endpoint>/automation/v1/panel`.
- The panel is dependency-free HTML/CSS/JS: connection status, window/tab/
  node inspection with details and bounds overlay on the screenshot,
  capture with DPI-correct click mapping, controlled input behind an
  explicit write toggle, the schema-driven method catalog with structured
  results, operation tracking with cooperative cancel, and bounded
  event/log polling with a redacted diagnostics download. The token lives
  in page memory only.
- Browser `Origin` headers (full URL form) are now accepted for loopback
  origins; foreign origins stay refused.

### Tools ship with the SDK

- `tools/devtools` (panel) and `tools/mcp` (adapter, production
  dependencies vendored from the committed lockfile) are packaged with the
  SDK and declared in `manifest.json` under `tools` with versions and entry
  points. SDK `verify` now covers both: panel logic regression, adapter
  load from a spaced directory, and a live MCP selfcheck against the
  packaged demo.
- The MCP adapter is versioned 0.3.0 to match.

## 0.2.0

Protocol version stays `1.0`; the SDK version moves because the public
surface grew. Consumers recompile against the new package - there is no
binary ABI promise within a version line.

### Logical sessions and input lifetime

- `POST /automation/v1/session` begins a logical session; sending its id
  as the `X-TabEngine-Session` header binds later HTTP requests to one
  stable connection identity, so raw multi-request gestures
  (`Input.pointer` down/move/up, `Input.key` down/up, multi-segment
  `Input.ime`) pair correctly. Sessions are bound to the bearer token,
  idle out after `session_idle_timeout_s` (swept by the service, not by
  client traffic) and are capped by `max_sessions`.
- Requests without the header are ephemeral: their connection closes
  right after the single response, cancelling anything still queued and
  releasing whatever input that request left held (buttons, keys, IME)
  on the next UI drain. `Input.releaseAll` now releases every held
  button, not just the first.
- `AutomationRuntime::add_connection_observer` lets embedders observe
  connection ends on the UI thread (the shell bridge uses it for input
  release).

### Deadlines and cancellation

- `AutomationRuntime::receive_with_deadline(connection, request, payload,
  response_deadline_s)` caps the execution deadline with the caller's
  wait and returns a `RequestCancellation` handle. The transport's 503
  now cancels the runtime request, so a queued mutation never executes
  after its client has already seen a failure; started work stays
  queryable through its operation.
- All transport socket I/O runs against absolute deadlines in
  stop-checked slices: a trickling client cannot extend its budget, and
  `stop()` joins within a bounded delay without cross-thread closes.

### Authorization and artifacts

- `GET /automation/v1/artifacts/<id>` is served through a real
  `Artifact.get` request: session policy, the `engine.observe` grant and
  the TTL apply exactly as on the RPC route.
- Artifact reads enforce the TTL on the read path with the runtime's
  clock (idle artifacts expire without a drain); metadata reports
  `expiresAt`.

### Node identity and strict clicks

- Provider registration slots are stable and never reused; node refs
  minted for an unregistered provider fail with `StaleNode` instead of
  addressing its successor.
- `Input.click`, `Capture.screenshot` (node mode) and `UI.resolveNode`
  resolve through the authoritative owner (the provider's
  `resolve_node`); `snapshotId` + `strict` compares against the merged
  layout revision, which folds provider-reported revisions and
  attach/detach - and now also covers stableKey clicks. Clicks land
  inside the visible clip.
- `UI.snapshot` reports `documentGeneration`; snapshot assembly is
  linear in the tab count.

### Waits, operations and lifecycle

- `Operation.wait` reports the operation's full terminal state
  (`state`, `result`/`error`, `waitedS`) identically whether the wait
  started before or after the outcome.
- `activityQuiet` binds to the activity providers that knew the scope:
  losing one fails the wait with `ProviderUnavailable` instead of
  reading absence as quiet.
- Environment hooks install through `AutomationRuntime::
  install_environment_hooks`; destroying the registration revokes them
  and fails dependent waits with `Unsupported` (a destroyed bridge
  leaves nothing callable behind).
- Synthetic double-click releases carry no click count, matching the
  native messages.
- `Window.setPlacement` gains `hide`/`show`; hiding keeps the instance
  resident (surfaces, tabs and input sessions stay alive). The Win32
  `set_client_bounds` now places custom-frame windows exactly on the
  requested client rectangle.
- `tools/automation/te.py` grows `session begin|end` and `--session`.

## 0.1.0

Initial public release: embeddable tab strip with the automation runtime,
shell bridge (Window/Tab/Input/UI/Capture/Trace), optional loopback HTTP
transport, raster and D3D12 capture, SDK packaging and the Python CLI.
