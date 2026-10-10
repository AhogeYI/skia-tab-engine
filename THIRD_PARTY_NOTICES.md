# Third-party components and licenses

TabEngine itself is released under [Apache-2.0](LICENSE). This file lists the
third-party components that TabEngine builds against and redistributes, the
license that applies to each, and where the license text lives. TabEngine is
an original implementation; the licenses below belong to its dependencies and
never to TabEngine's own code.

TabEngine vendors nlohmann/json source in `third_party/json/`. The pinned
Skia revision (`third_party/SKIA_REVISION.txt`) and the externals it declares
in Skia's `DEPS` are fetched into the gitignored `third_party/skia/` checkout
by `tools/sdk.py`. Every SDK package therefore redistributes binary copies of
the components below and carries their license texts in its `licenses/`
directory. Skia source builds collect their component notices into
`build/skia-shared/<config>/licenses/`. SDK installation adds the JSON notice;
the packaging step also collects the MCP dependency inventory described below.

## Distributed with the SDK packages and Skia builds

| Component | License | License text in the package |
| --- | --- | --- |
| Skia | BSD-3-Clause | `licenses/skia/LICENSE.txt` |
| skcms | BSD-3-Clause | `licenses/skcms/LICENSE.txt` |
| FreeType 2 | FreeType License (FTL) or GPL-2.0-or-later, dual-licensed; this project distributes it under the FTL | `licenses/freetype/LICENSE.TXT` |
| libpng | PNG Reference Library License version 2 | `licenses/libpng/LICENSE.txt` |
| zlib | Zlib | `licenses/zlib/LICENSE.txt` |
| D3D12 Memory Allocator | MIT | `licenses/d3d12allocator/LICENSE.txt`, `licenses/d3d12allocator/NOTICES.txt` |
| SPIRV-Cross | Apache-2.0 or Khronos Free Use, dual-licensed | `licenses/spirv-cross/LICENSE.txt`, `licenses/spirv-cross/LicenseRef-KhronosFreeUse.txt` |
| JSON for Modern C++ (nlohmann/json) 3.11.3 | MIT | `licenses/nlohmann-json/LICENSE.MIT`; compiled into `lib/tabengine_automation.lib`. |

All of these are permissive licenses compatible with TabEngine's Apache-2.0.
Binary redistribution must retain the copyright, license, and disclaimer
notices above; the SDK packaging does this automatically. When Skia features
or externals change, re-audit the license bundle instead of assuming an old
package's notices cover newly enabled code. FreeType is offered by its
authors under two mutually exclusive licenses; the FTL text in
`LICENSE.TXT` is the one this distribution selects.

DirectWrite, D3D12, DXGI, and the other system libraries the Windows build
links are operating-system components, not redistributed dependencies, and
are outside this list.

## MCP adapter dependencies

The SDK includes the MCP adapter's production npm dependencies, installed
from `tools/mcp/package-lock.json`. They are separate from the Skia binaries:
their source and original license files remain in `tools/mcp/node_modules/`.
The direct dependencies are `@modelcontextprotocol/sdk` (MIT) and `zod` (MIT).
Transitive dependencies use MIT, ISC, BSD-2-Clause, or BSD-3-Clause licenses
in the current lockfile.

Every SDK contains `tools/mcp/THIRD_PARTY_LICENSES.json`, a generated inventory
of the installed packages' names, exact versions, declared licenses, and
relative paths to their license/notice files. Packaging and verification fail
when a required dependency or license file is missing, empty, or inconsistent
with this inventory. Keep the inventory, lockfile, and dependency directories
together when redistributing the tools. Node.js itself is not bundled.
