#!/usr/bin/env python3
"""Regenerates the block-compressed texture fixtures in this directory; see README.md.

Needs Python 3 with Pillow, and basisu 2.50 (Homebrew formula basis_universal) and ImageMagick 7 (magick) on PATH.
"""
import os
import shutil
import struct
import subprocess
import tempfile

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK = 145, 146
KHR_DF_MODEL_BC7 = 134
KHR_DF_TRANSFER_LINEAR, KHR_DF_TRANSFER_SRGB = 1, 2
KHR_DF_PRIMARIES_BT709 = 1


def pattern(width, height):
    """RGBA texels with smooth ramps, a fine checker, edges and graded and binary alpha, so that the BC7 packer
    uses many of its modes and partitions. A fixed linear congruential generator adds noise."""
    image = Image.new('RGBA', (width, height))
    texels = image.load()
    seed = 12345
    for y in range(height):
        for x in range(width):
            seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
            noise = (seed >> 16) % 24
            u, v = x * 255 // max(width - 1, 1), y * 255 // max(height - 1, 1)
            quadrant = (x * 2 // width) + 2 * (y * 2 // height)
            if quadrant == 0:  # Smooth ramps.
                texel = (u, v, 255 - u // 2, 255)
            elif quadrant == 1:  # A two-texel checker of saturated colors with noise.
                on = (x // 2 + y // 2) % 2
                texel = (230 if on else 20 + noise, 40 + noise, 200 if not on else 60, 255)
            elif quadrant == 2:  # Graded alpha over a hue ramp.
                texel = (v, 255 - u, (u + v) // 2, (u * 3 + noise) % 256)
            else:  # A disc with a hard alpha edge and colored rings.
                dx, dy = x - width * 3 // 4, y - height * 3 // 4
                inside = dx * dx + dy * dy < (min(width, height) // 5) ** 2
                ring = (dx * dx + dy * dy) // 16 % 3
                texel = ((255, 64, 32), (32, 255, 64), (64, 32, 255))[ring] + ((255 if inside else 0),)
            texels[x, y] = texel
    return image


def opaque(width, height):
    """Opaque texels that vary smoothly, as an albedo map does: color ramps under a soft radial highlight, with a
    little noise. Frames drawn from it are compared with frames drawn from its source, within the encoding's error,
    so it avoids the zero alpha, where an encoder may give texels any color, and the hard edges, where BC7's error
    is largest, of the pattern fixtures."""
    image = Image.new('RGBA', (width, height))
    texels = image.load()
    seed = 67890
    for y in range(height):
        for x in range(width):
            seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
            noise = (seed >> 16) % 5 - 2
            u, v = x / (width - 1), y / (height - 1)
            highlight = max(0.0, 1 - ((u - 0.35) ** 2 + (v - 0.4) ** 2) * 4)
            texel = (40 + 180 * u + 30 * highlight, 60 + 150 * v + 40 * highlight, 200 - 140 * u * v + 20 * highlight)
            texels[x, y] = tuple(max(0, min(255, round(channel) + noise)) for channel in texel) + (255,)
    return image


def dfd(transfer):
    """A KTX2 data format descriptor with one basic block describing BC7 with @p transfer."""
    sample = struct.pack('<HBBBBBBII', 0, 127, 0, 0, 0, 0, 0, 0, 0xFFFFFFFF)
    block = struct.pack('<IHH', 0, 2, 24 + len(sample))
    block += bytes([KHR_DF_MODEL_BC7, KHR_DF_PRIMARIES_BT709, transfer, 0, 3, 3, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0])
    block += sample
    return struct.pack('<I', 4 + len(block)) + block


def dds_levels(path):
    """Width, height and each mip level's blocks of a BC7 DX10 DDS file."""
    data = open(path, 'rb').read()
    assert data[:4] == b'DDS '
    height, width, _, _, levels = struct.unpack_from('<IIIII', data, 12)
    assert data[84:88] == b'DX10'
    levels = max(levels, 1)
    offset, result = 148, []
    for level in range(levels):
        w, h = max(width >> level, 1), max(height >> level, 1)
        size = ((w + 3) // 4) * ((h + 3) // 4) * 16
        result.append(data[offset:offset + size])
        offset += size
    assert offset == len(data)
    return width, height, result


def write_ktx2(path, width, height, levels, vk_format, transfer):
    """A KTX 2.0 file of @p levels (base level first), without supercompression or key/value data. Level data are
    stored smallest first, each aligned to 16 bytes, as the specification requires."""
    identifier = b'\xabKTX 20\xbb\r\n\x1a\n'
    descriptor = dfd(transfer)
    header_bytes = 12 + 9 * 4 + 4 * 4 + 2 * 8 + len(levels) * 24
    dfd_offset = header_bytes
    offset = dfd_offset + len(descriptor)
    placed = [None] * len(levels)
    body = b''
    for level in reversed(range(len(levels))):
        padding = (-offset) % 16
        body += b'\0' * padding
        offset += padding
        placed[level] = (offset, len(levels[level]))
        body += levels[level]
        offset += len(levels[level])
    header = identifier + struct.pack('<9I', vk_format, 1, width, height, 0, 0, 1, len(levels), 0)
    header += struct.pack('<IIIIQQ', dfd_offset, len(descriptor), 0, 0, 0, 0)
    for start, size in placed:
        header += struct.pack('<QQQ', start, size, size)
    assert len(header) == header_bytes
    with open(path, 'wb') as file:
        file.write(header + descriptor + body)


def decoded_atlas(dds, work, name):
    """Every level of @p dds decoded by basisu, side by side from the base level, after checking that ImageMagick
    decodes the whole blocks of the base level identically. (ImageMagick 7.1.2-22 misplaces the texels of partial
    blocks at the right edge of an image whose width is not a multiple of 4.)"""
    subprocess.run(['basisu', '-unpack', dds, '-output_path', work, '-no_ktx'], check=True, capture_output=True)
    width, height, levels = dds_levels(dds)
    # basisu names each level's file after its level only when the file has several.
    level_names = [f'_level_{level}' for level in range(len(levels))] if len(levels) > 1 else ['']
    decoded = [Image.open(os.path.join(work, f'{name}_unpacked_rgba_RGBA32{level}_face_0_layer_0000.png'))
               .convert('RGBA') for level in level_names]
    magick = os.path.join(work, f'{name}-magick.png')
    subprocess.run(['magick', dds, magick], check=True)
    whole = (0, 0, width // 4 * 4, height // 4 * 4)
    assert Image.open(magick).convert('RGBA').crop(whole).tobytes() == decoded[0].crop(whole).tobytes(), \
        f'basisu and ImageMagick decode {name} differently'
    atlas = Image.new('RGBA', (sum(level.width for level in decoded), height))
    x = 0
    for level in decoded:
        atlas.paste(level, (x, 0))
        x += level.width
    return atlas


# Anchor texels of each partition: texel 0, then the anchors of the other subsets, from the Khronos Data Format
# Specification's tables of BPTC anchor index values.
ANCHOR_2 = [15] * 16 + [15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2, 15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2,
                        15, 15, 6, 6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15]
ANCHOR_3_2 = [3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3, 3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5, 15, 15,
              8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15, 3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3,
              3]
ANCHOR_3_3 = [15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8, 15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6, 10, 15,
              15, 10, 8, 15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8, 15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15,
              15, 3, 15, 15, 8]
ANCHORS = {2: [{0, a} for a in ANCHOR_2], 3: [{0, a, b} for a, b in zip(ANCHOR_3_2, ANCHOR_3_3)]}


class Bits:
    """A BC7 block written from its least significant bit up."""

    def __init__(self):
        self.value, self.count = 0, 0

    def put(self, value, bits):
        assert 0 <= value < (1 << bits)
        self.value |= value << self.count
        self.count += bits

    def block(self):
        assert self.count == 128
        return self.value.to_bytes(16, 'little')


def partition_blocks():
    """One mode 1 block for each two-subset partition and one mode 2 block for each three-subset partition, with
    endpoints and indices from a fixed linear congruential generator, so that a decoder's partition and anchor
    tables decide every texel. Each subset's anchor texel stores one index bit fewer, as the Khronos Data Format
    Specification's anchor tables say."""
    seed = 2024

    def draw(bits):
        nonlocal seed
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        return (seed >> 8) % (1 << bits)

    blocks = []
    for mode, subsets, color_bits, index_bits in ((1, 2, 6, 3), (2, 3, 5, 2)):
        for partition in range(64):
            bits = Bits()
            bits.put(1 << mode, mode + 1)
            bits.put(partition, 6)
            for channel in range(3):
                for subset in range(subsets):
                    for endpoint in range(2):
                        bits.put(draw(color_bits), color_bits)
            if mode == 1:
                bits.put(draw(1), 1)
                bits.put(draw(1), 1)
            anchors = ANCHORS[subsets][partition]
            for texel in range(16):
                width = index_bits - (1 if texel in anchors else 0)
                bits.put(draw(width), width)
            blocks.append(bits.block())
    return blocks


def write_dds(path, width, height, blocks, dxgi_format):
    """A DX10 DDS file of one level of BC7 @p blocks, for basisu and ImageMagick to decode."""
    header = struct.pack('<4s7I44x', b'DDS ', 124, 0x81007, height, width, len(blocks) * 16, 0, 1)
    header += struct.pack('<2I4s5I', 32, 0x4, b'DX10', 0, 0, 0, 0, 0)
    header += struct.pack('<5I', 0x1000, 0, 0, 0, 0)
    header += struct.pack('<5I', dxgi_format, 3, 0, 1, 0)
    assert len(header) == 148
    with open(path, 'wb') as file:
        file.write(header + b''.join(blocks))


def main():
    with tempfile.TemporaryDirectory() as work:
        # The linear fixtures encode their sources' texels as data, as a normal or metallic-roughness map would be.
        for name, source, (width, height), linear in (('pattern', 'pattern', (64, 64), False),
                                                      ('pattern-odd', 'pattern-odd', (30, 18), False),
                                                      ('pattern-linear', 'pattern', (64, 64), True),
                                                      ('opaque', 'opaque', (64, 64), False),
                                                      ('opaque-linear', 'opaque', (64, 64), True)):
            if not linear:
                (opaque if source == 'opaque' else pattern)(width, height).save(os.path.join(HERE, f'{source}.png'))
            staged = os.path.join(work, f'{name}.png')
            shutil.copyfile(os.path.join(HERE, f'{source}.png'), staged)
            dds = os.path.join(work, f'{name}.dds')
            subprocess.run(['basisu', '-dds', '-dds_format', 'BC7', '-mipmap', '-dds_bc7e_scalar_level', '6'] +
                           (['-linear'] if linear else []) + ['-output_file', dds, staged],
                           check=True, capture_output=True)
            width, height, levels = dds_levels(dds)
            write_ktx2(os.path.join(HERE, f'{name}-bc7.ktx2'), width, height, levels,
                       VK_FORMAT_BC7_UNORM_BLOCK if linear else VK_FORMAT_BC7_SRGB_BLOCK,
                       KHR_DF_TRANSFER_LINEAR if linear else KHR_DF_TRANSFER_SRGB)
            decoded_atlas(dds, work, name).save(os.path.join(HERE, f'{name}-bc7-decoded.png'))
        # 128 blocks, 16 across, in the order partition_blocks() makes them.
        blocks = partition_blocks()
        width, height = 64, 32
        rows = [b''.join(blocks[row * 16 + column] for column in range(16)) for row in range(8)]
        dds = os.path.join(work, 'partitions.dds')
        write_dds(dds, width, height, [rows[row][i:i + 16] for row in range(8) for i in range(0, 256, 16)], 98)
        write_ktx2(os.path.join(HERE, 'partitions-bc7.ktx2'), width, height, [b''.join(rows)],
                   VK_FORMAT_BC7_UNORM_BLOCK, KHR_DF_TRANSFER_LINEAR)
        decoded_atlas(dds, work, 'partitions').save(os.path.join(HERE, 'partitions-bc7-decoded.png'))


main()
