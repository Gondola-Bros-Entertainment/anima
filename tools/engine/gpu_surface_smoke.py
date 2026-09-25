#!/usr/bin/env python3
"""Check glTF surface maps against equivalent constant/normal reference assets."""

import argparse
import json
import math
import os
from pathlib import Path
import struct
import zlib
from gpu_smoke import discard_captures, run
from gpu_preview_smoke import read_ppm
from gpu_material_smoke import sample


def srgb(byte):
    x = byte / 255
    return x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4


def fixture(path, kind, reference=False, authored=True):
    rgba = {
        "normal": (218, 128, 218, 255),
        "surface": (40, 96, 192, 255),
        "emissive": (128, 180, 80, 255),
        "shared": (218, 128, 218, 255),
        "unlit": (128, 180, 80, 255),
    }[kind]
    n = [0, 0, 1]
    if reference and kind in ("normal", "shared"):
        n = [x / 255 * 2 - 1 for x in rgba[:3]]
        length = math.sqrt(sum(x * x for x in n))
        n = [x / length for x in n]
    pbr = {"baseColorFactor": [0.4, 0.5, 0.3, 1], "metallicFactor": 0.2, "roughnessFactor": 0.7}
    material = {"pbrMetallicRoughness": pbr}
    if kind in ("normal", "shared") and not reference:
        material["normalTexture"] = {"index": 0}
    if kind == "surface":
        if reference:
            pbr["metallicFactor"] *= rgba[2] / 255
            pbr["roughnessFactor"] *= rgba[1] / 255
        else:
            pbr["metallicRoughnessTexture"] = {"index": 0}
    if kind == "emissive":
        material["emissiveFactor"] = [0.35, 0.25, 0.45]
        if reference:
            material["emissiveFactor"] = [
                f * srgb(c) for f, c in zip(material["emissiveFactor"], rgba)
            ]
        else:
            material["emissiveTexture"] = {"index": 0}
    if kind in ("shared", "unlit"):
        if reference:
            pbr["baseColorFactor"] = [
                f * srgb(c) for f, c in zip(pbr["baseColorFactor"][:3], rgba)
            ] + [1]
        else:
            pbr["baseColorTexture"] = {"index": 0}
    if kind == "unlit":
        material["extensions"] = {"KHR_materials_unlit": {}}
    binary = bytearray()
    for x, y in [(-1, -1), (1, -1), (1, 1), (-1, -1), (1, 1), (-1, 1)]:
        binary.extend(struct.pack("<12f", x, y, 0, *n, (x + 1) / 2, (y + 1) / 2, 1, 0, 0, 1))
    vertices = len(binary)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(b"\0" + bytes(rgba)))
        + chunk(b"IEND", b"")
    )
    binary.extend(png)
    attributes = {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}
    if authored:
        attributes["TANGENT"] = 3
    doc = {
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "buffers": [{"byteLength": len(binary)}],
        "bufferViews": [
            {"buffer": 0, "byteLength": vertices, "byteStride": 48},
            {"buffer": 0, "byteOffset": vertices, "byteLength": len(png)},
        ],
        "accessors": [
            {"bufferView": 0, "byteOffset": offset, "componentType": 5126, "count": 6, "type": t}
            for offset, t in [(0, "VEC3"), (12, "VEC3"), (24, "VEC2"), (32, "VEC4")]
        ],
        "images": [{"bufferView": 1, "mimeType": "image/png"}],
        "textures": [{"source": 0}],
        "materials": [material],
        "meshes": [{"primitives": [{"attributes": attributes, "material": 0}]}],
    }
    if kind == "unlit":
        doc.update(
            extensionsUsed=["KHR_materials_unlit"], extensionsRequired=["KHR_materials_unlit"]
        )
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--output", type=Path, default=Path("build/verification/surface"))
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    os.environ.setdefault("VK_LAYER_VALIDATE_SYNC", "1")
    records = []
    for kind in ("normal", "surface", "emissive", "shared", "unlit"):
        images = []
        for variant, reference, authored in [
            ("reference", True, True),
            ("mapped", False, True),
            ("derivatives", False, False),
        ]:
            name = kind + "-" + variant
            glb = output / (name + ".glb")
            ppm = output / (name + ".ppm")
            fixture(glb, kind, reference, authored)
            ppm.unlink(missing_ok=True)
            run(
                args.binary.resolve(),
                ["--asset", str(glb), "--validation", "--frames", "2", "--capture", str(ppm)],
                output,
                name,
            )
            images.append(read_ppm(ppm))
        for actual in images[1:]:
            mean = sum(abs(a - b) for a, b in zip(images[0][2], actual[2])) / len(actual[2])
            assert mean < 0.15, (kind, mean)
            assert (
                max(abs(a - b) for a, b in zip(sample(images[0], 1, 1), sample(actual, 1, 1))) <= 2
            ), (kind, "center")
        records.append(
            {"case": kind, "center_rgb": [sample(i, 1, 1) for i in images], "passed": True}
        )
    (output / "summary.json").write_text(json.dumps(records, indent=2) + "\n")
    print(
        "PASS: authored/derivative normal frames, linear G/B surface maps, sRGB emission, mixed texture encodings and required unlit extension"
    )

    discard_captures(output)


if __name__ == "__main__":
    main()
