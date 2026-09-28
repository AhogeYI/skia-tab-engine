# Build an application with the TabEngine SDK

This guide is for application developers who have an **extracted TabEngine SDK**
but do not have the TabEngine or Skia source trees. The SDK includes the
libraries, DLL, headers, CMake package, API reference, and a complete example
application. It does not include a C++ compiler or the Windows SDK.

## Supported package

The current SDK is for Windows x64, C++20, and the Visual Studio 18 2026 MSVC
toolchain. Debug and Release are separate ZIP files. Use the matching SDK for
your build configuration; do not link a Debug application to a Release SDK or
vice versa. The package's `manifest.json` states its version, platform,
configuration, pinned Skia revision, and SHA-256 hashes of packaged files.

After extraction, the SDK root is the directory containing `manifest.json`:

```text
tabengine-sdk-<version>-windows-x64-<config>/
  README.md                     this guide
  manifest.json
  bin/skia.dll
  include/tabengine/*.h
  skia/include/...              matching Skia headers
  lib/*.lib
  lib/cmake/TabEngine/...
  docs/API_REFERENCE.md
  examples/sdk_hello_tabs/      complete application source
  licenses/                     third-party notices
```

Obtain the package and its expected ZIP SHA-256 from your distributor, then
verify the archive before extraction. The SDK manifest checks the contents of
an extracted archive, but it is not a substitute for a trusted expected ZIP
hash. TabEngine currently has no public release location; local SDK archives
are made by its maintainers.

## Run the included application

Open PowerShell in the extracted SDK root. Install CMake 3.24 or newer and
Visual Studio 18 2026 with the C++ and Windows SDK components. The example
only consumes the extracted package; it does not fetch or build Skia.

```powershell
$config = (Get-Content .\manifest.json -Raw | ConvertFrom-Json).configuration
cmake -S examples/sdk_hello_tabs -B "build/hello-$config" `
  -G "Visual Studio 18 2026" -A x64
cmake --build "build/hello-$config" --config $config
Start-Process -FilePath "./build/hello-$config/$config/tabengine_hello_tabs.exe"
```

The example opens a native tabbed window. Click **New page** in the
application-owned body, use the tab strip's plus and close controls, drag a
tab to reorder or tear it into a new window, and try Ctrl+T, Ctrl+W, Ctrl+N,
and Ctrl+Tab. Use `--raster` to force the CPU renderer. For a noninteractive
startup/lifetime check, run the executable with `--smoke`; it creates a hidden
window and exits without showing UI.

The example's CMake project can also be copied outside the SDK. In that case,
configure it with `-DTABENGINE_SDK_ROOT=<extracted-sdk-root>`. The build copies
`bin/skia.dll` beside the executable. Keep the package and its configuration
together when moving the build to another machine.

## Start your own application

1. Create a C++20 CMake project. Set `TABENGINE_SDK_ROOT` to the extracted SDK
   root and call `find_package(TabEngine <sdk-version> EXACT CONFIG REQUIRED
   PATHS "${TABENGINE_SDK_ROOT}" NO_DEFAULT_PATH)`.
2. Link your application to `TabEngine::SDK`. That target supplies the
   TabEngine libraries and matching Skia compile/link requirements. If your
   application uses the package's FreeType build, also link
   `TabEngine::FreeType`.
3. Copy `${TABENGINE_SDK_ROOT}/bin/skia.dll` beside the executable. Do not
   select a different Skia import library or independent Skia headers.
4. Implement `tabengine::IClient`. Its `create_tab()` returns a unique
   application-owned `ContentId` and a tab title. Keep each content instance
   alive through attach/detach and release it in `tab_closed()`.
5. Create a platform and renderer, construct `tabengine::Shell`, open a window,
   then run the platform event loop. The example shows the complete sequence:

   ```cpp
   auto platform = tabengine::make_win32_platform();
   auto renderer = tabengine::make_skia_windows_renderer();
   MyClient client;
   tabengine::Shell shell(*platform, *renderer, client);
   client.bind(shell, *platform); // If your client needs these references.
   auto window = shell.open_window();
   if (!window) return 1;
   return platform->run();
   ```

6. Implement `paint_body()` and `body_event()` for your product UI. The body
   rectangle and pointer positions are in native window client pixels. Use
   `platform->scale(window)` when drawing logical-size controls. The canvas
   is the same Skia canvas used for the chrome; draw only inside the body
   rectangle. TabEngine does not supply an address bar, document model,
   navigation, or a web engine.

`examples/sdk_hello_tabs/main.cpp` is intended to be copied and modified. It
shows content ownership, a product-drawn button, event handling, text drawing,
window startup, and a hidden smoke mode. See [API reference](API_REFERENCE.md)
for callback timing, identifiers, events, and the available interfaces.

## Package targets and deployment

| CMake target | Use |
| --- | --- |
| `TabEngine::SDK` | Normal application entry point: TabEngine shell, Win32 host, and matching Skia usage requirements. |
| `TabEngine::Skia` | Imported `skia.dll`/`skia.lib` and matching headers, exported by the package. |
| `TabEngine::FreeType` | Optional FreeType, libpng, and zlib libraries from the same build. |
| `TabEngine::Core`, `TabEngine::Win32`, `TabEngine::SkiaShell` | Individual library targets for advanced consumers. |

The SDK uses static TabEngine `.lib` files plus a dynamic `skia.dll`. Deploy
that DLL and the normal Visual C++ runtime required by your chosen MSVC
configuration. Debug builds require a matching development runtime and are not
ordinary redistributable application builds. The optional symbols ZIP is for
debugging; it is not required to link or launch the application.

TabEngine currently promises source compatibility, not a stable C++ binary
ABI across compiler or SDK upgrades. Rebuild your application when changing
the SDK. Keep Debug and Release in separate CMake build directories because
CMake caches the SDK path and imported targets.

## Common problems

| Problem | Check |
| --- | --- |
| CMake cannot find TabEngine | `TABENGINE_SDK_ROOT` must contain `manifest.json` and `lib/cmake/TabEngine/TabEngineConfig.cmake`. Point to the extracted root, not the ZIP or its parent. |
| Wrong configuration or CRT link errors | Read `manifest.json` `configuration`; use a matching build and MSVC toolchain. Start a new build directory after switching SDKs. |
| `skia.dll` cannot be loaded | Put the package's `bin/skia.dll` beside the executable. The included example copies it automatically. |
| Tabs work but the body is empty | Implement `IClient::paint_body`; TabEngine owns chrome, not application content. |
| A transferred tab loses its content | Preserve your `ContentId` across `tab_detached` and `tab_attached`. Only `tab_closed` is final destruction. |
| Input or redraw appears stuck | Run `IPlatform::run()` on the UI thread; request a redraw with `IPlatform::invalidate(window)` after changing product-owned state. |

For public API details, read [API reference](API_REFERENCE.md) and the
installed `include/tabengine/*.h` headers. The bundled example is the
executable reference for the full startup path.
