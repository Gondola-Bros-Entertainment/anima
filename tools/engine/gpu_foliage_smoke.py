#!/usr/bin/env python3
"""Check actual minified coverage and shared-image material isolation on the GPU."""

import argparse
import json
import os
from pathlib import Path
from gpu_smoke import discard_captures, run
from gpu_preview_smoke import read_ppm


def square(image, size):
    width, height, data = image
    assert (width, height) == (512, 512), (width, height)
    offset = (512 - size) // 2
    return [
        tuple(data[(y * width + x) * 3 : (y * width + x) * 3 + 3])
        for y in range(offset, offset + size)
        for x in range(offset, offset + size)
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("consumer", type=Path)
    parser.add_argument("--output", type=Path, default=Path("build/verification/foliage"))
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob("*.ppm"):
        old.unlink()
    os.environ.setdefault("VK_LAYER_VALIDATE_SYNC", "1")
    run(args.consumer.resolve(), ["--foliage", str(out)], out, "foliage", timeout=60)
    images = {path.stem: read_ppm(path) for path in out.glob("*.ppm")}
    records = []
    for size, expected in [(256, 22 / 64), (128, 5 / 16)]:
        pixels = square(images[f"{size}-0"], size)
        visible = [p for p in pixels if min(p) > 245]
        fraction = len(visible) / len(pixels)
        assert abs(fraction - expected) < 0.001, (size, fraction, expected)
        # Different material cutoffs need separate mips; both recover the same
        # footprint for this fixture. Opaque/emissive uses retain ordinary RGB.
        assert images[f"{size}-0"] == images[f"{size}-1"], (
            "Cutoff variants lost their common footprint"
        )
        assert images[f"{size}-2"] == images[f"{size}-3"], "Mask processing leaked into emission"
        opaque = square(images[f"{size}-2"], size)
        assert sum(p[1] for p in opaque) / len(opaque) < 180, (
            "Transparent magenta RGB was removed from opaque use"
        )
        records.append(
            {"size_pixels": size, "visible_fraction": fraction, "expected_fraction": expected}
        )
    assert images["resource-mask"] == images["reference-mask"] == images["restored-mask"], (
        "Scene path/replacement mismatch"
    )
    (out / "summary.json").write_text(
        json.dumps({"coverage": records, "validation_errors": 0}, indent=2) + "\n"
    )
    print(
        "PASS: minified coverage, distinct cutoffs, ordinary RGB/emission isolation and scene parity"
    )

    discard_captures(out)


if __name__ == "__main__":
    main()
