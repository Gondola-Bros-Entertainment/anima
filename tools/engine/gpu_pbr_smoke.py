#!/usr/bin/env python3
"""Verify scalar metallic/roughness shading on synthetic geometry; render a material study."""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import struct
from gpu_smoke import discard_captures, run
from gpu_preview_smoke import read_ppm
from gpu_material_smoke import sample


def unit(v):
    length = math.sqrt(sum(x * x for x in v))
    return [x / length for x in v]


def write_glb(path, shapes):
    binary = bytearray()
    views, accessors, primitives, materials = [], [], [], []
    for vertices, metallic, roughness, color in shapes:
        first = len(binary)
        for vertex in vertices:
            binary.extend(struct.pack("<6f", *vertex))
        view = len(views)
        views.append(
            {"buffer": 0, "byteOffset": first, "byteLength": len(binary) - first, "byteStride": 24}
        )
        accessor = len(accessors)
        positions = [vertex[:3] for vertex in vertices]
        accessors.extend(
            [
                {
                    "bufferView": view,
                    "componentType": 5126,
                    "count": len(vertices),
                    "type": "VEC3",
                    "min": [min(v[k] for v in positions) for k in range(3)],
                    "max": [max(v[k] for v in positions) for k in range(3)],
                },
                {
                    "bufferView": view,
                    "byteOffset": 12,
                    "componentType": 5126,
                    "count": len(vertices),
                    "type": "VEC3",
                },
            ]
        )
        primitives.append(
            {
                "attributes": {"POSITION": accessor, "NORMAL": accessor + 1},
                "material": len(materials),
            }
        )
        materials.append(
            {
                "name": f"metal={metallic} rough={roughness}",
                "pbrMetallicRoughness": {
                    "baseColorFactor": [*color, 1],
                    "metallicFactor": metallic,
                    "roughnessFactor": roughness,
                },
            }
        )
    doc = {
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "buffers": [{"byteLength": len(binary)}],
        "bufferViews": views,
        "accessors": accessors,
        "materials": materials,
        "meshes": [{"primitives": primitives}],
    }
    encoded = json.dumps(doc, separators=(",", ":")).encode()
    encoded += b" " * (-len(encoded) % 4)
    path.write_bytes(
        struct.pack("<III", 0x46546C67, 2, 28 + len(encoded) + len(binary))
        + struct.pack("<II", len(encoded), 0x4E4F534A)
        + encoded
        + struct.pack("<II", len(binary), 0x004E4942)
        + binary
    )


def sphere(center, rings=40, segments=64):
    def vertex(i, j):
        theta, phi = math.pi * i / rings, 2 * math.pi * j / segments
        n = [math.sin(theta) * math.cos(phi), math.cos(theta), math.sin(theta) * math.sin(phi)]
        return [c + x for c, x in zip(center, n)] + n

    result = []
    for i in range(rings):
        for j in range(segments):
            # Outward winding; avoid degenerate pole triangles.
            if i > 0:
                result.extend([vertex(i, j), vertex(i, j + 1), vertex(i + 1, j)])
            if i < rings - 1:
                result.extend([vertex(i, j + 1), vertex(i + 1, j + 1), vertex(i + 1, j)])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--output", type=Path, default=Path("build/verification/pbr"))
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    os.environ.setdefault("VK_LAYER_VALIDATE_SYNC", "1")

    def render(name, shapes):
        glb, ppm = output / (name + ".glb"), output / (name + ".ppm")
        write_glb(glb, shapes)
        ppm.unlink(missing_ok=True)
        run(
            binary,
            ["--asset", str(glb), "--validation", "--frames", "2", "--capture", str(ppm)],
            output,
            name,
        )
        return read_ppm(ppm)

    # A plane with its shading normal halfway between the known light and
    # default camera exposes the highlight; changing only roughness must affect it.
    light = unit([-0.6, 0.9, 0.8])
    view = unit([math.sin(0.3) * math.cos(0.12), math.sin(0.12), math.cos(0.3) * math.cos(0.12)])
    normal = unit([a + b for a, b in zip(light, view)])
    quad = [[x, y, 0, *normal] for x, y in [(-1, -1), (1, -1), (1, 1), (-1, -1), (1, 1), (-1, 1)]]
    records = {}
    for name, metallic, roughness, color in [
        ("black-rough", 0, 1, [0, 0, 0]),
        ("black-smooth", 0, 0.25, [0, 0, 0]),
        ("copper-dielectric", 0, 1, [0.7, 0.25, 0.06]),
        ("copper-metal", 1, 1, [0.7, 0.25, 0.06]),
        ("zero-roughness", 1, 0, [0.7, 0.25, 0.06]),
    ]:
        image = render(name, [(quad, metallic, roughness, color)])
        records[name] = sample(image, 1, 1)
    assert min(records["black-smooth"]) > max(records["black-rough"]) + 60, records
    assert min(records["black-rough"]) > 0, "Black dielectric must retain a specular reflection"
    assert max(records["black-smooth"]) - min(records["black-smooth"]) <= 1, (
        "Dielectric highlight must be neutral"
    )
    assert records["copper-dielectric"][0] > records["copper-metal"][0] + 20, (
        "Metal must lose diffuse reflection"
    )
    assert records["copper-metal"][0] > records["copper-metal"][1] > records["copper-metal"][2], (
        "Metal reflection must be tinted"
    )
    grid = []
    for metallic, y in [(0, 1.3), (1, -1.3)]:
        for x, roughness in [(-2.5, 0.2), (0, 0.5), (2.5, 0.9)]:
            grid.append((sphere([x, y, 0]), metallic, roughness, [0.7, 0.25, 0.06]))
    render("material-study", grid)
    (output / "summary.json").write_text(
        json.dumps(
            {
                "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                "center_rgb": records,
                "study": "Top: dielectric. Bottom: metal. Left to right roughness 0.2, 0.5, 0.9.",
                "passed": True,
            },
            indent=2,
        )
        + "\n"
    )
    print(
        "PASS roughness response, neutral dielectric specular, tinted metal, zero roughness and material study"
    )

    discard_captures(output)


if __name__ == "__main__":
    main()
