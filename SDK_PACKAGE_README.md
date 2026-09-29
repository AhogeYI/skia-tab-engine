# TabEngine SDK

This is a binary Windows x64 SDK. You can build an application without the
TabEngine or Skia source repositories.

1. Read the [SDK user guide](docs/SDK_USER_GUIDE.md) for requirements,
   build commands, deployment, and a first application.
2. Build and run the included `examples/sdk_hello_tabs` CMake project.
3. Use the [API reference](docs/API_REFERENCE.md) with the installed
   `include/tabengine/*.h` headers when implementing your application.

`manifest.json` identifies the package version, configuration, pinned Skia
revision, and hashes of installed files. Debug and Release packages are
separate. `bin/skia.dll` must be deployed beside your application executable.

TabEngine is released under Apache-2.0: `LICENSE` and `NOTICE` sit at the SDK
root, and `THIRD_PARTY_NOTICES.md` maps every redistributed third-party
component (Skia, skcms, FreeType, libpng, zlib, D3D12 Memory Allocator,
SPIRV-Cross) to its license text under `licenses/`.
