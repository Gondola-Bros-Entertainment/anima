# Third-party dependencies and notices

## Vendored headers

Builds do not download these vendored files. Anima is C++20; the C libraries among
them compile as C++ and add no language runtime dependency.

| Library | Source revision | Files / license |
| --- | --- | --- |
| [cgltf 1.15](https://github.com/jkuhlmann/cgltf/tree/360db1a95480fe102ae9c69b27c5d101167ff5ba) | `360db1a95480fe102ae9c69b27c5d101167ff5ba` (peeled `v1.15` tag) | `cgltf/cgltf.h`, `cgltf/LICENSE` (MIT) |
| [nlohmann/json 3.12.0](https://github.com/nlohmann/json/tree/v3.12.0) | `v3.12.0` (unmodified single header) | `nlohmann/json.hpp`, `nlohmann/LICENSE.MIT` (MIT); private document parser |
| [stb_image 2.30](https://github.com/nothings/stb/tree/2c980bb59875b0d32144a71867fbdebb2f77cd20) | `2c980bb59875b0d32144a71867fbdebb2f77cd20` | `stb/stb_image.h`, `stb/LICENSE` (MIT or public domain; Anima uses MIT) |
| [doctest 2.5.3](https://github.com/doctest/doctest/tree/2d0a9359a60c51affe2a9bebb1be1dca47868151) | `2d0a9359a60c51affe2a9bebb1be1dca47868151` (`v2.5.3` tag) | `doctest/doctest/doctest.h`, `doctest/LICENSE.txt` (MIT); test runner only, never linked into engine targets |
| [Vulkan Memory Allocator 3.4.0](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/tree/3aa921224c154a0d2c43912bc88e1c42ce1f7607) | `3aa921224c154a0d2c43912bc88e1c42ce1f7607` (`v3.4.0` tag) | `vma/vk_mem_alloc.h`, `vma/LICENSE.txt` (MIT); private to `anima::desktop`, which compiles its implementation once |
| [miniaudio 0.11.25](https://github.com/mackron/miniaudio/tree/9634bedb5b5a2ca38c1ee7108a9358a4e233f14d) | `9634bedb5b5a2ca38c1ee7108a9358a4e233f14d` (`0.11.25` tag) | `miniaudio/miniaudio.h`, `miniaudio/LICENSE` (public domain or MIT No Attribution; Anima uses MIT No Attribution); private to `anima::core` |
| [stb_vorbis 1.22](https://github.com/mackron/miniaudio/tree/9634bedb5b5a2ca38c1ee7108a9358a4e233f14d/extras) | miniaudio's `extras/stb_vorbis.c` at the same revision | `miniaudio/stb_vorbis.c` (MIT or public domain, stated at the end of the file; Anima uses MIT); private to `anima::core` |

The cgltf, JSON, doctest, Vulkan Memory Allocator, miniaudio and stb_vorbis files
are unmodified. stb_image carries two local size-safety changes: 16-bit channel
conversion uses its checked allocation helper, and 8-bit PNG row copying reuses the
validated row byte count. The upstream revision and license remain unchanged; the
checksum below identifies the patched header.

JSON parsing is private to asset and component document implementations; public
APIs accept UTF-8 document strings. `anima::core` does not include or link the JSON
parser.

miniaudio decodes, mixes and plays `anima::core`'s audio, with stb_vorbis for Ogg
Vorbis. One translation unit, `src/runtime/miniaudio.cpp`, compiles both
implementations as C++ in the private `anima_audio_dependencies` library, from a
system include directory and without the project's warning flags, so their warnings
are not checked. That library's compile definitions reach every translation unit
that includes `miniaudio.h`, which keeps miniaudio's structure layouts consistent:
`MA_NO_RESOURCE_MANAGER`, `MA_NO_ENCODING` and `MA_NO_GENERATION` leave out what
Anima does not use, and on Apple platforms `MA_NO_RUNTIME_LINKING` links Core Audio
through the CoreFoundation, CoreAudio and AudioToolbox frameworks, the setting
miniaudio's documentation gives for Apple's notarization. Elsewhere miniaudio loads
its platform audio libraries at run time, and Linux links `pthread` and `dl`.

SHA-256:

```text
aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63  nlohmann/json.hpp
46a65cffd1ea955132d95a8dd921640714a8d6b537d2e4e482d31145ae95b603  nlohmann/LICENSE.MIT
e378a21c084bf1f288bb799de827bb26906efb024255f1ecf1705ea13f11c6ec  cgltf/cgltf.h
f619925f80ef862497aaf8e8155ef218fa6a2190055129523ca3df9119a9ba95  cgltf/LICENSE
41bdbf44fbb4eb34d1dcedace946053b81830d865fb451964300a9e8d25a0a0e  stb/stb_image.h
bebfe904b14301657e4e5d655c811d51fd31b97c455b9cc2d8600d6bac6cff63  stb/LICENSE
cfd518a3ef90f67e1f3ba514df23fb3627437de1a2feeba78cf5062a40021421  doctest/doctest/doctest.h
0fe0b331fa1513dcce8604ff1fa925f32d1cea17d8aeb1c2471fad40d291adc5  doctest/LICENSE.txt
8487b7995ad3b263eb73bc5b9a77d71aa69b6bef5d58a715c02d2663afd81f1a  vma/vk_mem_alloc.h
52df2c03d6cfc9ffec13c9d3626c530fc9ce0cbe41d5ea3d10cd46edeb1aeb38  vma/LICENSE.txt
ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89  miniaudio/miniaudio.h
457f1b500e0adf6bc059edddfa78a2f62012e7c3bb43476c20e0bd23b25ba0eb  miniaudio/LICENSE
4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636  miniaudio/stb_vorbis.c
```

## Optional dependencies

- [Jolt Physics](jolt/README.md): pinned source and [MIT license](jolt/LICENSE).
- [Box2D](box2d/README.md): pinned source and [MIT license](box2d/LICENSE).
- [RmlUi and FreeType](rmlui/README.md): source provenance, RmlUi's
  [MIT license](rmlui/LICENSE.txt), and the [FreeType License](rmlui/FreeType-FTL.txt).

SDL3 is found from an installed package by default. The optional CMake fetch is
pinned to SDL 3.4.16 commit `fa2c02bb6e21974a89ea9824bc53c9932abe5f9c`.
Vulkan and glslc come from the installed SDK.

## Test assets

The [UI fixture notes](../tests/ui/assets/README.md) record the upstream Lato font
and engine-authored checker image. The font's original
[notices and SIL Open Font License](../tests/ui/assets/LICENSE.txt) are retained
beside the font.
