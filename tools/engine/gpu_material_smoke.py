#!/usr/bin/env python3
"""Render generated textured quads and check per-pixel sRGB, filtering, wrapping and minification."""

import argparse
import json
import math
import os
from pathlib import Path
import struct
import zlib
from gpu_smoke import discard_captures, run
from gpu_preview_smoke import read_ppm, changed


def fixture(
    path, wrap, linear=False, mipmapped=False, *, mag_filter=None, min_filter=None, uv_extent=None
):
    size, uv_max = (64, 64) if mipmapped else (2, 2)
    if uv_extent is not None:
        uv_max = uv_extent

    def chunk(kind, data):
        return (
            struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
        )

    rows = b"".join(
        b"\0" + bytes(128 * ((x + y) % 2) for x in range(size) for _ in range(3))
        for y in range(size)
    )
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(rows))
        + chunk(b"IEND", b"")
    )
    vertices = b"".join(
        struct.pack("<8f", x, y, 0, 0, 0, 1, (x + 1) * uv_max / 2, (y + 1) * uv_max / 2)
        for x, y in [(-1, -1), (1, -1), (1, 1), (-1, -1), (1, 1), (-1, 1)]
    )
    binary = vertices + png
    doc = {
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "buffers": [{"byteLength": len(binary)}],
        "bufferViews": [
            {"buffer": 0, "byteLength": len(vertices), "byteStride": 32},
            {"buffer": 0, "byteOffset": len(vertices), "byteLength": len(png)},
        ],
        "accessors": [
            {"bufferView": 0, "byteOffset": offset, "componentType": 5126, "count": 6, "type": kind}
            for offset, kind in [(0, "VEC3"), (12, "VEC3"), (24, "VEC2")]
        ],
        "images": [{"bufferView": 1, "mimeType": "image/png"}],
        "textures": [{"source": 0, "sampler": 0}],
        "samplers": [
            {
                "wrapS": wrap,
                "wrapT": wrap,
                "magFilter": mag_filter if mag_filter is not None else (9729 if linear else 9728),
                "minFilter": min_filter
                if min_filter is not None
                else (9987 if mipmapped else (9729 if linear else 9728)),
            }
        ],
        "materials": [
            {
                "pbrMetallicRoughness": {
                    "baseColorTexture": {"index": 0},
                    "metallicFactor": 0,
                    "roughnessFactor": 1,
                }
            }
        ],
        "meshes": [
            {
                "primitives": [
                    {"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "material": 0}
                ]
            }
        ],
    }
    encoded = json.dumps(doc, separators=(",", ":")).encode()
    encoded += b" " * (-len(encoded) % 4)
    binary += b"\0" * (-len(binary) % 4)
    path.write_bytes(
        struct.pack("<III", 0x46546C67, 2, 28 + len(encoded) + len(binary))
        + struct.pack("<II", len(encoded), 0x4E4F534A)
        + encoded
        + struct.pack("<II", len(binary), 0x004E4942)
        + binary
    )


def sample(image, u, v):
    # Project an interior point of the quad through the documented default orbit camera.
    width, height, pixels = image
    sy, cy, sp, cp = math.sin(0.3), math.cos(0.3), math.sin(0.12), math.cos(0.12)
    x, y = u - 1, v - 1
    depth = 3 * math.sqrt(2) - sy * cp * x - sp * y
    px = int((1 + (1 + math.sqrt(2)) / (width / height) * cy * x / depth) * width / 2)
    py = int((1 - (1 + math.sqrt(2)) * (-sy * sp * x + cp * y) / depth) * height / 2)
    offset = (py * width + px) * 3
    return list(pixels[offset : offset + 3])


def expected_rgb(fraction, point):
    linear_gray = ((128 / 255 + 0.055) / 1.055) ** 2.4

    # Independent closed form for a roughness=1 dielectric: D=1/pi,
    # correlated Smith visibility=1/(2*(N.L+N.V)).
    def unit(v):
        size = math.sqrt(sum(x * x for x in v))
        return [x / size for x in v]

    distance = 3 * math.sqrt(2)
    eye = [
        distance * math.sin(0.3) * math.cos(0.12),
        distance * math.sin(0.12),
        distance * math.cos(0.3) * math.cos(0.12),
    ]
    view = unit([eye[0] - (point[0] - 1), eye[1] - (point[1] - 1), eye[2]])
    light = unit([-0.6, 0.9, 0.8])
    half = unit([a + b for a, b in zip(view, light)])
    fresnel = 0.04 + 0.96 * (1 - sum(a * b for a, b in zip(view, half))) ** 5
    albedo = linear_gray * fraction
    value = (
        0.4 * albedo
        + 0.08 * 0.04
        + 0.5 * light[2] * ((1 - fresnel) * albedo + fresnel / (2 * (light[2] + view[2])))
    )
    return round(
        255 * (12.92 * value if value <= 0.0031308 else 1.055 * value ** (1 / 2.4) - 0.055)
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--output", type=Path, default=Path("build/verification/material"))
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    os.environ.setdefault("VK_LAYER_VALIDATE_SYNC", "1")
    points = [(0.25, 0.25), (0.75, 0.25), (1.25, 0.25), (1.75, 0.25), (0.25, 0.75)]
    cases = [
        ("nearest-repeat", 10497, False, False, points, [0, 1, 0, 1, 1]),
        ("nearest-clamp", 33071, False, False, points, [0, 1, 1, 1, 1]),
        ("nearest-mirror", 33648, False, False, points, [0, 1, 1, 0, 1]),
        ("linear-clamp", 33071, True, False, [(0.5, 0.25), (0.5, 0.5)], [0.5, 0.5]),
        ("mip-minification", 10497, True, True, points, [0.5] * 5),
    ]
    records = []
    for name, wrap, linear, mipmapped, probes, fractions in cases:
        glb, ppm = output / f"{name}.glb", output / f"{name}.ppm"
        fixture(glb, wrap, linear, mipmapped)
        ppm.unlink(missing_ok=True)
        run(
            args.binary.resolve(),
            ["--asset", str(glb), "--validation", "--frames", "2", "--capture", str(ppm)],
            output,
            name,
        )
        image = read_ppm(ppm)
        samples = [sample(image, *point) for point in probes]
        expected = [expected_rgb(fraction, point) for fraction, point in zip(fractions, probes)]
        assert all(
            len(rgb) == 3 and max(abs(c - target) for c in rgb) <= 4
            for rgb, target in zip(samples, expected)
        ), (name, samples, expected)
        records.append(
            {"case": name, "uv_probes": probes, "actual_rgb": samples, "expected_gray": expected}
        )
    # Dense UVs force minification while retaining only mip level zero. Matching
    # filter references were qualified analytically above; mixed samplers must
    # reproduce their min-filter reference, independent of their mag filter.
    references = {}
    for name, mag, minimum in [
        ("min-reference-nearest", 9728, 9728),
        ("min-reference-linear", 9729, 9729),
        ("min-linear-mag-nearest", 9728, 9729),
        ("min-nearest-mag-linear", 9729, 9728),
    ]:
        glb, ppm = output / f"{name}.glb", output / f"{name}.ppm"
        fixture(glb, 10497, mag_filter=mag, min_filter=minimum, uv_extent=1024)
        ppm.unlink(missing_ok=True)
        run(
            args.binary.resolve(),
            ["--asset", str(glb), "--validation", "--frames", "2", "--capture", str(ppm)],
            output,
            name,
        )
        rendered = read_ppm(ppm)
        if mag == minimum:
            references[minimum] = rendered
        else:
            assert rendered == references[minimum], f"{name}: minification used the wrong filter"
            records.append(
                {"case": name, "matches_minification_reference": True, "mipmapped": False}
            )
    assert changed(references[9728], references[9729]) > 0.01, (
        "Filter references do not discriminate minification"
    )
    (output / "summary.json").write_text(json.dumps({"cases": records}, indent=2) + "\n")
    print(
        "PASS per-pixel sRGB, nearest/linear, wrapping, mip minification and independent min/mag filters"
    )

    discard_captures(output)


if __name__ == "__main__":
    main()
