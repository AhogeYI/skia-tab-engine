# TabEngine

An embeddable C++20 tabbed desktop window framework. The current implementation prioritizes Windows. Its model, layout, and host contracts have no Win32 types; other desktop backends can be added without changing application code.

This repository is being developed independently. It does not implement an address bar, navigation, file management, session files, menus, or a web engine. The application supplies a content ID for each tab and paints and handles the entire area below the tab strip through `IClient`.

## Current capabilities

- Ordered tabs, active selection, metadata updates, and transfer between windows while preserving tab and content IDs.
- Width adapting tab strip, selection, close and new tab controls, in strip reorder, and a Win32 native window move loop for tear off and attach.
- Custom Win32 frame with resize and caption hit testing; a Skia Ganesh/D3D12 flip-swapchain renderer with per-window raster fallback; a visual workbench application.
- DirectWrite-backed Skia UI and caption typefaces so tab titles and controls render in the Windows build.
- Headless core and shell tests, plus a hidden-HWND renderer smoke test. The core can be configured without Skia on other platforms.
- Configurable chrome colors, application-painted leading and tab-icon slots, and render backend/surface-size diagnostics.

The Windows demo now presents Skia drawings through D3D12 when a suitable hardware adapter and swapchain are available. If initialization or presentation fails, it uses Skia raster pixels via `StretchDIBits`. Full drag animation, keyboard focus/IME, accessibility, touch, pinned tabs, tab groups, and non-Windows backends are still future work. The current API promises source compatibility only; no binary ABI is specified.

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
cmake --build build/windows --config Debug --target tabengine_win32_demo tabengine_shell_test tabengine_windows_renderer_test
ctest --test-dir build/windows -C Debug --output-on-failure
```

The Skia package must contain `include/` and `lib/skia.lib`. The package used for the initial Windows build is a local MSVC Debug package; TabEngine does not import any external product code or CMake configuration. The demo executable is `build/windows/Debug/tabengine_win32_demo.exe` for the command above.

When working in the the development workspace workspace, select **TabEngine (vs-debug)** in VS Code's Run and Debug menu and press F5. Its pre-launch task configures and builds the demo in `tab-engine/build/vs-debug` using the workspace's Skia package.

## F5 workbench acceptance

The workbench deliberately draws a toolbar, sidebar, cards, and status area in its own `IClient`. None of that product UI is in the library. The library draws the tab strip, title controls, and native window frame; `IClient` supplies tab icons, branding, body painting, and body input. The status panel shows the active renderer and surface size, making resize errors visible.

Run `tabengine_win32_demo.exe --raster` to force the Skia CPU backend for visual comparison or machines without D3D12. In the development workspace, **TabEngine (raster)** is also a VS Code launch choice. The default F5 entry uses the GPU-first renderer.

Try tab selection and closing, the plus button, horizontal reordering, tearing a tab into a new native window, resizing, the workbench sidebar, and **NEW TAB**. The demo starts with three tabs so these behaviors can be exercised immediately. Automated checks cover model transfer, shell drag transfer, font pixels, hidden-window rendering, and D3D12 resize while keeping its backend. The current screenshot and interaction checks were performed on Windows with the repository's local Skia Debug package; native tear-off still needs a hands-on pointer check.

## Embedding boundary

`Model` owns tab presentation records and window membership. The application owns content identified by `ContentId`. `IClient` creates that ID, receives close notifications, and controls body rendering and input. `IPlatform` owns native windows, input and move loops. `IRenderer` supplies a canvas and presents each window. The controller in `Shell` connects these contracts.

The Windows drag path follows the responsibilities in Chromium's [TabStripModel](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/chrome/browser/ui/tabs/tab_strip_model.h), [Views Widget](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/docs/ui/views/overview.md), and [TabDragController](https://chromium.googlesource.com/chromium/src/+/b39ab7bc4ae0db831a930d373264e5edf8205fdc/chrome/browser/ui/views/tabs/dragging/tab_drag_controller.h): separate model, view, native host, and drag session. It is an original implementation with no Chromium build dependency.

Windows is the only backend implemented now. The intended future desktop scope is the platforms supported by desktop Chrome; platform behavior is negotiated through `IPlatform`. Mobile Chrome platforms are outside this desktop library's scope.

## Repository status

This local repository has no remote or public license yet. Those choices must be settled before distribution. Do not copy source from Chromium, Skia, or other projects into this repository without checking the file's license and required notices.
