# TabEngine

An embeddable C++20 tab-strip and native-window framework for desktop
applications. TabEngine draws the tab strip, title-bar controls, and native
window chrome; the application owns everything below the strip and paints it
on the same Skia canvas through one callback. Its tab model and drag
responsibilities follow the architecture of Chromium's tab strip (model, view,
native host, and drag session as separate pieces) while the implementation is
original, with no Chromium code or build dependency.

TabEngine is a component, not an application framework. It does not implement
an address bar, navigation, file management, session files, menus, or a web
engine — the application supplies all of that and keeps full control of
content, input routing, and styling.

## Features

- **Tab model** — ordered tabs with active selection and metadata; tabs move
  between windows while keeping their tab and content identity. Closure is
  vetoable by the application, and a transfer never destroys content: the
  application-owned instance is detached and reattached, reported as closed
  exactly once when finally closed.
- **Dragging** — pointer-following in-strip reordering, native tear-off of a
  tab into a new OS window, and re-attachment over another tab strip. Escape,
  capture loss, or deactivation restores the original order; a canceled
  tear-off restores the tab to its source window when it still exists.
- **Motion** — animated creation, close, reorder, and drag settling; hover
  transitions; a delayed hover card with fade in/out and an
  application-painted preview.
- **Input** — host-first shortcut handling (return true to consume a chord
  before the built-in Ctrl+T/W/N/Tab bindings), modifier-aware chords reaching
  the application body, wheel and character events, and a full IME composition
  contract so applications with in-place text editors receive preedit and
  commit strings and place the candidate window themselves.
- **Rendering** — Skia Ganesh on a D3D12 flip-model swapchain with a per-window
  CPU-raster fallback, plus DirectWrite-backed UI and caption typefaces. The
  application draws its body into the same canvas the chrome uses.
- **Theming** — configurable chrome colors and metrics, an application-painted
  leading slot, optional tab icons, and up to four extra caption buttons the
  application paints and handles while TabEngine keeps their layout and hit
  testing aligned with the strip.
- **Automation** — an in-process control runtime with a method registry,
  semantic snapshots merging engine chrome with application-provided
  nodes, real input synthesis (clicks by node or coordinate, drags that
  tear off through the shell's own state machine, keys, text, IME, wheel),
  frame-accurate screenshots on both Raster and D3D12 with bounded
  artifacts, typed wait conditions, operations, an event ring and bounded
  traces, plus an optional loopback HTTP transport and a Python CLI.
  External tools drive a running application the way a user does; the
  engine core stays free of product concepts.
- **Win32 platform** — custom frame with resize and caption hit testing,
  per-monitor DPI and activation notifications, interactive-placement
  callbacks, a cross-thread UI wake, and a native move loop with
  `Completed`/`Canceled`/`Unsupported` results.

## Platform support

| Platform | Status |
| --- | --- |
| Windows 10/11, x64 (Win32 + D3D12) | Implemented, development preview |
| Other desktop platforms | Planned; the model, layout, and host contracts carry no Win32 types — see the [porting guide](docs/PORTING.md) |

## Quick start from source

Core library and tests build with no Skia package at all:

```powershell
cmake --preset core
cmake --build --preset core
ctest --preset core
```

The Windows shell, renderer, workbench demo, and their tests additionally need
a Skia package with `include/` and `lib/skia.lib`. `tools/sdk.py build-skia`
prepares one from the pinned Skia revision in `third_party/SKIA_REVISION.txt`:

```powershell
python tools/sdk.py build-skia --config Debug
cmake -S . -B build/windows -G "Visual Studio 18 2026" -A x64 `
  -DTABENGINE_BUILD_WIN32_DEMO=ON `
  -DTABENGINE_SKIA_ROOT="${PWD}/build/skia-shared/Debug"
cmake --build build/windows --config Debug
ctest --test-dir build/windows -C Debug --output-on-failure
```

Run `build/windows/Debug/tabengine_win32_demo.exe` for the workbench (add
`--raster` to force the CPU backend). For a library-only Windows build without
the demo, set `TABENGINE_BUILD_SKIA=ON` and `TABENGINE_BUILD_WIN32_DEMO=OFF`.

Both paths also exist as CMake presets: `cmake --preset core` works on any
desktop OS with any generator, and the `windows-demo` preset reads the Skia
package root from the `TABENGINE_SKIA_ROOT` environment variable.

## Using the SDK

The SDK packages a pinned Skia DLL, import library, and headers together with
the TabEngine libraries, a relocatable CMake package (`TabEngine::SDK`,
`TabEngine::FreeType`), documentation, and a complete example:

```powershell
python tools/sdk.py release --config Release   # builds Skia, TabEngine, and dist/tabengine-sdk-*.zip
python tools/sdk.py verify --config Release    # extracts, hashes, builds an external consumer
```

A consumer only declares `find_package(TabEngine CONFIG REQUIRED)`, links
`TabEngine::SDK`, and deploys `bin/skia.dll` beside its executable. Debug and
Release packages are separate; see the
[SDK user guide](docs/SDK_USER_GUIDE.md) and the packaged
`examples/sdk_hello_tabs` application. Published SDK archives and their
SHA-256 hashes are available on
[GitHub Releases](https://github.com/AhogeYI/skia-tab-engine/releases).
Local development versions in this checkout may be newer than the latest
published SDK; check the manifest and release tag when choosing a package.

## A minimal application

The complete, buildable version of this example lives in
`examples/sdk_hello_tabs` (also inside every SDK package):

```cpp
#include "tabengine/shell.h"
#include "tabengine/text.h"
#include "tabengine/win32_platform.h"
#include "include/core/SkCanvas.h"

class HelloClient final : public tabengine::IClient {
public:
    // The four pure virtuals: create_tab, tab_closed, paint_body, body_event.
    tabengine::NewTab create_tab() override {
        const tabengine::ContentId id = next_content_++;
        return {id, "Page " + std::to_string(id)};
    }

    void tab_closed(tabengine::ContentId) override {}

    std::string window_title() override { return "Hello Tabs"; }

    // Everything below the tab strip is the application's to paint; titles
    // update through Shell::update_tab, input arrives in body_event.
    void paint_body(tabengine::WindowId, tabengine::TabId,
                    SkCanvas& canvas, tabengine::Rect body) override {
        tabengine::paint_ui_text(canvas, "The application owns this area.",
                                 static_cast<float>(body.x + 24),
                                 static_cast<float>(body.y + 54),
                                 25.0f, 0xFF162238);
    }

    void body_event(const tabengine::Event&, tabengine::Rect) override {}

private:
    tabengine::ContentId next_content_ = 1;
};

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    auto platform = tabengine::make_win32_platform();
    auto renderer = tabengine::make_skia_windows_renderer();
    HelloClient client;
    tabengine::Shell shell(*platform, *renderer, client);
    if (!shell.open_window()) return 1;  // visible window with one initial tab
    return platform->run();
}
```

Tear a tab out of the resulting window and it becomes a real second OS window;
drag it back over the strip to re-attach. `examples/win32_demo.cpp` is a
larger workbench that paints a toolbar, sidebar, and cards in its own client
to show the boundary in both directions.

## How embedding works

- `Shell` owns the tab model and window membership. The application owns
  content, identified by `ContentId`, and creates it in `IClient::create_tab`.
- `IClient` receives attach/detach/activation, body geometry, DPI, and
  placement callbacks, can veto closure, and paints and handles the body
  (plus tab icons, the leading slot, extra caption buttons, and hover-card
  subtitles/previews) — all through one virtual interface.
- `IPlatform` owns native windows, input, move loops, a monotonic clock, and
  one-shot animation frame scheduling. `IRenderer` supplies a canvas per
  window and presents it.
- Reentrant structural operations are rejected while a mutation callback
  runs; canceled drags and tear-offs restore state without ever reporting a
  false `tab_closed`.

The full contract is documented in [docs/API_REFERENCE.md](docs/API_REFERENCE.md)
against the headers in `include/tabengine/`.

## Documentation

| Document | Contents |
| --- | --- |
| [docs/SDK_USER_GUIDE.md](docs/SDK_USER_GUIDE.md) | Building an application from an extracted SDK |
| [docs/API_REFERENCE.md](docs/API_REFERENCE.md) | Public API contract, header by header |
| [docs/AUTOMATION_GUIDE.md](docs/AUTOMATION_GUIDE.md) | Enabling, securing and extending the automation runtime and transport |
| [docs/AUTOMATION_API_REFERENCE.md](docs/AUTOMATION_API_REFERENCE.md) | Automation API reference, header by header |
| [docs/SDK_DEVELOPMENT.md](docs/SDK_DEVELOPMENT.md) | Building and packaging the SDK, pinned Skia inputs |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Development environment, build/test loop, contribution terms |

## Project status

TabEngine is a development preview. The public API promises **source
compatibility only** — there is no stable binary ABI, so rebuild consumers
when updating the SDK.

What is verified: thirteen CTest suites cover the model, shell lifecycle, drag
transfer and cancellation, IME and character input, text rendering, hidden
window rendering, D3D12 resize with backend retention, the automation
runtime (protocol values, registry, queueing, waits, events), the shell
bridge over real protocol calls, and the loopback transport over real
sockets; GPU and raster **capture** checks assert pixel content on both
public renderers; a second window and a scripted native tear-off have been
confirmed on Windows; and the platform-neutral core is built and tested on
Linux, macOS, and Windows in Debug and Release by a CI gate
(`.github/workflows/ci.yml`) that keeps
OS-specific dependencies out of the contracts. The packaged SDK is validated
by extracting it outside the source tree and building and running both
bundled examples, including the automation demo's self-check, for Debug and
Release. What is not yet validated:
physical pointer feel,
attachment over an existing window, multi-monitor DPI switches, and IME
candidate behavior on real hardware need hands-on testing, and other desktop
platforms are unimplemented. Native input injection, OLE drag-and-drop and
system IME candidate windows are explicitly out of scope for the first
automation version and report `Unsupported`; keyboard
focus/accessibility, touch, pinned tabs, and tab groups are future work.

## Contributing

Contributions are welcome under the Apache-2.0 license with a DCO
`Signed-off-by` (no CLA) — see [CONTRIBUTING.md](CONTRIBUTING.md). Report
security issues privately through [SECURITY.md](SECURITY.md); please keep
conduct discussions within [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).

## Acknowledgments

- Chromium's [TabStripModel](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/chrome/browser/ui/tabs/tab_strip_model.h),
  [Views Widget](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/docs/ui/views/overview.md),
  and [TabDragController](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/chrome/browser/ui/views/tabs/dragging/tab_drag_controller.h)
  served as the responsibility model for tab, window, and drag separation.
- TabEngine builds on [Skia](https://skia.org) and its bundled components;
  their licenses are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

TabEngine is released under [Apache-2.0](LICENSE); copyright is held by The
TabEngine Authors. See [NOTICE](NOTICE) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for third-party components
redistributed with the SDK packages.
