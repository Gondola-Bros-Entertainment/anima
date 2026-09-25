#!/usr/bin/env python3
"""Check independent instance updates in the out-of-tree desktop consumer."""

import argparse
import hashlib
import json
import os
from pathlib import Path
from gpu_preview_smoke import read_ppm, changed
from gpu_smoke import discard_captures, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("consumer", type=Path)
    parser.add_argument("--asset", type=Path)
    parser.add_argument("--output", type=Path, default=Path("build/verification/instances"))
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    os.environ.setdefault("VK_LAYER_VALIDATE_SYNC", "1")
    cases = [("synthetic", [])]
    if args.asset:
        cases.append(("external-asset", [str(args.asset.resolve(strict=True))]))
    records = []
    for name, extra in cases:
        directory = output / name
        directory.mkdir(exist_ok=True)
        for filename in [
            "consumer-start.ppm",
            "consumer-partial.ppm",
            "consumer-overlay.ppm",
            "consumer-updated.ppm",
            "consumer-unloaded.ppm",
            "consumer-cleared.ppm",
            "consumer-orthographic.ppm",
            "consumer-camera-moved.ppm",
            "consumer-camera-restored.ppm",
            "consumer-camera-rejected.ppm",
        ]:
            (directory / filename).unlink(missing_ok=True)
        log = run(args.consumer.resolve(strict=True), [str(directory), *extra], output, name)
        assert "110 frames, 10 captures, scene cameras and multi-scene partial unload" in log
        assert (directory / "consumer-camera-restored.ppm").read_bytes() == (
            directory / "consumer-updated.ppm"
        ).read_bytes(), "Persisted camera replacement changed the rendered view"
        assert (directory / "consumer-camera-rejected.ppm").read_bytes() == (
            directory / "consumer-camera-restored.ppm"
        ).read_bytes(), "Rejected camera selection changed the renderer"
        orthographic = read_ppm(directory / "consumer-orthographic.ppm")
        assert changed(orthographic, read_ppm(directory / "consumer-updated.ppm")) > 0.005
        assert changed(orthographic, read_ppm(directory / "consumer-camera-moved.ppm")) > 0.005
        assert (directory / "consumer-unloaded.ppm").read_bytes() == (
            directory / "consumer-cleared.ppm"
        ).read_bytes(), "Retained unloaded scene rendered differently from an empty renderer"
        assert (directory / "consumer-partial.ppm").read_bytes() == (
            directory / "consumer-overlay.ppm"
        ).read_bytes(), "Unloading one scene changed the surviving scene"
        assert (directory / "consumer-partial.ppm").read_bytes() != (
            directory / "consumer-unloaded.ppm"
        ).read_bytes(), "Surviving scene did not render"
        start = read_ppm(directory / "consumer-start.ppm")
        updated = read_ppm(directory / "consumer-updated.ppm")
        assert start[:2] == updated[:2]
        width, height = start[:2]
        right = lambda image: b"".join(
            image[2][(y * width + width // 2) * 3 : (y + 1) * width * 3] for y in range(height)
        )
        assert right(start) == right(updated), (
            "Updating the left instance changed the right half of the image"
        )
        fraction = changed(start, updated)
        assert fraction > 0.005, (name, fraction)
        background = start[2][:3]
        right_pixels = right(start)
        occupied = sum(
            max(abs(a - b) for a, b in zip(right_pixels[i : i + 3], background)) > 20
            for i in range(0, len(right_pixels), 3)
        )
        assert occupied > width * height * 0.01, "The unchanged right instance was not visible"
        records.append(
            {
                "case": name,
                "presented_frames": 110,
                "captures": 10,
                "persisted_camera_matches_original": True,
                "rejected_selection_preserves_pixels": True,
                "right_half_identical": True,
                "unloaded_matches_empty": True,
                "partial_unload_matches_survivor": True,
                "right_foreground_pixels": occupied,
                "changed_fraction": fraction,
                "validation_warnings": 0,
                "validation_errors": 0,
                "capture_sha256": {
                    p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in directory.glob("*.ppm")
                },
            }
        )
    (output / "summary.json").write_text(json.dumps({"runs": records}, indent=2) + "\n")
    print(
        f"PASS {len(records)} external GPU consumer cases: independent instance pose/appearance and scene unload"
    )

    discard_captures(output)


if __name__ == "__main__":
    main()
