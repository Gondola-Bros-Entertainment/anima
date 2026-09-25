#!/usr/bin/env python3
"""Exercise transactional scene replacement through an external public-API consumer."""

import argparse
import hashlib
import json
import os
from pathlib import Path
from gpu_preview_smoke import changed, read_ppm
from gpu_smoke import discard_captures, run


def clear(image):
    pixels = image[2]
    assert pixels == pixels[:3] * (image[0] * image[1]), "Empty scene contains geometry"
    assert max(pixels[:3]) < 80, "Empty scene has unexpected background"


def foreground(image):
    pixels = image[2]
    count = sum(
        max(abs(a - b) for a, b in zip(pixels[i : i + 3], pixels[:3])) > 20
        for i in range(0, len(pixels), 3)
    )
    assert count > image[0] * image[1] * 0.01, "Expected geometry is not visible"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("consumer", type=Path)
    parser.add_argument("--viewer", type=Path)
    parser.add_argument("--asset", type=Path)
    parser.add_argument("--output", type=Path, default=Path("build/verification/replacement"))
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    os.environ.setdefault("VK_LAYER_VALIDATE_SYNC", "1")
    records = []
    for mode, extra in [("default", []), ("fallback", ["--no-present-fences"])]:
        for fatal in ["", "upload-timeout", "device-lost"]:
            name = mode + (("-" + fatal) if fatal else "")
            directory = output / name
            directory.mkdir(exist_ok=True)
            for path in directory.glob("*.ppm"):
                path.unlink()
            arguments = ["--replace", str(directory), *extra]
            if fatal:
                arguments.extend(["--fatal", fatal])
            elif args.asset:
                arguments.extend(["--asset", str(args.asset.resolve(strict=True))])
            log = run(args.consumer.resolve(strict=True), arguments, output, name)
            result = next(
                json.loads(line.removeprefix("RESULT "))
                for line in log.splitlines()
                if line.startswith("RESULT ")
            )
            if fatal:
                assert result["fatal_injection"] == fatal and result["fatal_rejected"]
                assert result["scene_generations"] == 2
            else:
                stable = log.split("STABLE_SWAPCHAIN_BEGIN\n", 1)[1].split(
                    "STABLE_SWAPCHAIN_END\n", 1
                )[0]
                assert "Swapchain " not in stable, "Scene replacement recreated the swapchain"
                assert result["recoverable_rollbacks"] == 8 and result["mutation_rejections"] == 3
                assert result["scene_generations"] == 32 + bool(args.asset), result
                assert result["same_window"] and result["minimized_replacement"]
                assert result["swapchain_generations"] >= 2
                images = {path.stem: read_ppm(path) for path in directory.glob("*.ppm")}
                assert len(images) == result["captures"] == 18 + bool(args.asset), result
                for empty in ["empty-start", "empty-end", "empty-final"]:
                    clear(images[empty])
                assert images["empty-start"] == images["empty-end"]
                reference = images["scene-a"]
                for same in [
                    "restored",
                    "rollback-vertex",
                    "rollback-index",
                    "rollback-texture",
                    "rollback-texture-upload",
                    "rollback-descriptors",
                    "rollback-ready",
                    "rollback-invalid",
                    "rollback-update",
                ]:
                    assert images[same] == reference, (
                        f"{name}/{same} did not preserve the old scene"
                    )
                assert changed(reference, images["updated"]) > 0.01
                assert changed(reference, images["scene-b"]) > 0.01
                assert images["scene-b"] == images["scene-b-repeat"]
                assert images["resized"][:2] != reference[:2]
                for mesh in ["scene-a", "scene-b", "resized", "restored-window"]:
                    foreground(images[mesh])
                if args.asset:
                    foreground(images["external-asset"])
                result.update(
                    rollback_pixels_identical=True,
                    repeated_pixels_identical=True,
                    replacement_preserves_swapchain=True,
                    empty_has_no_triangle=True,
                )
            result["case"] = name
            result["capture_sha256"] = {
                path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                for path in sorted(directory.glob("*.ppm"))
            }
            records.append(result)
    if args.viewer:
        path = output / "viewer-empty.ppm"
        path.unlink(missing_ok=True)
        run(
            args.viewer.resolve(strict=True),
            ["--empty", "--validation", "--frames", "3", "--capture", str(path)],
            output,
            "viewer-empty",
        )
        clear(read_ppm(path))
        records.append(
            {
                "case": "viewer-empty",
                "empty_has_no_triangle": True,
                "capture_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            }
        )
    (output / "summary.json").write_text(json.dumps({"runs": records}, indent=2) + "\n")
    print(
        f"PASS {len(records)} replacement/empty/fatal-classification cases; fatal errors are injected, not real device loss"
    )

    discard_captures(output)


if __name__ == "__main__":
    main()
