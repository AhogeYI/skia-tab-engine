# Platform porting guide

TabEngine's stated desktop scope is the platforms supported by desktop Chrome.
Today only Windows ships. This guide records exactly where the platform
boundary sits, which Windows assumptions remain, and what a new backend has to
implement — so the multi-platform path is a planned extension, not a rewrite.

## What is platform-neutral today

The whole core is portable C++20 with no Skia and no OS headers:

- `include/tabengine/model.h`, `layout.h`, `theme.h`, `types.h` — data model,
  strip geometry, chrome metrics, value types.
- `include/tabengine/platform.h`, `render.h`, `shell.h` — the host contracts.
  They deliberately contain no HWND, NSWindow, or X11 types;
  `win32_platform.h` is the only Win32-facing public header, kept apart so
  application code that targets other platforms can ignore it.
- `src/core/` — model, layout, theme, and the `Shell` controller (event
  routing, drag state machine, animations). `Shell` is the portability
  anchor: it consumes only the contracts below.

The core library and its tests build and run wherever CMake ≥ 3.24 and a
C++20 compiler exist — `cmake --preset core` needs no Skia package. The CI
workflow builds and tests the core on Linux and Windows precisely to keep
this true: a Windows-only dependency cannot leak into the contracts, model,
layout, or theme without failing that gate.

## Windows-only inventory

| Area | File | What is Windows-specific |
| --- | --- | --- |
| Platform backend | `src/win32/win32_platform.cpp` | Message loop, custom frame hit testing, native move loop (`SC_MOVE` + thread-local input hooks), WM_CHAR/IME translation, per-monitor DPI |
| D3D12 renderer | `src/skia/d3d12_renderer.cpp` | Adapter selection, flip-model swapchain, fences, device-loss fallback policy |
| Raster presentation | `src/skia/raster_renderer.cpp` | Only the `present` step (`StretchDIBits` blit); surface creation and drawing are portable |
| Text | `src/skia/win32_text.cpp` | DirectWrite font manager; the paint/measure API itself is portable |
| SDK builder | `tools/sdk.py` | MSVC/MSBuild/GN toolchain, pinned Skia DLL packaging |

## Adding a platform backend

1. **`IPlatform`** (see `platform.h` for the full contract). A backend owns
   native windows and translates OS events into `Event`s. The parts that are
   optional and how they degrade:
   - IME: `set_ime_caret_provider` has a default no-op implementation; a
     backend that cannot support in-app preedit simply never emits
     `ImeStart/Update/Commit/Cancel`.
   - Native move loop: report `supports_native_move_loop() == false`; the
     shell skips native tear-off (`run_native_move_loop` is never asked for
     `Unsupported` platforms to tear) and drags stay in-strip or use a
     system drag-and-drop session instead.
   - Wake: `wake()` must be safe from any thread and may coalesce; handlers
     drain pending state rather than count calls.
   - Required: create/show/destroy, client size/origin, per-window DPI scale,
     pointer capture, `monotonic_seconds`, one-shot `request_animation_frame`.
2. **`IRenderer`** (`render.h`). One surface per window with
   attach/resize/canvas/present. Skia drawing is portable everywhere; only
   presentation is per platform — a GPU backend wraps each swapchain image in
   a `SkSurface` (D3D12 today; Vulkan or Metal follow the same shape), and a
   raster backend blits CPU pixels with the platform's blit call.
3. **Text.** The chrome needs a UI typeface plus a symbol typeface for
   caption glyphs, fed to the portable `paint_ui_text` /
   `paint_caption_symbol` / `measure_ui_text` entry points. A port replaces
   the DirectWrite font manager in `win32_text.cpp` with its platform's
   manager; nothing else changes.

## Source and build layout conventions

- Portable code stays in `src/core/` (and portable parts of `src/skia/`);
  backends live in `src/<platform>/` — the existing `src/win32/` is the
  template.
- CMake gates per-platform targets on platform checks (see the `WIN32`
  guards). A new backend adds its option and target the same way; the
  core-only configuration must keep configuring everywhere, and non-Windows
  configure prints a status line pointing here.
- The responsibility split (model, view, native host, drag session) follows
  Chromium's tab architecture; per-platform source directories are the same
  organization choice. The build system itself stays CMake — GN and
  depot_tools are Chromium infrastructure that does not pay for itself in a
  single-library repository; revisit only if the platform count grows past
  what one CMake graph holds comfortably.

## Definition of a port

A platform counts as supported when: the core suite passes, the platform
backend passes its own headless message-level tests, the workbench renders
through the platform's GPU path with a raster fallback, and drag, tear-off,
IME, and DPI behavior have hands-on validation — the same bar Windows is
currently held to (see "Project status" in the README).
