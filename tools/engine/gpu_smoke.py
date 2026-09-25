#!/usr/bin/env python3
"""Bounded GPU/lifecycle regression runner; no display automation or third-party Python packages."""

import argparse
import json
import os
import re
from pathlib import Path
import subprocess


def run(binary, args, output, name, expected=0, timeout=35):
    result = subprocess.run([str(binary), *args], text=True, capture_output=True, timeout=timeout)
    log = result.stdout + result.stderr
    (output / f"{name}.log").write_text(log)
    if result.returncode != expected:
        raise RuntimeError(f"{name}: exit {result.returncode}, expected {expected}\n{log}")
    if (
        "[Vulkan validation]" in log
        or "validation_errors=0" not in log
        or "validation_warnings=0" not in log
    ):
        raise RuntimeError(f"{name}: validation/cleanup failed\n{log}")
    print(f"PASS {name}", flush=True)
    return log


def discard_captures(output):
    """Remove readbacks after their pixel checks and summaries are complete."""
    for capture in output.rglob("*.ppm"):
        capture.unlink()


def verify_triangle(path):
    with path.open("rb") as stream:
        assert stream.readline() == b"P6\n", "Capture must be PPM RGB"
        width, height = map(int, stream.readline().split())
        assert stream.readline() == b"255\n"
        pixels = stream.read()
    assert len(pixels) == width * height * 3

    def pixel(x, y):
        offset = (y * width + x) * 3
        return tuple(pixels[offset : offset + 3])

    background = pixel(0, 0)
    center = pixel(width // 2, height // 2)
    assert max(background) < 80 and min(center) > 80, (background, center)
    changed = sum(
        max(abs(a - b) for a, b in zip(pixels[i : i + 3], background)) > 20
        for i in range(0, len(pixels), 3)
    )
    fraction = changed / (width * height)
    assert 0.07 < fraction < 0.4, f"Triangle coverage {fraction:.3f}"
    return {
        "width": width,
        "height": height,
        "background_rgb": background,
        "center_rgb": center,
        "triangle_coverage": fraction,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument(
        "--asset", type=Path, help="Optional external-asset GLB for the viewer regression"
    )
    parser.add_argument("--output", type=Path, default=Path("build/verification/smoke"))
    options = parser.parse_args()
    binary = options.binary.resolve(strict=True)
    os.environ.setdefault("VK_LAYER_VALIDATE_SYNC", "1")
    output = options.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    summary = {
        "binary": str(binary),
        "driver_override": os.environ.get("VK_DRIVER_FILES"),
        "runs": [],
    }
    for name, extra in [("lifecycle", []), ("fallback", ["--no-present-fences"])]:
        capture = output / f"{name}.ppm"
        # Prevent stale artifacts from passing a new run.
        capture.unlink(missing_ok=True)
        log = run(binary, ["--smoke", "--capture", str(capture), *extra], output, name)
        records = [
            json.loads(line.removeprefix("RESULT "))
            for line in log.splitlines()
            if line.startswith("RESULT ")
        ]
        assert len(records) == 1 and records[0]["passed"], log
        record = {"name": name, **records[0], "capture": verify_triangle(capture)}
        summary["runs"].append(record)
    for stage in ["instance", "surface", "device", "resources", "swapchain"]:
        log = run(
            binary,
            ["--validation", "--frames", "1", "--fail-after", stage],
            output,
            f"failure-{stage}",
            expected=1,
        )
        assert f"Injected initialization failure after {stage}" in log, log
        summary["runs"].append(
            {"name": f"failure-{stage}", "expected_failure": True, "cleanup_clean": True}
        )
    if options.asset:
        capture = output / "external-asset.ppm"
        capture.unlink(missing_ok=True)
        log = run(
            binary,
            [
                "--smoke",
                "--asset",
                str(options.asset.resolve(strict=True)),
                "--capture",
                str(capture),
            ],
            output,
            "external-asset",
        )
        counts = dict(
            (key, int(value))
            for key, value in re.findall(r"\b(mesh_nodes|primitives|triangles)=(\d+)", log)
        )
        assert all(counts.get(key, 0) > 0 for key in ("mesh_nodes", "primitives", "triangles")), log
        record = next(
            json.loads(line.removeprefix("RESULT "))
            for line in log.splitlines()
            if line.startswith("RESULT ")
        )
        assert record["passed"] and record["camera_updates"] >= 60, record
        with capture.open("rb") as image:
            assert image.readline() == b"P6\n"
            width, height = map(int, image.readline().split())
            assert image.readline() == b"255\n"
            pixels = image.read()
        assert len(pixels) == width * height * 3
        background = pixels[:3]
        foreground = [
            i // 3
            for i in range(0, len(pixels), 3)
            if max(abs(a - b) for a, b in zip(pixels[i : i + 3], background)) > 20
        ]
        coverage = len(foreground) / (width * height)
        assert 0.015 < coverage < 0.4, coverage
        assert (
            max(i // width for i in foreground) - min(i // width for i in foreground)
            > height * 0.45
        )
        assert len({pixels[i * 3 : i * 3 + 3] for i in foreground}) > 100
        summary["runs"].append({"name": "external-asset", **record, "coverage": coverage})
        run(
            binary,
            [
                "--validation",
                "--frames",
                "1",
                "--asset",
                str(options.asset.resolve()),
                "--fail-after",
                "resources",
            ],
            output,
            "failure-mesh-resources",
            expected=1,
        )
        summary["runs"].append(
            {"name": "failure-mesh-resources", "expected_failure": True, "cleanup_clean": True}
        )
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(
        f"All {len(summary['runs'])} GPU checks passed. Evidence: {output / 'summary.json'}",
        flush=True,
    )

    discard_captures(output)


if __name__ == "__main__":
    main()
