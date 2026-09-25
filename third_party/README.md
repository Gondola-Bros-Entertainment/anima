# Third-party dependencies and notices

## Vendored headers

Builds do not download these vendored files. Anima is C++20;
the two small C-compatible parsing/decoding libraries compile as C++ and add no
language runtime dependency.

| Library | Source revision | Files / license |
| --- | --- | --- |
| [cgltf 1.15](https://github.com/jkuhlmann/cgltf/tree/360db1a95480fe102ae9c69b27c5d101167ff5ba) | `360db1a95480fe102ae9c69b27c5d101167ff5ba` (peeled `v1.15` tag) | `cgltf/cgltf.h`, `cgltf/LICENSE` (MIT) |
| [nlohmann/json 3.12.0](https://github.com/nlohmann/json/tree/v3.12.0) | `v3.12.0` (unmodified single header) | `nlohmann/json.hpp`, `nlohmann/LICENSE.MIT` (MIT); private document parser |
| [stb_image 2.30](https://github.com/nothings/stb/tree/2c980bb59875b0d32144a71867fbdebb2f77cd20) | `2c980bb59875b0d32144a71867fbdebb2f77cd20` | `stb/stb_image.h`, `stb/LICENSE` (MIT or public domain; Anima uses MIT) |

The cgltf and JSON files are unmodified. stb_image carries two local size-safety
changes: 16-bit channel conversion uses its checked allocation helper, and 8-bit
PNG row copying reuses the validated row byte count. The upstream revision and
license remain unchanged; the checksum below identifies the patched header.

JSON parsing is private to asset and component document implementations; public
APIs accept UTF-8 document strings. `anima::core` does not include or link the JSON
parser.

SHA-256:

```text
aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63  nlohmann/json.hpp
46a65cffd1ea955132d95a8dd921640714a8d6b537d2e4e482d31145ae95b603  nlohmann/LICENSE.MIT
e378a21c084bf1f288bb799de827bb26906efb024255f1ecf1705ea13f11c6ec  cgltf/cgltf.h
f619925f80ef862497aaf8e8155ef218fa6a2190055129523ca3df9119a9ba95  cgltf/LICENSE
41bdbf44fbb4eb34d1dcedace946053b81830d865fb451964300a9e8d25a0a0e  stb/stb_image.h
bebfe904b14301657e4e5d655c811d51fd31b97c455b9cc2d8600d6bac6cff63  stb/LICENSE
```

## Optional dependencies

- [Jolt Physics](jolt/README.md): pinned source and [MIT license](jolt/LICENSE).
- [Box2D](box2d/README.md): pinned source and [MIT license](box2d/LICENSE).
- [RmlUi and FreeType](rmlui/README.md): source provenance, RmlUi's
  [MIT license](rmlui/LICENSE.txt), and the [FreeType License](rmlui/FreeType-FTL.txt).

SDL3 is found from an installed package by default. The optional CMake fetch is
pinned to SDL 3.4.2 commit `683181b47cfabd293e3ea409f838915b8297a4fd`.
Vulkan and glslc come from the installed SDK.

## Test assets

The [UI fixture notes](../tests/ui/assets/README.md) record the upstream Lato font
and engine-authored checker image. The font's original
[notices and SIL Open Font License](../tests/ui/assets/LICENSE.txt) are retained
beside the font.
