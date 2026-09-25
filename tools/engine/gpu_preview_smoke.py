#!/usr/bin/env python3
"""Verify actual rendered animation, pause, restart and bind restoration on one local GPU."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
from gpu_smoke import discard_captures, run


def fixture(output):
    """Generate a skinned quad with two clips; no external art export is required."""
    binary = bytearray()
    views, accessors = [], []

    def accessor(values, components, kind, fmt="f", component_type=5126):
        binary.extend(b"\0" * (-len(binary) % 4))
        data = struct.pack("<" + fmt * len(values), *values)
        views.append({"buffer": 0, "byteOffset": len(binary), "byteLength": len(data)})
        binary.extend(data)
        accessors.append(
            {
                "bufferView": len(views) - 1,
                "componentType": component_type,
                "count": len(values) // components,
                "type": kind,
            }
        )
        return len(accessors) - 1

    positions = accessor([-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, -1, 0, 1, 1, 0, -1, 1, 0], 3, "VEC3")
    accessors[positions].update(min=[-1, -1, 0], max=[1, 1, 0])
    normals = accessor([0, 0, 1] * 6, 3, "VEC3")
    joints = accessor([0, 0, 0, 0] * 6, 4, "VEC4", "H", 5123)
    weights = accessor([1, 0, 0, 0] * 6, 4, "VEC4")
    inverse_bind = accessor([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1], 16, "MAT4")
    animations = []
    for name, duration, travel in [("Idle", 2, 0.15), ("Walk", 4, 0.9)]:
        times = accessor([0, duration / 2, duration], 1, "SCALAR")
        accessors[times].update(min=[0], max=[duration])
        translations = accessor([0, 0, 0, travel, 0, 0, 0, 0, 0], 3, "VEC3")
        animations.append(
            {
                "name": name,
                "samplers": [{"input": times, "output": translations}],
                "channels": [{"sampler": 0, "target": {"node": 1, "path": "translation"}}],
            }
        )
    doc = {
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"children": [1, 2]}, {"name": "joint"}, {"mesh": 0, "skin": 0}],
        "skins": [{"joints": [1], "inverseBindMatrices": inverse_bind}],
        "meshes": [
            {
                "primitives": [
                    {
                        "attributes": {
                            "POSITION": positions,
                            "NORMAL": normals,
                            "JOINTS_0": joints,
                            "WEIGHTS_0": weights,
                        },
                        "material": 0,
                    }
                ]
            }
        ],
        "materials": [
            {"pbrMetallicRoughness": {"baseColorFactor": [0.3, 0.6, 0.9, 1], "metallicFactor": 0}}
        ],
        "animations": animations,
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"byteLength": len(binary)}],
    }
    encoded = json.dumps(doc, separators=(",", ":")).encode()
    encoded += b" " * (-len(encoded) % 4)
    binary.extend(b"\0" * (-len(binary) % 4))
    output.joinpath("preview.glb").write_bytes(
        struct.pack("<III", 0x46546C67, 2, 28 + len(encoded) + len(binary))
        + struct.pack("<II", len(encoded), 0x4E4F534A)
        + encoded
        + struct.pack("<II", len(binary), 0x004E4942)
        + binary
    )
    manifest = output / "preview.asset.json"
    manifest.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "units": "meters",
                "asset_id": "test.preview",
                "model": "preview.glb",
                "skeleton": {"id": "test.rig", "joint_count": 1, "bind_signature": "0" * 64},
                "clips": [{"name": name, "loop": True, "events": []} for name in ("Idle", "Walk")],
                "equipment": [],
            }
        )
        + "\n"
    )
    return manifest


def read_ppm(path):
    with path.open("rb") as image:
        assert image.readline() == b"P6\n"
        width, height = map(int, image.readline().split())
        assert image.readline() == b"255\n"
        data = image.read()
    assert len(data) == width * height * 3
    return width, height, data


def changed(a, b):
    assert a[:2] == b[:2]
    pixels = sum(
        max(abs(x - y) for x, y in zip(a[2][i : i + 3], b[2][i : i + 3])) > 3
        for i in range(0, len(a[2]), 3)
    )
    return pixels / (a[0] * a[1])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument(
        "manifest", type=Path, nargs="?", help="Optional external manifest with Idle and Walk clips"
    )
    parser.add_argument("--output", type=Path, default=Path("build/verification/preview"))
    args = parser.parse_args()
    binary, output = args.binary.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    manifest = args.manifest.resolve() if args.manifest else fixture(output)
    # Only this runner's generated frames; never allow previous frames to satisfy a new run.
    for path in output.glob("*.ppm"):
        path.unlink()
    os.environ.setdefault("VK_LAYER_VALIDATE_SYNC", "1")
    log = run(
        binary,
        ["--manifest", str(manifest), "--preview-smoke", str(output), "--timeout", "45"],
        output,
        "animated-preview",
        timeout=55,
    )
    result = next(
        json.loads(line.removeprefix("RESULT "))
        for line in log.splitlines()
        if line.startswith("RESULT ")
    )
    assert result["passed"] and result["frames"] == 360 and result["captures"] == 28, result
    pairs = [
        ("idle_start", "idle_mid", 0.0001),
        ("walk_start", "walk_mid", 0.001),
        ("walk_early", "walk_swing", 0.001),
    ]
    differences = {}
    for a, b, minimum in pairs:
        fraction = changed(read_ppm(output / f"{a}.ppm"), read_ppm(output / f"{b}.ppm"))
        assert fraction > minimum, (a, b, fraction)
        differences[f"{a} / {b}"] = fraction
    assert (output / "walk_paused.ppm").read_bytes() == (
        output / "walk_paused_again.ppm"
    ).read_bytes(), "Paused pose moved"
    assert (output / "walk_swing.ppm").read_bytes() == (output / "walk_restart.ppm").read_bytes(), (
        "Restart did not reproduce pose"
    )
    sequence = sorted(output.glob("stride_[0-9][0-9][0-9].ppm"))
    assert (
        len(sequence) == 17
        and len({hashlib.sha256(p.read_bytes()).hexdigest() for p in sequence}) >= 14
    )
    assert (output / "bind_start.ppm").read_bytes() == (output / "bind_later.ppm").read_bytes(), (
        "Bind pose moved"
    )
    report = {
        "render": result,
        "pixel_change_fraction": differences,
        "paused_identical": True,
        "restart_identical": True,
        "stride_sequence_frames": len(sequence),
        "captures": {
            p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in output.glob("*.ppm")
        },
    }
    (output / "preview-summary.json").write_text(json.dumps(report, indent=2) + "\n")
    print("PASS animated poses, pause, restart and bind restoration; 28 GPU captures")

    discard_captures(output)


if __name__ == "__main__":
    main()
