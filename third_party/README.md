# Third-party dependencies and notices

## Vendored sources

Builds do not download these vendored files. Anima is C++20; the C libraries among
them compile as C++ and add no language runtime dependency.

| Library | Source revision | Files / license |
| --- | --- | --- |
| [cgltf 1.15](https://github.com/jkuhlmann/cgltf/tree/360db1a95480fe102ae9c69b27c5d101167ff5ba) | `360db1a95480fe102ae9c69b27c5d101167ff5ba` (peeled `v1.15` tag) | `cgltf/cgltf.h`, `cgltf/LICENSE` (MIT) |
| [nlohmann/json 3.12.0](https://github.com/nlohmann/json/tree/v3.12.0) | `v3.12.0` (single header, one local change) | `nlohmann/json.hpp`, `nlohmann/LICENSE.MIT` (MIT); private document parser |
| [stb_image 2.30](https://github.com/nothings/stb/tree/2c980bb59875b0d32144a71867fbdebb2f77cd20) | `2c980bb59875b0d32144a71867fbdebb2f77cd20` | `stb/stb_image.h`, `stb/LICENSE` (MIT or public domain; Anima uses MIT) |
| [doctest 2.5.3](https://github.com/doctest/doctest/tree/2d0a9359a60c51affe2a9bebb1be1dca47868151) | `2d0a9359a60c51affe2a9bebb1be1dca47868151` (`v2.5.3` tag) | `doctest/doctest/doctest.h`, `doctest/LICENSE.txt` (MIT); test runner only, never linked into engine targets |
| [Vulkan Memory Allocator 3.4.0](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/tree/3aa921224c154a0d2c43912bc88e1c42ce1f7607) | `3aa921224c154a0d2c43912bc88e1c42ce1f7607` (`v3.4.0` tag) | `vma/vk_mem_alloc.h`, `vma/LICENSE.txt` (MIT); private to `anima::desktop`, which compiles its implementation once |
| [meshoptimizer 1.3](https://github.com/zeux/meshoptimizer/tree/9e1f07b159d3cb777f1c67ed31fc11fd117986f4) | `9e1f07b159d3cb777f1c67ed31fc11fd117986f4` (`v1.3` tag) | `meshoptimizer/meshoptimizer.h`, `meshoptimizer/allocator.cpp`, `meshoptimizer/indexgenerator.cpp`, `meshoptimizer/simplifier.cpp`, `meshoptimizer/vcacheoptimizer.cpp`, `meshoptimizer/LICENSE.md` (MIT); private to `anima::assets`, which simplifies mesh draws into levels of detail and orders each level's triangles for the vertex cache |
| [miniaudio 0.11.25](https://github.com/mackron/miniaudio/tree/9634bedb5b5a2ca38c1ee7108a9358a4e233f14d) | `9634bedb5b5a2ca38c1ee7108a9358a4e233f14d` (`0.11.25` tag) | `miniaudio/miniaudio.h`, `miniaudio/LICENSE` (public domain or MIT No Attribution; Anima uses MIT No Attribution); private to `anima::core` |
| [stb_vorbis 1.22](https://github.com/mackron/miniaudio/tree/9634bedb5b5a2ca38c1ee7108a9358a4e233f14d/extras) | miniaudio's `extras/stb_vorbis.c` at the same revision | `miniaudio/stb_vorbis.c` (MIT or public domain, stated at the end of the file; Anima uses MIT); private to `anima::core` |

The cgltf, doctest, meshoptimizer, Vulkan Memory Allocator and stb_vorbis files are unmodified. Of meshoptimizer,
Anima keeps only the simplifier, the position remap and vertex cache ordering that its levels of detail use, and the
allocator they use, from the release's `src` directory.
stb_image carries two local size-safety changes: 16-bit channel conversion uses its
checked allocation helper, and 8-bit PNG row copying reuses the validated row byte
count. The JSON header carries one local thread-safety change: its lexer and
serializer read the locale's decimal point and thousands separator through one
function, `detail::locale_character`, which calls `localeconv()` under a lock,
because glibc's `localeconv()` rewrites one static buffer on every call and documents
concurrent calls as a data race. That function replaces the lexer's
`get_decimal_point` body and the serializer's `loc` member. Anima compiles its copy
in the inline namespace `nlohmann::json_anima_v3_12_0` (`src/detail/json.hpp`), so an
application's own copy of the release shares none of its definitions. miniaudio
carries two local changes that make spatial mixing agree across targets. `ma_rsqrtf`,
which normalizes the listener's axes, computes an exact reciprocal square root;
upstream uses SSE's `rsqrtss`, an approximation to about 12 bits that placed sources
slightly nearer on x86, so that one at its maximum distance stayed faintly audible.
The stereo gain ramp's paths that step two frames at a time, taken with SSE2 and by
MSVC without it, advance two frames' gain per step and give an odd last frame the
gain that follows; upstream advances one frame's gain and gives that frame the ramp's
starting gain, so a ramp covered half its change and the rest arrived at once in the
next block. Anima mixes in stereo, so the six-channel path, which steps the same way,
is left as upstream. The upstream revisions and licenses remain unchanged; the
checksums below identify the patched headers.

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
Besides miniaudio's functions, `src/runtime/audio.cpp` uses a few fields of its
structures directly: a sound's processing cache count and resampler when a voice
seeks, its spatializer and the engine's listener when a voice starts, and a buffer
reference's sample rate. An update must check that they still mean the same.

SHA-256:

```text
e9128ea8c9a9cdc3f58a7c673cebbade052e07f6a478b0101457fbd90d2ec7b0  nlohmann/json.hpp
46a65cffd1ea955132d95a8dd921640714a8d6b537d2e4e482d31145ae95b603  nlohmann/LICENSE.MIT
e378a21c084bf1f288bb799de827bb26906efb024255f1ecf1705ea13f11c6ec  cgltf/cgltf.h
f619925f80ef862497aaf8e8155ef218fa6a2190055129523ca3df9119a9ba95  cgltf/LICENSE
41bdbf44fbb4eb34d1dcedace946053b81830d865fb451964300a9e8d25a0a0e  stb/stb_image.h
bebfe904b14301657e4e5d655c811d51fd31b97c455b9cc2d8600d6bac6cff63  stb/LICENSE
cfd518a3ef90f67e1f3ba514df23fb3627437de1a2feeba78cf5062a40021421  doctest/doctest/doctest.h
0fe0b331fa1513dcce8604ff1fa925f32d1cea17d8aeb1c2471fad40d291adc5  doctest/LICENSE.txt
8487b7995ad3b263eb73bc5b9a77d71aa69b6bef5d58a715c02d2663afd81f1a  vma/vk_mem_alloc.h
52df2c03d6cfc9ffec13c9d3626c530fc9ce0cbe41d5ea3d10cd46edeb1aeb38  vma/LICENSE.txt
e3688ef553f603391747895a5c221ef1528574cc7f8173782417a96cb531f61b  meshoptimizer/meshoptimizer.h
d2cc48691fe2f4c6d097bf7a766389cfb7f83ca14a5f747e3944332656d02254  meshoptimizer/allocator.cpp
f362cdcd8af10016333bf699347a9364335987f413f061e5b7e8cafe3b498948  meshoptimizer/indexgenerator.cpp
719303b6970398631e30724e8eebc6cd2e7e08b67fe8ab7cbf7eecafdaa08616  meshoptimizer/simplifier.cpp
618429ef4db8ab9b16fde4e73dcf425d89452acbf103085478bc8b1761fa6689  meshoptimizer/vcacheoptimizer.cpp
f03037ca7bad1e3eb7f4a63fa6084a8baabd5ba30d3c239a9a7f35705d873e26  meshoptimizer/LICENSE.md
1760f0ca949e885e971600406ee223616c701c0bf6920584571cff85570b4f75  miniaudio/miniaudio.h
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
beside the font. The [audio fixture notes](../tests/audio/assets/README.md) record
how the engine-authored tones were generated. The
[texture fixture notes](../tests/textures/README.md) record how the engine-authored
BC7 images and their reference decodes were made with basisu and ImageMagick; no
code from either enters Anima.
