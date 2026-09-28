# TabEngine SDK development guide

This guide covers building the Windows SDK, changing TabEngine, validating a
packaged consumer, and integrating the SDK into an application. It describes
the current local build; it is not a GitHub publication procedure. TabEngine
has no public remote or project license yet.

## Scope and build inputs

The supported SDK target is Windows x64 with Visual Studio 18 2026 and C++20.
The SDK contains TabEngine static libraries, one pinned Skia DLL and import
library, matching TabEngine and Skia headers, FreeType/libpng/zlib libraries,
CMake package files, a manifest, and third-party notices. Debug and Release
are separate packages. A consumer must use the matching SDK configuration and
an MSVC toolchain compatible with the package. There is no stable C++ binary
ABI across arbitrary compiler or SDK updates; rebuild consumers after an SDK
update.

Install Visual Studio with the C++ and Windows SDK components, CMake, Ninja,
Git, and Python. The first Skia build needs network access. The script locates
MSBuild and `lib.exe` through `PATH` or `vswhere`. Run all commands below from
the TabEngine repository root in PowerShell.

The relevant inputs are:

| Input | Purpose |
| --- | --- |
| `third_party/SKIA_REVISION.txt` | Pinned Skia commit. |
| `tools/sdk.py` `EXTERNALS` | Exact external commits, checked against Skia `DEPS`. |
| `cmake/skia_shared_args.gn` | Shared Skia feature selection. The script supplies Debug/Release and CRT flags. |
| `CMakeLists.txt` project version | SDK version and CMake package version. |
| `include/tabengine/` | Installed public TabEngine API. |
| `tests/sdk_consumer/` | Consumer built outside the source tree by `verify`. |

The SDK builder owns Skia. A product using the SDK does not run GN, compile
Skia, or choose an independent Skia revision. The older `TABENGINE_SKIA_ROOT`
demo build in the README is a separate workbench path and is not the SDK
packaging path.

## First build

For a complete build and validation of both configurations:

```powershell
python tools/sdk.py release --config Debug
python tools/sdk.py release --config Release
```

`release` runs `build-skia`, `package`, and `verify` in order. It creates these
ignored local outputs:

```text
third_party/skia/                         pinned source and externals
build/skia-shared/Debug|Release/          prepared shared Skia inputs
build/sdk-build/Debug|Release/            TabEngine CMake build trees
build/sdk-stage/Debug|Release/            installed SDK roots
build/logs/                               Skia and install logs
dist/tabengine-sdk-<version>-windows-x64-<config>.zip
dist/tabengine-sdk-<version>-windows-x64-<config>-symbols.zip (when symbols exist)
```

`release` names a **local artifact build**. It does not publish a GitHub
Release, create a Git tag, or upload anything. It rebuilds the shared Skia
input, so use the shorter cycles below when only TabEngine code has changed.

## Development cycles

### Change the core, Win32 host, shell, or renderer

Use the relevant source tests during editing. A core-only build needs no Skia:

```powershell
cmake -S . -B build/core -G "Visual Studio 18 2026" -A x64
cmake --build build/core --config Debug --target tabengine_core_test
ctest --test-dir build/core -C Debug --output-on-failure -R tabengine_core_test
```

For shell or renderer changes, test against the same shared Skia package that
will enter the SDK. After the first `build-skia` or `release` run:

```powershell
cmake -S . -B build/sdk-tests/Debug -G "Visual Studio 18 2026" -A x64 `
  -DTABENGINE_BUILD_SKIA=ON `
  -DTABENGINE_SKIA_ROOT="${PWD}/build/skia-shared/Debug"
cmake --build build/sdk-tests/Debug --config Debug --target tabengine_shell_test tabengine_windows_renderer_test
$env:PATH = "${PWD}/build/skia-shared/Debug/bin;$env:PATH"
ctest --test-dir build/sdk-tests/Debug -C Debug --output-on-failure -R '^(tabengine_shell_test|tabengine_windows_renderer_test)$'
```

Select other affected test targets and CTest names when changing their code.
For Release tests, use a separate `build/sdk-tests/Release` tree and the
Release shared Skia package. Once the change is ready for SDK consumers,
reuse the existing shared Skia package and rebuild only TabEngine and the ZIP:

```powershell
python tools/sdk.py package --config Debug
python tools/sdk.py verify --config Debug
```

Run the same two commands with `--config Release` when validating a release
candidate. `package` replaces `build/sdk-stage/<config>` and the matching ZIP.
It requires `build/skia-shared/<config>` from an earlier `build-skia` or
`release` run. If that input is missing, prepare it with
`python tools/sdk.py build-skia --config Debug` (or Release) first.

### Change Skia or its build options

Update the Skia commit, the matching `EXTERNALS` revisions from that commit's
`DEPS`, and the shared GN configuration as one reviewed change. If the
consumer-visible package changes, update the TabEngine version in
`CMakeLists.txt` and the downstream version requirement. Build both
configurations from the new inputs:

```powershell
python tools/sdk.py release --config Debug
python tools/sdk.py release --config Release
```

The script refuses an external revision that disagrees with Skia `DEPS` and
refuses to replace a modified dependency checkout. Inspect the license bundle
and runtime DLL closure when changing Skia features or externals. Do not
assume an old package's notices cover newly enabled code.

### Package an existing shared Skia build

`package --skia-root <path>` accepts a prepared shared package containing
`bin/skia.dll`, `lib/skia.lib`, `lib/freetype2.lib`, `lib/libpng.lib`,
`lib/zlib.lib`, matching `include/`, `modules/`, FreeType headers under
`third_party/freetype/include/`, `licenses/`, `SKIA_BUILD_INFO.txt`, and
`skia_args.gn`. Use this only when the package was built from the pinned
TabEngine inputs and with matching Debug/Release options. `package` does not
prove the provenance of an arbitrary `--skia-root` path.

## Package and consumer contract

Each ZIP contains one top-level directory with a relocatable SDK root. The
root contains `manifest.json`, `bin/skia.dll`, `lib/`, `include/tabengine/`,
`skia/`, `docs/`, `examples/sdk_hello_tabs/`, `licenses/`, and
`lib/cmake/TabEngine/`. Extract the archive and point
the consumer at that root, not at its parent directory or at the ZIP itself.

The CMake package provides:

| Target | Consumer use |
| --- | --- |
| `TabEngine::SDK` | Aggregate TabEngine shell/Win32 target; propagates bundled Skia include, `SKIA_DLL`, and link requirements. |
| `TabEngine::Skia` | Imported Skia DLL/import library target, available for direct target references. |
| `TabEngine::FreeType` | Optional matching FreeType plus libpng/zlib static libraries for a host font engine. |
| `TabEngine::Core`, `TabEngine::Win32`, `TabEngine::SkiaShell` | Individual installed libraries; prefer the aggregate target for normal applications. |

Minimal application CMake wiring:

```cmake
cmake_minimum_required(VERSION 3.24)
project(MyTabbedApp LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(TABENGINE_SDK_ROOT "" CACHE PATH "Extracted TabEngine SDK root")
find_package(TabEngine 0.1.0 EXACT CONFIG REQUIRED
    PATHS "${TABENGINE_SDK_ROOT}" NO_DEFAULT_PATH)
add_executable(my_tabbed_app main.cpp)
target_link_libraries(my_tabbed_app PRIVATE TabEngine::SDK)
add_custom_command(TARGET my_tabbed_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${TABENGINE_SDK_ROOT}/bin/skia.dll"
        "$<TARGET_FILE_DIR:my_tabbed_app>/skia.dll")
```

Set `TABENGINE_SDK_ROOT` to the extracted SDK root. Copy `skia.dll` next to
the executable for launch. An application that builds against the bundled
FreeType also links `TabEngine::FreeType`. The SDK does not contain RmlUi or
Lua; those remain application decisions. Product content, address bars,
navigation, and plugin behavior belong to the application, not TabEngine.

`tests/sdk_consumer/` is an executable example of the package contract. It
compiles direct Skia calls, TabEngine text and platform/renderer factories,
and FreeType initialization through the installed targets. The Win32 demo in
`examples/win32_demo.cpp` is the larger example of `IClient` and `Shell`
integration. Public API contracts are in `include/tabengine/`; start with
`shell.h`, `platform.h`, and `render.h`.

## Validation and handoff

```powershell
python tools/sdk.py verify --config Debug
python tools/sdk.py verify --config Release
```

`verify` extracts the ZIP into a temporary location, checks every file listed
in `manifest.json`, builds an external CMake consumer, then builds and runs the
packaged `sdk_hello_tabs` example without source-tree inputs. If a
symbols ZIP exists, it also checks its reference to the SDK ZIP hash and its
PDB hashes. It does **not** compare the SDK against a separately committed
expected ZIP hash, certify the compiler ABI, or publish the artifact. A
downstream release process must pin and verify the ZIP SHA-256 independently.

Before handing a package to an application, record its version, configuration,
platform, ZIP SHA-256, Skia revision, and any local TabEngine source commit.
Keep Debug and Release roots and build directories separate. Reconfigure the
consumer when switching roots; do not reuse a CMake cache that points to an
older SDK. An application can use `build/sdk-stage/<config>` directly for local
development or the extracted ZIP for a clean consumer check. Both paths use
the same CMake package.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| MSBuild or `lib.exe` not found | Install the VS C++ workload and x64 tools; check `vswhere` or a developer shell. |
| A dependency fetch fails | Check network access and the exact pinned commit. The script tries the listed mirrors but never selects a floating revision. |
| Modified Skia checkout is refused | Preserve or reset your changes deliberately before using the pinned fetch path; the builder will not overwrite them. |
| `package` cannot find `skia.dll` | Run `build-skia --config <config>` first, or pass a complete matching `--skia-root`. |
| Consumer cannot find TabEngine | Point CMake to the extracted SDK root containing `lib/cmake/TabEngine/TabEngineConfig.cmake`. |
| Link or CRT mismatch | Confirm architecture, SDK configuration, MSVC toolset, and `/MDd` for Debug or `/MD` for Release. Never mix the two SDKs. |
| Application cannot load `skia.dll` | Deploy the DLL beside the executable; check other runtime dependencies with the chosen Windows toolchain. |
| `verify` reports a file mismatch | Repackage from the intended inputs. Do not edit files inside a staged or extracted SDK. |

Skia/MSBuild logs are in `build/logs/`. Errors from the installer and external
consumer appear in the command output. If a package is to be distributed,
settle TabEngine's project license and audit the bundled third-party notices
before publication.
