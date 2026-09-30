# Block-compressed texture fixtures

Engine-authored BC7 images in KTX 2.0 files, with their sources and reference decodes,
for `anima_texture_compression_tests` and `consumer_desktop --compressed-textures`.
`make_textures.py` made every file here and remakes them identically:

```sh
python3 tests/textures/make_textures.py
```

It needs Python 3 with Pillow (12.1.1 was used), and on `PATH` basisu 2.50.0 (Homebrew
formula `basis_universal`) and ImageMagick 7.1.2-22 (`magick`).

| Files | Contents |
| --- | --- |
| `pattern.png`, `pattern-odd.png` | 64×64 and 30×18 RGBA sources whose ramps, checker, edges and graded and binary alpha make the encoder use every BC7 mode |
| `opaque.png` | A 64×64 opaque source that varies smoothly, as an albedo map does, for comparing drawn frames with their source |
| `*-bc7.ktx2` | Each source encoded with its full mip chain, as `VK_FORMAT_BC7_SRGB_BLOCK`, or as `VK_FORMAT_BC7_UNORM_BLOCK` for `pattern-linear` and `opaque-linear`, which encode `pattern.png` and `opaque.png` as data |
| `partitions-bc7.ktx2` | 64×32 texels of synthetic blocks: one of mode 1 for each two-subset partition and one of mode 2 for each three-subset partition |
| `*-bc7-decoded.png` | Every level of the KTX2 file of the same name, decoded, side by side from the base level |

For each image the script runs `basisu -dds -dds_format BC7 -mipmap
-dds_bc7e_scalar_level 6` (with `-linear` for the linear files), which writes a DDS file
with the bc7e encoder, and copies its levels into a KTX 2.0 file with one basic data
format descriptor block and no supercompression or key/value data. The synthetic
blocks are written by the script itself. The reference decodes are `basisu -unpack`'s
RGBA32 output for the DDS file; ImageMagick decodes the whole blocks of every base
level identically, which the script checks. (ImageMagick 7.1.2-22 misplaces the texels
of partial blocks at the right edge of an image whose width is not a multiple of 4.)

These files are original to Anima and share its [Apache-2.0](../../LICENSE) license.
