# Release notes

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
