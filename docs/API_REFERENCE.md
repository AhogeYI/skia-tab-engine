# TabEngine 0.1 SDK API reference

This reference describes the public C++ headers installed with the Windows
x64 SDK. The declarations in `include/tabengine/*.h` are authoritative for
exact signatures. Start with the [SDK user guide](SDK_USER_GUIDE.md) and the
buildable `examples/sdk_hello_tabs` application.

## Ownership and lifecycle

An application creates `IPlatform`, `IRenderer`, and `IClient`, then constructs
`Shell` with references to them. Keep those three objects alive until after
`Shell` is destroyed. Run the platform event loop and call Shell's window/tab
operations on the UI thread; Shell is not a synchronized cross-thread API.

`Shell` owns the tab model, chrome behavior, animations, and native window
lifecycle. The application owns content and uses `ContentId` to identify it.
One `ContentId` should identify one live content instance. A tab transfer
keeps that ID and calls detach/attach callbacks; a final close calls
`tab_closed(ContentId)` exactly once. Keep content alive during transfers and
close animations. If background work changes product state, use the platform
`wake()`/`set_wake_handler()` contract to process it on the UI thread and
`invalidate(window)` to request paint.

### Identifiers and geometry (`types.h`)

`WindowId`, `TabId`, and `ContentId` are `std::uint64_t`. Zero is the invalid
window/tab value; the host chooses content IDs. `Point` and `Size` hold integer
coordinates and dimensions. `Rect{x,y,width,height}` has `right()`,
`bottom()`, and half-open `contains(Point)`. Body paint/input geometry uses
client pixels; `open_window` bounds are native window bounds in screen pixels.
`IPlatform::scale(window)` converts logical UI dimensions to pixels.

## Application entry point (`shell.h`)

### `IClient`

The following callbacks define the application side of the shell. Only
`create_tab`, `tab_closed`, `paint_body`, and `body_event` are pure virtual;
other callbacks have defaults.

| Callback | Meaning |
| --- | --- |
| `create_tab() -> NewTab` | Create application content and return its ID and initial title. Called for the initial tab and new-tab actions. |
| `window_title() -> std::string` | Native title for each window, including a new tear-off window. Default: `Tabbed Window`. |
| `allow_close_tab(window, tab, content)` | Return false to veto a tab close. The content remains owned by the application. |
| `allow_close_window(window)` | Return false to veto a window close. A last-tab close also checks this. |
| `tab_attached(window, tab, content)` | Content is now represented in the named window. |
| `tab_detached(window, tab, content)` | Content left that window. This can be a transfer, not destruction. |
| `active_tab_changed(window, previous, current)` | Active tab changed; either ID can be zero when entering/leaving a window. |
| `tab_closed(content)` | Final release notification for application content. Do not release on detach alone. |
| `body_geometry_changed(window, body, scale)` | Body rectangle or scale changed. `body` is in client pixels. |
| `window_activation_changed(window, active)` | Native activation state changed. |
| `dpi_changed(window, scale)` | Window DPI scale changed. Recompute product UI as needed. |
| `window_placement_changed(window, bounds, scale)` | Settled move/resize; bounds are client-area bounds in screen pixels. |
| `handle_shortcut(event) -> bool` | Called before engine KeyDown shortcuts. Return true when the product consumed the key. |
| `ime_caret_rect(window) -> Rect` | Logical, body-relative IME caret rectangle; return empty when no editor is active. |
| `paint_body(window, active_tab, canvas, body)` | Paint product content inside the body rectangle on the shared `SkCanvas`. |
| `body_event(event, body)` | Handle input routed to product content. Pointer positions and `body` are client pixels. |
| `paint_leading(window, canvas, bounds)` | Paint the application-owned leading slot in the tab strip. |
| `paint_tab_icon(window, tab, canvas, bounds)` | Paint an application icon for a tab. |
| `hover_card_subtitle(window, tab, content)` | Optional product subtitle in a tab hover card. |
| `paint_hover_card_preview(window, tab, content, canvas, bounds)` | Optional preview for an inactive tab. |
| `paint_extra_caption_button(window, index, canvas, bounds, hovered)` | Paint a reserved application caption button. |
| `extra_caption_button_pressed(window, index)` | Respond to that caption button. |

`NewTab{content,title}` uses a UTF-8 `std::string` title. Callback arguments
refer to the current Shell state; avoid starting another structural mutation
of the same window inside a mutation callback. Such reentrant structural
operations are rejected. Use an application queue if a callback must initiate
a later structural change.

### `Shell`

| Method | Meaning |
| --- | --- |
| `Shell(platform, renderer, client)` | Connects the platform's event handler to the engine. References must outlive Shell. |
| `open_window(bounds, with_initial_tab, visible) -> WindowId` | Create native window and renderer; optionally create the first tab. Returns zero on failure. Defaults: `{100,100,1100,720}`, true, true. |
| `new_tab(window) -> TabId` | Calls `create_tab`, inserts and selects the new tab. Zero means failure. |
| `close_tab(window, tab) -> bool` | Checks close vetoes. A nonfinal tab uses a close animation; final tab closes its window. |
| `close_window(window)` | Checks vetoes, closes all content, and destroys the native window. |
| `select_tab(window, tab) -> bool` | Change active tab; rejects a closing tab. |
| `move_tab(window, tab, index) -> bool` | Reorder inside one window. Index is zero based. |
| `transfer_tab(from, to, tab, index) -> bool` | Move a tab between windows while preserving its tab and content IDs. |
| `update_tab(window, tab, title, loading, attention) -> bool` | Update tab metadata and invalidate the window. |
| `model() -> const Model&` | Read-only access to windows, tabs, content IDs, and active tabs. |
| `chrome_targets(window) -> vector<ChromeTarget>` | Current tab/close/new-tab hit targets in client pixels, including animation position. Useful for accessibility and automation. |
| `set_body_drag(window, active)` | Route pointer moves over the strip to the application during an application-owned body drag; clear it on release/cancel/capture loss. |
| `set_theme(Theme)` | Change library-owned chrome colors and invalidate windows. |
| `set_chrome_options(ChromeOptions)` | Change leading-slot width and count of application caption buttons. |
| `on_event(event)` | Engine event entry point for a custom platform backend. The packaged Win32 platform connects this automatically. |

`ChromeTarget` has `kind` (`Tab`, `CloseTab`, or `NewTab`), `tab` (zero for the
new-tab target), and `bounds`. `ChromeOptions::leading_slot_width_dp` is a
logical width; `extra_caption_buttons` reserves buttons before the native
caption controls. The host paints and handles reserved buttons through
`IClient`.

## Input (`platform.h`)

`Event` carries a `type`, `window`, client and screen points, client `size`,
virtual `key`, Ctrl/Shift/Alt state, `code_point`, signed `wheel` delta,
UTF-8 `ime_text`, and `button` (`None`, `Left`, `Right`, `Middle`). On Windows,
one wheel notch is normally 120 units. Positions are physical pixels. Text
input and key input are separate: do not derive typed characters from
`KeyDown`.

| Event group | Routing |
| --- | --- |
| `KeyDown` | `handle_shortcut` first, then TabEngine's Ctrl+T/W/N/Tab chords, then `body_event` if unhandled. |
| `TextInput` | Unicode scalar in `code_point`, sent directly to `body_event`. |
| `ImeStart`, `ImeUpdate`, `ImeCommit`, `ImeCancel` | Directly to `body_event`. Update/commit text is UTF-8 in `ime_text`; update replaces the current preedit. |
| `PointerDown`, `PointerMove`, `PointerUp`, `CaptureLost` | Strip/caption input is handled by Shell; eligible body input is sent to `body_event`. The held/pressed button is in `button`. |
| `PointerLeave` | Clears Shell hover state; it is not forwarded to `body_event`. |
| `PointerWheel` | Sent to `body_event` with signed native `wheel` and client position. |
| `Paint`, `Resized`, `AnimationFrame` | Shell repaint and animation lifecycle. |
| `CloseRequested`, `Moving`, `WindowActivated`, `WindowDeactivated`, `DpiChanged`, `PlacementChanged` | Native window lifecycle, tab tear-off, and related client callbacks. |

`SurrogateComposer` is a helper for a custom UTF-16 platform backend: feed
one code unit at a time and emit `code_point` only when `feed()` returns true.
The packaged Win32 platform already performs this conversion. For IME
composition, `ime_caret_rect()` positions the native candidate window; an
empty rectangle requests system-default placement.

Default engine chords are Ctrl+T (new tab), Ctrl+W (close active tab), Ctrl+N
(new window), and Ctrl+Tab / Ctrl+Shift+Tab (cycle tabs). `handle_shortcut`
can consume a key before those bindings. Shift or Alt with Ctrl+T/W/N leaves
those modified chords available to the application.

## Platform and rendering (`platform.h`, `render.h`, `win32_platform.h`)

Normal Windows applications use `make_win32_platform()` and either
`make_skia_windows_renderer()` (D3D12 with raster fallback) or
`make_skia_raster_renderer()` (CPU). `IPlatform::run()` pumps native events and
returns when the native loop exits. `IPlatform::show(window)` displays a
window created with `visible=false`; `invalidate(window)` requests repaint.
`client_size`, `client_origin`, `scale`, and `native_handle` expose the native
window geometry/handle when an application needs integration with Win32.

`IPlatform` also defines window create/destroy, pointer capture,
minimize/maximize, native move loops, animation scheduling, cross-thread
`wake()` and UI-thread wake handling. Most applications need only the
packaged Win32 factory; implement this interface to add another backend.
`MoveLoopResult` is `Unsupported`, `Completed`, or `Canceled`.

For a custom platform backend, the complete `IPlatform` method groups are:

| Methods | Backend responsibility |
| --- | --- |
| `set_event_handler`, `set_caption_hit_handler` | Deliver native events and ask Shell which client points act as the caption. Shell installs these handlers; an application should not replace them afterward. |
| `set_wake_handler`, `wake` | Cross-thread notification; wake handlers execute on the UI pump thread and must drain coalesced work. |
| `set_ime_caret_provider` | Obtain the client-pixel caret rectangle for native IME placement. |
| `create`, `show`, `destroy`, `invalidate` | Window lifecycle and paint scheduling. |
| `monotonic_seconds`, `request_animation_frame` | Monotonic clock and one-shot animation scheduling. |
| `capture_pointer`, `release_pointer` | Pointer capture for tab drag gestures. |
| `minimize`, `toggle_maximize` | Native caption controls. |
| `client_size`, `client_origin`, `scale`, `native_handle` | Window geometry, DPI scale, and native handle lookup. |
| `window_at`, `supports_native_move_loop`, `set_client_origin`, `run_native_move_loop`, `end_native_move_loop` | Tear-off and attachment support. Return `Unsupported` if the backend cannot run a native move loop. |
| `run` | Pump the native UI loop and return its exit code. |

`IRenderer` defines attach, resize, detach, canvas acquisition, present, and
`info(window)`. `RenderInfo` reports `RenderBackend` (`None`, `Raster`, or
`D3D12`) and surface size. The application does not normally call renderer
lifecycle methods; Shell does. It may query `info()` for diagnostics.

| `IRenderer` method | Meaning |
| --- | --- |
| `attach(window, native_handle, size) -> bool` | Create a surface for a native window. |
| `resize(window, size)` | Resize that surface. |
| `detach(window)` | Release that surface. |
| `canvas(window) -> SkCanvas*` | Obtain the Skia canvas for Shell and client painting. |
| `present(window, native_handle)` | Present the completed frame. |
| `info(window) -> RenderInfo` | Query active backend and surface size. |

The shared `SkCanvas` is supplied by the SDK's pinned Skia build. Include
Skia headers from the SDK (for example, `include/core/SkCanvas.h`) and link
through `TabEngine::SDK`. `text.h` offers `paint_ui_text`,
`paint_caption_symbol`, and `measure_ui_text`; coordinates and font sizes are
pixels, so scale product UI text as needed.

## Model, layout, and theme (`model.h`, `layout.h`, `theme.h`)

`Model` stores `WindowTabs{id,tabs,active}` and `Tab{id,content,title,pinned,
loading,attention,closing}`. The `pinned` field is present in the data type;
the current shell does not offer a complete pinned-tab UX. `Model` exposes
create/remove window, add/close/select/move/transfer/update tab, mark closing,
window lookup, sorted `window_ids()`, and `set_observer(Change)`. `ChangeKind`
distinguishes window add/remove and tab add/remove/move/select/update. A
standalone `Model` can be used for model-level work, but do not mutate a
separate model and expect Shell's windows or animation state to follow it.
Use Shell's operations for application UI.

| `Model` method | Meaning |
| --- | --- |
| `set_observer(Observer)` | Receive `Change{kind,window,other_window,tab}` after model changes. |
| `create_window()`, `remove_window(id)` | Create/remove a model window. `remove_window` does not itself release application content. |
| `add_tab(window, content, title, index)` | Insert a tab; default index appends. Returns zero for an unknown window. |
| `close_tab(window, tab)` | Remove a tab immediately at model level. Shell adds close policy and animation. |
| `select_tab(window, tab)` | Set a valid tab as active. |
| `move_tab(window, tab, index)`, `transfer_tab(from, to, tab, index)` | Reorder or transfer while preserving IDs. |
| `update_tab(window, tab, title, loading, attention)` | Change tab metadata. |
| `set_tab_closing(window, tab)` | Mark a tab as closing. |
| `window(id)`, `window_ids()` | Look up a window or return sorted window IDs. `window(id)` has const and mutable overloads on a standalone Model. |

`Layout::tab_strip()` computes `StripLayout`: tab rectangles, new-tab and
leading-slot rectangles, caption region/count, and strip height. It accepts
either a tab count or a closing-state vector plus scale and `ChromeOptions`.
`insertion_index()` calculates a reorder destination; `drag_visual()`
computes pointer-following tab and new-button rectangles. Shell uses these
internally; applications may use the pure layout functions for inspection.

| `Layout` static method | Output |
| --- | --- |
| `tab_strip(width_px, count, scale, options)` | Strip geometry for a tab count. |
| `tab_strip(width_px, closing_flags, scale, options)` | Strip geometry including close-animation state. |
| `insertion_index(layout, x, dragged_index)` | Reorder insertion index. |
| `drag_visual(layout, dragged_index, pointer_x, grab_x, scale)` | Moving tab and new-tab button rectangles. |

`Theme` holds ARGB colors for library-owned chrome and provides
`Theme::light()`. Pass it to `Shell::set_theme()`; paint the application body
and product-specific controls using your own theme policy. TabEngine does
not interpret application content, navigation, menus, or plugins.
Its fields are `strip`, `body`, `tab_active`, `tab_inactive`, `tab_hover`,
`text`, `text_muted`, `tab_close`, `separator`, `new_tab`, `new_tab_hover`,
`hover_card`, `hover_card_border`, `caption_hover`, and
`caption_close_hover`. Colors use `0xAARRGGBB` values.
