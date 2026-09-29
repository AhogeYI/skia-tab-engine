# Contributing to TabEngine

Thank you for your interest in TabEngine. The project is at an early stage;
keep changes small, verifiable, and clearly layered. This document covers the
minimal loop — environment → build → test → pull request — plus the rules a
change must respect.

## Project scope

TabEngine is an embeddable C++20 tabbed desktop window framework, developed
independently. It implements the tab strip, window chrome, drag
and tear-off, and the host contracts. It does not implement an address bar,
navigation, file management, session files, menus, or a web engine: the
application owns content and paints everything below the tab strip through
`IClient`. Windows is the only backend today; the model, layout, and host
contracts carry no Win32 types so other desktop backends can be added without
changing application code.

## Environment requirements

| Component | Minimum | Used for |
| --- | --- | --- |
| Windows | Windows 10 | Win32 host, D3D12/DirectWrite runtimes |
| Visual Studio | 2026 with the C++ workload | MSVC toolchain, the `Visual Studio 18 2026` CMake generator |
| CMake | 3.24 | The build graph |
| Python | 3.10 | `tools/sdk.py` (Skia fetch, SDK build, packaging) |
| Git | any recent version | Dependency fetches and development |
| Ninja | required only for the Skia build | Invoked by GN inside `tools/sdk.py build-skia` |

The first Skia build needs network access. Two Skia paths exist: the SDK
builder in `tools/sdk.py` owns the pinned Skia revision end to end (the
supported path), while the older workbench build accepts any
`TABENGINE_SKIA_ROOT` package with `include/` and `lib/skia.lib`. See the
[README](README.md) and the [SDK development guide](docs/SDK_DEVELOPMENT.md)
for the full commands.

## Build and test

Core library and tests, no Skia required:

```powershell
cmake -S . -B build/core -G "Visual Studio 18 2026" -A x64
cmake --build build/core --config Debug --target tabengine_core_test
ctest --test-dir build/core -C Debug --output-on-failure
```

Windows shell, renderer, and demo (requires the shared Skia package from
`python tools/sdk.py build-skia --config Debug`):

```powershell
cmake -S . -B build/sdk-tests/Debug -G "Visual Studio 18 2026" -A x64 `
  -DTABENGINE_BUILD_SKIA=ON `
  -DTABENGINE_SKIA_ROOT="${PWD}/build/skia-shared/Debug"
cmake --build build/sdk-tests/Debug --config Debug --target tabengine_shell_test
ctest --test-dir build/sdk-tests/Debug -C Debug --output-on-failure
```

Run the tests that cover the code you touched; a renderer change needs
`tabengine_windows_renderer_test`, a drag change the drag tests, and so on.
Changes ready for consumers are validated end to end with
`python tools/sdk.py package --config Debug` and
`python tools/sdk.py verify --config Debug` (re-run for Release when both
configurations are affected).

## Code style

- C++20. Public API lives in `include/tabengine/` under `namespace tabengine`.
  Files are `snake_case`, types `PascalCase`, headers use `#pragma once`.
- The public headers `model.h`, `layout.h`, `platform.h`, `render.h`,
  `shell.h`, `text.h`, `theme.h`, and `types.h` stay platform-neutral: no
  Win32 types, no Skia includes except where a callback contract explicitly
  passes `SkCanvas`. `win32_platform.h` is the only Win32-facing public
  header.
- Match the density and idiom of the surrounding code; comments state
  constraints the code cannot show.
- Never copy source from Chromium, Skia, or any other project into this
  repository without checking the file's license and the notices it requires.

## Tests

- Tests are executables under `tests/`; a new test needs one
  `add_executable` + `add_test` pair in `CMakeLists.txt` alongside the
  existing ones (there is no automatic glob).
- Core and shell tests are headless and run anywhere Windows builds; the
  renderer test uses a hidden HWND and a real Skia raster surface. Do not add
  tests that require a visible window, interactive input, or wall-clock
  timing.

## Commit notes

Commits start with `feat:` / `fix:` / `docs:` (use `build:` / `test:` for
build-system and test-infrastructure changes), one line stating the behavior
change and motivation, written in English. Before pushing, confirm the
affected tests are green and, when the SDK contract changed, that `package`
and `verify` pass for the affected configurations.

## License and contribution terms

This repository is licensed under [Apache-2.0](LICENSE). By submitting, you
agree to publish your contribution under that license and need to attach
`Signed-off-by` to commits (`git commit -s`, i.e. the DCO Developer
Certificate of Origin); this project **does not require signing a CLA**.
Third-party components keep their upstream licenses — see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Where to get help

- Open an issue (bug reports and feature proposals, English or Chinese).
- Security issues: **do not** open a public issue — use the channels in
  [SECURITY.md](SECURITY.md).
- Community conduct: [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).
