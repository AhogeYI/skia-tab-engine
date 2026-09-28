# TabEngine

An embeddable C++20 tabbed desktop window framework. The current implementation prioritizes Windows. Its model, layout, and host contracts have no Win32 types; other desktop backends can be added without changing application code.

This repository is being developed independently. It does not implement an address bar, navigation, file management, session files, menus, or a web engine. The application supplies a content ID for each tab and paints and handles the entire area below the tab strip through `IClient`.

## Current capabilities

- Ordered tabs, active selection, metadata updates, and transfer between windows while preserving tab and content IDs.
- Content attach/detach and active-tab notifications, body geometry updates, and vetoable tab/window closure. Transfers keep the application-owned content alive; final closure reports it once.
- Win32 activation, DPI, and settled window placement notifications; host-first keyboard shortcut handling; Escape, capture loss, or deactivation restores an in-strip drag's original order. A canceled native tear-off restores the original tab and content to their source window when it still exists.
- 200 ms ease-out tab creation, close, reorder, and drag settling; 120 ms tab and new-tab hover color transitions. The platform supplies monotonic time and one-shot animation frames.
- A 300 ms delayed tab hover card with 200 ms fade-in, 150 ms fade-out, and animated movement between tabs. The application supplies its subtitle and optional preview painting.
- Width adapting tab strip, selection, close and new tab controls, pointer-following in-strip drag with a moving new-tab button, reorder, and a Win32 native window move loop for tear off and attach.
- Custom Win32 frame with resize and caption hit testing; a Skia Ganesh/D3D12 flip-swapchain renderer with per-window raster fallback; a visual workbench application.
- DirectWrite-backed Skia UI and caption typefaces so tab titles and controls render in the Windows build.
- Headless core and shell tests, plus a hidden-HWND renderer smoke test. The core can be configured without Skia on other platforms.
- Configurable chrome colors and leading-slot width, application-painted tab icons and optional caption buttons, and render backend/surface-size diagnostics.

The Windows demo now presents Skia drawings through D3D12 when a suitable hardware adapter and swapchain are available. If initialization or presentation fails, it uses Skia raster pixels via `StretchDIBits`. Keyboard focus/IME, accessibility, touch, pinned tabs, tab groups, and non-Windows backends are still future work. The current API promises source compatibility only; no binary ABI is specified.

## Build

Core library and tests, with no Skia package required:

```powershell
cmake -S . -B build/core -G "Visual Studio 18 2026" -A x64
cmake --build build/core --config Debug --target tabengine_core_test
ctest --test-dir build/core -C Debug --output-on-failure
```

Windows demo and shell test:

```powershell
cmake -S . -B build/windows -G "Visual Studio 18 2026" -A x64 `
  -DTABENGINE_BUILD_WIN32_DEMO=ON `
  -DTABENGINE_SKIA_ROOT=<path-to-skia-package>
cmake --build build/windows --config Debug --target tabengine_win32_demo tabengine_shell_test tabengine_native_drag_cancel_test tabengine_windows_renderer_test
ctest --test-dir build/windows -C Debug --output-on-failure
```

The Skia package must contain `include/` and `lib/skia.lib`. The package used for the initial Windows build is a local MSVC Debug package; TabEngine does not import any external product code or CMake configuration. The demo executable is `build/windows/Debug/tabengine_win32_demo.exe` for the command above.

For a library-only Windows build, set `TABENGINE_BUILD_SKIA=ON` and `TABENGINE_BUILD_WIN32_DEMO=OFF`. This builds `tabengine_skia` and its tests without compiling the workbench executable.

## Windows SDK build and local release artifacts

TabEngine owns the pinned Skia source revision in `third_party/SKIA_REVISION.txt` and the shared-library GN configuration in `cmake/skia_shared_args.gn`. Its SDK builder fetches only the pinned Skia dependencies, checks their revisions against Skia's `DEPS`, builds `skia.dll`, builds the TabEngine libraries, and packages them together. This is separate from the older static-Skia workbench build above.

```powershell
python tools/sdk.py release --config Debug
python tools/sdk.py release --config Release
```

The archives are `dist/tabengine-sdk-0.1.0-windows-x64-debug.zip` and `dist/tabengine-sdk-0.1.0-windows-x64-release.zip`. When PDBs are available, a matching `-symbols.zip` is emitted separately and tied to the SDK zip by SHA-256. The first run requires network access and the Visual Studio 18 2026 C++/MSBuild toolchain, CMake, Ninja, Git, and Python. Later runs reuse the pinned source and compiled objects. `build-skia`, `package`, and `verify` can also be run separately; `package --skia-root <path>` accepts an already built shared Skia package with `bin/skia.dll`, `lib/skia.lib`, headers, license bundle, build info, and GN arguments.

Each archive contains TabEngine static libraries, the pinned Skia DLL and import library, compatible public headers, a relocatable CMake package, third-party license notices, and a SHA-256 manifest. The build installs a single aggregate `TabEngine::SDK` target. It propagates the bundled Skia headers, `SKIA_DLL` definition, and import-library link. `TabEngine::FreeType` exposes the matching FreeType, libpng, and zlib static libraries from the same build for hosts that need a font engine such as RmlUi. Applications can still use Skia APIs directly, but do not run GN or select a separate Skia package. Deploy `bin/skia.dll` beside the application executable. The normal Windows/Visual C++ runtime remains a platform prerequisite; Debug binaries require the matching development runtime.

The `verify` action extracts the zip to a new directory, copies a small consumer outside the source tree, then builds and runs that consumer using only `find_package(TabEngine CONFIG REQUIRED)`, `TabEngine::SDK`, and `TabEngine::FreeType`. The consumer exercises Skia drawing, TabEngine text drawing, FreeType initialization, and the Win32 platform/renderer factories. The SDK's `manifest.json` records the Skia and external revisions, GN argument hash, build configuration, and every packaged file hash. Debug and Release SDKs are separate and must not be mixed. TabEngine still offers source compatibility rather than a stable cross-toolchain C++ ABI; rebuild consumers when updating the SDK.

These are local release artifacts. A public release still requires choosing a license for TabEngine itself and validating distribution notices. No File Manager build path is changed by this SDK work.

When working in the the development workspace workspace, select **TabEngine (vs-debug)** in VS Code's Run and Debug menu and press F5. Its pre-launch task configures and builds the demo in `tab-engine/build/vs-debug` using the workspace's Skia package.

## F5 workbench acceptance

The workbench deliberately draws a toolbar, sidebar, cards, and status area in its own `IClient`. None of that product UI is in the library. The library draws the tab strip, title controls, and native window frame; `IClient` supplies tab icons, branding, body painting, and body input. The status panel shows the active renderer and surface size, making resize errors visible.

Run `tabengine_win32_demo.exe --raster` to force the Skia CPU backend for visual comparison or machines without D3D12. In the development workspace, **TabEngine (raster)** is also a VS Code launch choice. The default F5 entry uses the GPU-first renderer.

Try tab selection and closing, the plus button, horizontal reordering, tearing a tab into a new native window, resizing, the workbench sidebar, **NEW WINDOW**, and **NEW TAB**. The demo starts with three tabs so these behaviors can be exercised immediately. Ctrl+T, Ctrl+W, Ctrl+N, and Ctrl+Tab switch or create tabs and windows. Automated checks cover model transfer, shell drag transfer, font pixels, hidden-window rendering, and D3D12 resize while keeping its backend. Windows screenshot checks confirmed GPU and raster rendering, resize, a second window, and a scripted native tear-off. Physical pointer feel and attachment to an existing window still need hands-on validation.

## Embedding boundary

`Model` owns tab presentation records and window membership. The application owns content identified by `ContentId`. `IClient` creates content, receives attach/detach/activation and body geometry callbacks, can veto tab/window closure, and controls body rendering and input. The public `Shell::move_tab` and `Shell::transfer_tab` operations route model changes through those callbacks. A transfer never calls `tab_closed`; a final close does. With other live tabs present, `close_tab` starts a 200 ms contraction, marks the tab `closing`, and reports `tab_detached`/`tab_closed` when the animation finishes. Closing the last live tab or its window completes immediately. Reentrant structural operations on a window are rejected while a tab or window mutation callback is running. `IPlatform` owns native windows, input, move loops, a monotonic clock, and one-shot animation frame scheduling. `IRenderer` supplies a canvas and presents each window. The controller in `Shell` connects these contracts.

`Shell::set_chrome_options` changes the application-painted leading slot and reserves up to four extra caption buttons before the system controls. The host paints and handles those buttons through `IClient`; TabEngine keeps their layout and hit testing aligned with the tab strip.

`IClient::window_title` supplies the native title of every new window, including tear-off windows. Its default is `Tabbed Window`; products should override it with their own identity.

`IClient::handle_shortcut` runs before the built-in Ctrl+T/W/N/Tab bindings. Activation, DPI, and settled interactive placement changes are delivered through separate `IClient` callbacks. Placement uses client-area bounds in screen pixels; `body_geometry_changed` reports the drawable area inside the window.

Hover cards use the library's delay, placement, hit testing, fade, and slide behavior. `hover_card_subtitle` returns application text; `paint_hover_card_preview` receives the preview area on the same Skia canvas. The active tab's card omits its preview, and the workbench paints a small application-owned preview for inactive tabs.

`IPlatform::run_native_move_loop` returns `Completed`, `Canceled`, or `Unsupported`. A canceled tear-off reattaches the tab at its original index and destroys the temporary window; an attach request over another tab strip takes precedence over the move loop's canceled result. If the original window disappears during the move, the torn window keeps the content alive. No drag rollback calls `tab_closed`.

The Windows drag path follows the responsibilities in Chromium's [TabStripModel](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/chrome/browser/ui/tabs/tab_strip_model.h), [Views Widget](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/docs/ui/views/overview.md), and [TabDragController](https://chromium.googlesource.com/chromium/src/+/b39ab7bc4ae0db831a930d373264e5edf8205fdc/chrome/browser/ui/views/tabs/dragging/tab_drag_controller.h): separate model, view, native host, and drag session. It is an original implementation with no Chromium build dependency.

Windows is the only backend implemented now. The intended future desktop scope is the platforms supported by desktop Chrome; platform behavior is negotiated through `IPlatform`. Mobile Chrome platforms are outside this desktop library's scope.

## Repository status

This local repository has no remote or public license yet. Those choices must be settled before distribution. Do not copy source from Chromium, Skia, or other projects into this repository without checking the file's license and required notices.
