# Optional UI dependency provenance

- RmlUi 6.3: <https://github.com/mikke89/RmlUi/releases/tag/6.3>
- Immutable source: `ba95ffe8bfb6370efb2cdcca927eaad4710c5413`
- Download: <https://codeload.github.com/mikke89/RmlUi/tar.gz/ba95ffe8bfb6370efb2cdcca927eaad4710c5413>
- Archive SHA-256: `1541ef5577115e9368f8ed389b29f0925ef6572f326a33d378ea16c3cfa2cde8`
- License: upstream MIT notice preserved in [LICENSE.txt](LICENSE.txt).

The optional build uses RmlUi Core, with its SDL platform helper for desktop UI.
Samples, Lua, SVG/Lottie, debugger use and third-party container implementations
are disabled. RmlUi is configured only when `ANIMA_BUILD_UI_DOCUMENTS` or
`ANIMA_BUILD_UI` is enabled.

[RmlUiBoolString.cmake](../../cmake/patches/RmlUiBoolString.cmake) changes the
boolean-to-string assignment to a single-character assignment. This preserves
the conversion while avoiding GCC's optimized `-Wrestrict` diagnostic; no warning
is suppressed. The fetched archive is patched automatically. Source overrides
must supply this pinned revision with the same patch applied.

The default font engine links the consumer's FreeType package.
We use the FreeType License option; its unmodified text from the upstream
`VER-2-14-3` source is preserved in [FreeType-FTL.txt](FreeType-FTL.txt).
This software is based in part on the work of the FreeType Team.
Portions of this software are copyright © 2026 The FreeType Project
(https://freetype.org). All rights reserved.
Distributors must retain the applicable notices and package their selected
FreeType build's runtime libraries and any enabled transitive dependencies.
Anima does not bundle or install a system FreeType binary.

The test font is an unmodified `LatoLatin-Regular.ttf` from this same RmlUi source.
Its upstream font notices, including the Lato SIL Open Font License 1.1 and copyright,
are preserved beside it in `tests/ui/assets/LICENSE.txt`. The game chooses and
licenses its own shipped fonts; the test font is not an implicit game asset.
