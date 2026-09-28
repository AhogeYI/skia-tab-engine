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
