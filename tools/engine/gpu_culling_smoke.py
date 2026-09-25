#!/usr/bin/env python3
"""Compare exact images with resource frustum culling enabled and disabled."""

import argparse
import json
import os
from pathlib import Path
from gpu_preview_smoke import read_ppm
from gpu_replacement_smoke import clear, foreground
from gpu_smoke import discard_captures, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("consumer", type=Path)
    parser.add_argument("--asset", type=Path)
    parser.add_argument("--output", type=Path, default=Path("build/verification/culling"))
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    captures = output / "captures"
    captures.mkdir(exist_ok=True)
    for path in captures.glob("*.ppm"):
        path.unlink()
    os.environ.setdefault("VK_LAYER_VALIDATE_SYNC", "1")
    options = ["--culling", str(captures)]
    if args.asset:
        options += ["--asset", str(args.asset.resolve(strict=True))]
    log = run(args.consumer.resolve(strict=True), options, output, "culling", timeout=120)
    assert "PASS culling" in log
    cases = [
        json.loads(line.removeprefix("CASE "))
        for line in log.splitlines()
        if line.startswith("CASE ")
    ]
    assert len(cases) >= 11
    for case in cases:
        name = case["name"]
        off = read_ppm(captures / f"{name}-off.ppm")
        on = read_ppm(captures / f"{name}-on.ppm")
        assert off == on, f"Culling changed rendered pixels: {name}"
        case["pixels_identical"] = True
    foreground(read_ppm(captures / "visible-on.ppm"))
    foreground(read_ppm(captures / "returned-on.ppm"))
    clear(read_ppm(captures / "all-outside-on.ppm"))
    clear(read_ppm(captures / "hierarchy-inactive-on.ppm"))
    assert read_ppm(captures / "hierarchy-reactivated-on.ppm") == read_ppm(
        captures / "visible-on.ppm"
    )
    assert any(case["culled_draws"] for case in cases)
    result = {"cases": cases, "validation_warnings": 0, "validation_errors": 0}
    (output / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"PASS {len(cases)} culling on/off pairs with identical pixels; no validation issues")

    discard_captures(output)


if __name__ == "__main__":
    main()
