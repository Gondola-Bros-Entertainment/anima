#!/usr/bin/env python3
"""Compare shared-resource GPU skinning with the expanded CPU reference."""

import argparse
import json
import os
from pathlib import Path
from gpu_preview_smoke import changed, read_ppm
from gpu_replacement_smoke import clear, foreground
from gpu_smoke import discard_captures, run


def parity(reference, actual):
    assert reference[:2] == actual[:2]
    a, b = reference[2], actual[2]
    mean = sum(abs(x - y) for x, y in zip(a, b)) / len(a)
    large = sum(
        max(abs(x - y) for x, y in zip(a[i : i + 3], b[i : i + 3])) > 16
        for i in range(0, len(a), 3)
    ) / (reference[0] * reference[1])
    # Floating-point skinning/rasterization can move an edge by a pixel. Broad
    # lighting, texture, pose or seam changes must still fail this comparison.
    assert mean < 0.5 and large < 0.002, (mean, large)
    return {"mean_channel_error_255": mean, "large_pixel_difference_fraction": large}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("consumer", type=Path)
    parser.add_argument("--asset", type=Path)
    parser.add_argument("--hide", default="")
    parser.add_argument("--output", type=Path, default=Path("build/verification/resource"))
    parser.add_argument("--fatal", action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    os.environ.setdefault("VK_LAYER_VALIDATE_SYNC", "1")
    records = []
    cases = [("", False)]
    if args.fatal:
        cases += [
            (stage, prepare)
            for prepare in (False, True)
            for stage in ("upload-timeout", "device-lost")
        ]
    for fatal, prepare in cases:
        name = ("prepare-" if prepare else "") + (fatal or "parity")
        directory = output / name
        directory.mkdir(exist_ok=True)
        for path in directory.glob("*.ppm"):
            path.unlink()
        options = ["--resources", str(directory)]
        if args.asset:
            options += ["--asset", str(args.asset.resolve(strict=True))]
        if args.hide:
            options += ["--hide", args.hide]
        if fatal:
            options += ["--prepare-fatal" if prepare else "--fatal", fatal]
        log = run(args.consumer.resolve(strict=True), options, output, name, timeout=120)
        if fatal:
            assert f"PASS resource fatal classification {fatal}" in log
            records.append({"injected_fatal": fatal, "preparation": prepare, "cleanup_clean": True})
            continue
        result = next(
            json.loads(line.removeprefix("RESULT "))
            for line in log.splitlines()
            if line.startswith("RESULT ")
        )
        images = {p.stem: read_ppm(p) for p in directory.glob("*.ppm")}
        assert images["preloaded-reference"] == images["reference"], (
            "Preparation changed the selected scene"
        )
        foreground(images["gpu"])
        clear(images["empty"])
        result["parity"] = {
            "initial": parity(images["reference"], images["gpu"]),
            "updated": parity(images["reference-updated"], images["gpu-updated"]),
            "visibility": parity(images["reference-hidden"], images["gpu-hidden"]),
            "resized": parity(images["reference-resized"], images["gpu-resized"]),
        }
        assert images["gpu-hidden-instance"] == images["gpu-hidden"]
        assert changed(images["gpu"], images["gpu-hidden"]) > 0.001
        assert images["gpu-resized"][:2] != images["gpu"][:2]
        for clip in range(result["clip_captures"]):
            result["parity"][f"clip-{clip}"] = parity(
                images[f"reference-clip-{clip}"], images[f"gpu-clip-{clip}"]
            )
        for same in [
            "after-churn",
            "rollback-vertex",
            "rollback-index",
            "rollback-texture",
            "rollback-texture-upload",
            "rollback-descriptors",
            "rollback-ready",
            "rollback-palette",
        ]:
            assert images[same] == images["gpu"], f"{same} did not preserve accepted image"
        for stage in ("vertex", "index", "texture", "texture-upload", "descriptors", "ready"):
            assert images[f"preload-rollback-{stage}"] == images["gpu"], (
                f"Preparation failure changed pixels: {stage}"
            )
        assert images["gpu-restored-scene"] == images["gpu-updated"]
        assert changed(images["gpu"], images["gpu-updated"]) > 0.001
        result["rollback_pixels_identical"] = True
        result["membership_reuses_mesh"] = True
        records.append(result)
    (output / "summary.json").write_text(json.dumps({"runs": records}, indent=2) + "\n")
    print(f"PASS resource parity, membership, rollback and {len(records) - 1} injected fatal cases")

    discard_captures(output)


if __name__ == "__main__":
    main()
