#!/usr/bin/env python3
"""Verify the optional UI through a public consumer, SDL events and GPU readbacks."""

import argparse
import hashlib
import json
import os
from pathlib import Path
from tempfile import TemporaryDirectory
from generate_ui_fixtures import generate
from gpu_preview_smoke import read_ppm
from gpu_replacement_smoke import clear
from gpu_smoke import discard_captures, run


def pixel(image, x, y, scale):
    offset = (int(y * scale) * image[0] + int(x * scale)) * 3
    return tuple(image[2][offset : offset + 3])


def close(actual, expected, tolerance=3):
    assert len(actual) == 3 and max(abs(a - b) for a, b in zip(actual, expected)) <= tolerance, (
        actual,
        expected,
    )


def linear(byte):
    s = byte / 255
    return s / 12.92 if s <= 0.04045 else ((s + 0.055) / 1.055) ** 2.4


def srgb(value):
    return round(
        255 * (12.92 * value if value <= 0.0031308 else 1.055 * value ** (1 / 2.4) - 0.055)
    )


def verify(images, scale):
    empty, mesh, plain = (images[name] for name in ["empty-ui", "mesh-ui", "mesh-no-ui"])
    background = pixel(empty, 0, 0, scale)
    assert max(background) < 80
    # Rectangular clipping bounds, including every pixel in the solid red region.
    for y in range(int(40 * scale), int(90 * scale)):
        left = (y * empty[0] + int(350 * scale)) * 3
        right = left + int(100 * scale) * 3
        assert empty[2][left:right] == bytes([255, 0, 0]) * int(100 * scale)
    for x, y in [(349, 60), (450, 60), (380, 39), (380, 90), (480, 100)]:
        close(pixel(empty, x, y, scale), background, 0)
    close(pixel(empty, 390, 250, scale), (0, 255, 0), 0)
    close(pixel(empty, 360, 240, scale), background, 0)
    # White 128/255 alpha must composite in linear light over clear and scene.
    alpha_results = []
    for foreground, under in [(empty, background), (mesh, pixel(plain, 400, 160, scale))]:
        expected = tuple(srgb(128 / 255 + linear(c) * (1 - 128 / 255)) for c in under)
        actual = pixel(foreground, 400, 160, scale)
        close(actual, expected)
        alpha_results.append({"under_rgb": under, "expected_rgb": expected, "actual_rgb": actual})
    assert pixel(mesh, 400, 160, scale) != pixel(empty, 400, 160, scale)
    # External PNG texel centers: RGB primaries and a translucent white texel.
    for x, y, expected in [
        (496, 246, (255, 0, 0)),
        (528, 246, (0, 255, 0)),
        (496, 278, (0, 0, 255)),
    ]:
        close(pixel(empty, x, y, scale), expected, 8)
    close(pixel(empty, 528, 278, scale), alpha_results[0]["expected_rgb"], 8)
    # Text atlas produces actual glyphs, and UTF-8 editing changes the input field.
    glyphs = edits = 0
    for y in range(int(30 * scale), int(55 * scale)):
        for x in range(int(28 * scale), int(200 * scale)):
            offset = (y * empty[0] + x) * 3
            glyphs += min(empty[2][offset : offset + 3]) > 200
    for y in range(int(70 * scale), int(97 * scale)):
        for x in range(int(33 * scale), int(145 * scale)):
            offset = (y * empty[0] + x) * 3
            edits += (
                max(
                    abs(a - b)
                    for a, b in zip(
                        empty[2][offset : offset + 3], images["edited"][2][offset : offset + 3]
                    )
                )
                > 30
            )
    assert glyphs > 80 * scale * scale and edits > 30 * scale * scale, (glyphs, edits)
    assert mesh == images["mesh-ui-repeat"], "UI/scene replacement changed stable pixels"
    assert images["dropdown"] != images["edited"], "Dropdown produced no visible change"
    assert images["resized-ui"][:2] != empty[:2]
    clear(images["after-ui-shutdown"])
    return {
        "alpha": alpha_results,
        "rectangular_clip_exact": True,
        "unmasked_transform": True,
        "png_rgba": True,
        "font_glyph_pixels": glyphs,
        "edited_text_pixels": edits,
        "scene_replacement_pixels_identical": True,
        "shutdown_clear": True,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("consumer", type=Path)
    parser.add_argument("--assets", type=Path, default=Path("tests/ui/assets"), help="source controls/font fixtures")
    parser.add_argument("--output", type=Path, default=Path("build/verification/ui"))
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    os.environ.setdefault("VK_LAYER_VALIDATE_SYNC", "1")
    records = []
    try:
        with TemporaryDirectory(prefix="anima-ui-smoke-") as temporary:
            assets = generate(args.assets, Path(temporary))
            for mode, extra in [("default", []), ("fallback", ["--no-present-fences"])]:
                directory = output / mode
                directory.mkdir(exist_ok=True)
                for path in directory.glob("*.ppm"):
                    path.unlink()
                log = run(
                    args.consumer.resolve(strict=True),
                    ["--ui", str(directory), str(assets), *extra],
                    output,
                    mode,
                )
                result = next(
                    json.loads(line.removeprefix("RESULT "))
                    for line in log.splitlines()
                    if line.startswith("RESULT ")
                )
                assert result["frames"] == 36 and result["captures"] == 9 and result["clicks"] == 2, result
                assert result["context_recreations"] == 4 and result["rejected_features"] == 2, result
                assert result["ui_warnings"] == result["ui_errors"] == 0
                images = {p.stem: read_ppm(p) for p in directory.glob("*.ppm")}
                assert len(images) == result["captures"]
                result.update(
                    case=mode,
                    pixel_checks=verify(images, result["display_scale"]),
                    capture_sha256={
                        p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                        for p in sorted(directory.glob("*.ppm"))
                    },
                )
                records.append(result)
    finally:
        discard_captures(output)
    (output / "summary.json").write_text(json.dumps({"runs": records}, indent=2) + "\n")
    print(
        "PASS two UI presentation paths: input, fonts, PNG, linear alpha, clipping, transforms, replacement and cleanup"
    )


if __name__ == "__main__":
    main()
