# TabEngine Automation API reference

Header-by-header contract for `include/tabengine/automation/`. Read
[AUTOMATION_GUIDE.md](AUTOMATION_GUIDE.md) first for the operating model;
this document is the per-type reference.

Conventions: everything lives in `tabengine::automation`. All values on
the wire are JSON with decimal-string identifiers. Types with a move-only
registration handle must outlive their registration.

## values.h - the protocol value model

- `JsonValue`: self-contained JSON value (null/bool/int64/double/string/
  array/object). Typed accessors never throw; `find(key)` returns `nullptr`
  for missing members; `set` preserves member order; `serialize()` emits
  compact round-trip-safe JSON. No third-party JSON type appears in any
  public header.
- `parse_json_strict(text, max_bytes, out)`: one complete document; rejects
  duplicate object keys, nesting past 64, integers outside int64,
  non-finite/overflowing floats, invalid UTF-8 (including lone surrogates),
  trailing bytes and documents past the byte cap. Returns a status, the
  failure offset and a detail string.
- `id_to_wire` / `id_from_wire`: strict decimal uint64 conversions used for
  every identifier on the wire.
- `rect_to_wire` / `rect_from_wire`: `{x, y, width, height}` integer
  rectangles in client pixels.

## errors.h - error contract

- `ErrorCode`: JSON-RPC reserved negatives plus the positive business
  space listed in the guide; `error_type_name` maps each code to its
  stable wire name.
- `Error`: `{code, message, data}`; `data` always carries a `type`, and
  callers may add target/request/operation correlation and recovery hints.
  Factories: `Error::make`, `unsupported`, `invalid_params`,
  `target_not_found`, `stale_node`.
- Messages are operator-readable English; stacks, pointers and raw
  exception text never cross the boundary.

## method.h - methods and registration

- `kGrantObserve`, `kGrantInput`, `kGrantWindowTab`, `kGrantTestHooks`:
  engine grant names.
- `is_reserved_domain` / `is_valid_app_domain`: domain policy helpers.
- `MethodDescriptor`: `{name, summary, grants, mutating, idempotent,
  timeout_s, params, result}`. `params`/`result` are schemas in the subset
  documented in `schema.h`; the same declaration validates dispatch and is
  published by `System.getSchema`.
- `MethodOutcome`: settled result or `Error`; `success`/`failure`
  factories.
- `CompletionSink`: `std::function<void(MethodOutcome)>`, exactly once,
  thread-safe; late calls after settlement/teardown are dropped.
- `RequestContext`: `{request_id, method, params, connection, slot,
  submitted, deadline, operation_id, cancel_flag}` - no socket, window or
  platform types. `cancel_flag` is cooperative.
- `MethodHandler`: `void(const RequestContext&, CompletionSink)`, UI
  thread; settle inline or defer.
- `MethodRegistration`: move-only RAII registration; `valid()`, `token()`.
  Destruction unregisters the domain: queued requests fail
  `ProviderUnavailable` and later completions are dropped.

## schema.h - schema subset

`validate_against_schema(instance, schema, path)` implements the v1
vocabulary: `type` (`object`, `array`, `string`, `integer`, `number`,
`boolean`, `null`, `any`), per-property `required`, `items`, `enum`,
`minimum`/`maximum`, `minLength`/`maxLength`, `minItems`/`maxItems`,
`description`. Objects reject undeclared properties; failures are
path-qualified (`params.windowId: expected string`).

## semantic.h - semantic tree

- `roles::*`: engine role names (`window`, `tabStrip`, `tab`,
  `tabCloseButton`, `newTabButton`, `captionButton`, `captionArea`,
  `body`, `button`, `text`, `edit`, `listItem`, `pane`, `custom`).
- `SemanticNode`: `{role, name, stable_key, node, parent, window, content,
  bounds, clip, visible, enabled, focused, selected, actions, details}`.
  `name` is display text (localizable); `stable_key` is the business
  identity; `node` is a provider-slot-qualified ref; an empty `clip` means
  fully clipped.
- `SemanticSnapshot`: `{window, revision, layout_revision,
  document_generation, captured_at, truncated, total_nodes, nodes}` -
  depth-first nodes with `parent = 0` for roots.
- `ISemanticProvider`: `provide_semantic(window, out)` appends this
  provider's subtree (UI thread, consistent with the app's own hit
  testing); `resolve_node(window, node, resolved)` re-resolves refs and
  returns `false` for stale ones.
- `ProviderRegistration`: move-only RAII handle for the semantic/activity/
  diagnostics provider slots.

## activity.h - finite work

- `ActivityReport`: `{generation, pending, describe}`.
- `IActivityProvider`: `activity(scope, out)` (UI thread; `false` =
  unknown scope), optional `await_quiet(scope, generation, callback)` for
  push-style completion notification, and `scopes()` for the schema.
  Long-lived watchers must never be reported as pending.

## diagnostics.h - logs and snapshots

- `LogRecord`: `{sequence, at, level, source, message, request_id,
  operation}`; the sequence is the caller's read cursor.
- `IDiagnosticProvider`: `read_logs(after_sequence, max, out)` returns
  records with strictly greater sequence in ascending order; optional
  `diagnostic_snapshot()` returns a structured object merged into
  `Log.read`'s response. Providers keep secrets out themselves.

## policy.h - connection authorization

- `SessionInfo`: `{connection, origin, user_agent, loopback}`.
- `IAutomationPolicy`: `authorize_session` (once per connection),
  `grants` (re-evaluated per request), `allow_method` (final per-method
  admission). The policy sees descriptors and grants only. Never call the
  runtime from inside policy callbacks.

## events.h - observation events

- Names in `event_names::*`: window added/removed/activated/deactivated/
  boundsChanged, tab added/removed/moved/selected/updated/transferred,
  input.dispatched, animation.settled, frame.presented (reserved),
  capture.completed, operation.updated. Applications add
  `app.<owner>.<event>` names.
- `AutomationEvent`: `{sequence, name, window, tab, content, generation,
  request_id, operation, at, details}` - sequence is per-instance,
  monotonic, gap-free until trimmed.

## artifacts.h - bounded binary store

- `ArtifactStore`: `store(bytes, media_type, target, now)` returns an id
  (empty when the payload alone exceeds the budget); `get`/`metadata`
  read entries; `expire(now)` drops TTL-expired entries; eviction is
  oldest-first at the byte budget. Entries carry size, SHA-256, media
  type, TTL and target metadata.
- `sha256_hex`: lowercase hex digest helper.

## runtime.h - the runtime

- `RuntimeBudgets`: host-lowered v1 limits (request bytes, queue, inflight
  per session, request timeout, event items/bytes, poll batch, screenshot
  pixels, in-flight captures, artifact bytes/TTL, trace duration, drain
  budget/batch).
- `InstanceIdentity`: `{protocol_version, engine_version, application,
  app_version, sdk_build, instance_id}` (empty id is generated).
- `OperationState` / `OperationInfo`: running/completed/failed/cancelled
  with method, connection, result/error and timestamps.
- `RuntimeEnvironmentHooks`: engine-side wait predicates
  (`animation_active`, `frame_serial`, `node_exists`) filled by the shell
  bridge; absent hooks make the matching conditions `Unsupported`.
- `UiTaskPump`: `bind_platform(platform)` installs the single platform
  wake handler; `subscribe(drain)` adds a coalesced drain subscriber with
  a move-only RAII `Subscription`. The pump must not outlive its platform.
- `AutomationRuntime`:
  - construction: budgets, identity, monotonic `clock`.
  - registration: `register_methods` (app domains), `register_engine_methods`
    (bridge/tests), `register_semantic_provider`,
    `register_activity_provider`, `register_diagnostics_provider`,
    `set_policy`, `set_environment_hooks`, `set_instance_capabilities`.
  - transport-facing (thread-safe): `set_response_sink`, `receive`,
    `connection_closed`, `set_wake`, `set_deferred_wake`.
  - UI thread: `drain(now)`, `shutdown(now)`, `emit_event`,
    `start_operation`, `finish_operation`, `get_operation`, `log`.
  - queries: `budgets`, `identity`, `now`, `has_method`,
    `artifacts()`, `build_schema_document`, `semantic_providers`,
    `activity_providers`, `instance_capabilities`.
  - engine-internal domains installed automatically: `System.getInfo`,
    `System.getCapabilities`, `System.getSchema`, `Event.poll`,
    `Operation.get/wait/cancel`, `Wait.condition`, `Log.read`,
    `Artifact.get`.

## shell_bridge.h - binding a Shell

- `ShellAutomationBridge::Dependencies`: `{shell, platform, renderer,
  runtime}` - none are owned; construct after the shell, destroy before it.
- `set_renderer_backend(name)`: wire label reported by capabilities
  (`"raster"`, `"d3d12"`).
- Registered engine domains: `Window` (list/get/create/close/activate/
  setBounds/setPlacement with honest `closed`/`denied` outcomes), `Tab`
  (list/create/select/close/move/transfer), `Input` (click with node mode
  and strict layout checks, pointer, drag, key, typeText, wheel, ime,
  releaseAll), `UI` (snapshot, resolveNode), `Capture` (screenshot),
  `Trace` (start/stop/status). Window/tab commands call the same shell
  actions products use; semantic clicks go through the same pointer
  pipeline.

## transport.h - the optional loopback service

- `TransportConfig`: host/port (loopback literal, 0 = OS-assigned),
  token (required), header/body caps, `max_connections`, socket and
  response timeouts, auth-failure limit and lockout.
- `AutomationTransport`: `start()` (false on empty token, bind conflict or
  Winsock failure - it never listens elsewhere), `stop()` (bounded,
  idempotent, joins workers, closes every owned socket), `running`,
  `port`, `endpoint`, `describe()` (connection descriptor without the
  token), `stats()`.

## render.h - capture capability

- `CapturedFrame`: independent premultiplied BGRA pixels plus backend and
  size; `valid()` checks the buffer matches the dimensions.
- `IRenderer::supports_capture()` / `capture_frame(id, out)`: optional,
  default-absent. Implementations copy the last presented frame; they
  never hand out the live canvas or a GPU resource.

## shell.h - observation surface

`ShellObserver` (non-owning, UI thread): window/tab lifecycle,
`activation_changed`, raw `shell_event`, `frame_presented(window, serial)`,
`animations_settled`, `drag_started`/`drag_finished`. `Shell::add_observer`
/ `remove_observer`, `frame_serial(window)`, `has_active_animations(window)`
and `body_bounds(window)` are the supporting queries. Observers never
change product callback order.

## platform.h - platform capabilities

Optional virtuals with default-absent behavior: `activate`,
`set_client_bounds`, `begin_move_session`/`end_move_session` (stepwise
tear-off drags), `request_delayed_wake` (bounded wait polling). Event
extensions: `KeyUp`, `MoveEnded`, wheel `wheel_x`, `click_count`,
`buttons` bitmask and `repeat`. The Win32 backend implements all of them.
