#!/usr/bin/env python3
"""Independent pixel checks for the public environment/shadow consumer."""

import argparse
import json
import math
import os
from pathlib import Path
from gpu_smoke import discard_captures, run
from gpu_preview_smoke import changed, read_ppm


def ground_pixel(image, x, z, eye=(0, 6, 10), y=0):
    width, height, pixels = image
    # Camera looks at the origin, 45 degree vertical FOV.
    length = math.sqrt(sum(v * v for v in eye))
    backward = [v / length for v in eye]
    horizontal = math.hypot(eye[0], eye[2])
    right = [eye[2] / horizontal, 0, -eye[0] / horizontal]
    up = [
        backward[1] * right[2],
        backward[2] * right[0] - backward[0] * right[2],
        -backward[1] * right[0],
    ]
    x_view = right[0] * x + right[2] * z
    y_view = up[0] * x + up[1] * y + up[2] * z
    depth = length - backward[0] * x - backward[1] * y - backward[2] * z
    px = round((1 + (1 + math.sqrt(2)) * x_view / (depth * 4 / 3)) * width / 2)
    py = round((1 - (1 + math.sqrt(2)) * y_view / depth) * height / 2)
    offset = (py * width + px) * 3
    return tuple(pixels[offset : offset + 3])


def curved_probes(images):
    """Check a known convex silhouette using geometry, independently of the shader.

    Smooth radial normals describe the intended round cylinder. The selected
    coarse side points away from the sun, but the normals at these pixels face
    it. Intersect each shadow texel's light ray with the closed 12-gon to prove
    that ordinary self-occlusion is smaller than the configured residual bias.
    Extending the near-tangent triangle plane beyond itself must not invent an
    additional occluder. As on the slope fixture, allow only two encoded levels
    of rendering/quantization error against the no-shadow reference.
    """
    unshadowed = images["curved-unshadowed"]
    shadowed = images["curved-shadowed"]
    assert shadowed == images["curved-reference-shadowed"], "Curved receiver backend mismatch"
    width, height, pixels = unshadowed
    assert shadowed[:2] == (width, height)
    vertices = [
        (0.3 * math.sin(i * math.pi / 6), 0.3 * math.cos(i * math.pi / 6)) for i in range(12)
    ]
    light = (-4 / math.sqrt(17), 1 / math.sqrt(17))
    right = (light[1], -light[0])

    def dot(a, b):
        return sum(x * y for x, y in zip(a, b))

    geometric_light = dot((math.sin(math.pi / 12), math.cos(math.pi / 12)), light)
    assert geometric_light < 0, "Curved fixture no longer exercises the geometric light terminator"

    def linear(value):
        value /= 255
        return value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4

    probes = []
    for fraction in (0.04, 0.06, 0.08):
        # Use actual pixel centers, not the ideal projected sample coordinates.
        px = round((0.15 * fraction / 1.2 + 0.5) * width - 0.5)
        x = ((px + 0.5) / width - 0.5) * 1.2
        t = x / 0.15
        z = vertices[0][1] * (1 - t) + vertices[1][1] * t
        radial = (x, z)
        length = math.hypot(*radial)
        shaded_light = dot(tuple(v / length for v in radial), light)
        assert shaded_light > 0, "Curved probe is not directly lit"
        bias = 70 * (0.00035 + 0.001 * (1 - shaded_light))
        shadow_pixel = (dot(radial, right) / 50 + 0.5) * 2048 - 0.5
        first_texel = math.floor(shadow_pixel)
        advances = []
        for tap in range(-1, 3):
            projected = ((first_texel + tap + 0.5) / 2048 - 0.5) * 50
            hits = []
            for a, b in zip(vertices, vertices[1:] + vertices[:1]):
                ra, rb = dot(a, right), dot(b, right)
                if min(ra, rb) <= projected <= max(ra, rb):
                    u = (projected - ra) / (rb - ra)
                    hit = tuple(v + (w - v) * u for v, w in zip(a, b))
                    hits.append(dot(hit, light))
            if hits:
                advances.append(max(hits) - dot(radial, light))
        maximum_advance = max(advances, default=0)
        assert maximum_advance < bias, (
            "Curved probe is genuinely occluded beyond bias",
            maximum_advance,
            bias,
        )
        for y in (-0.2, 0, 0.2):
            py = round((0.5 - y / 0.9) * height - 0.5)
            offset = (py * width + px) * 3
            a, b = tuple(pixels[offset : offset + 3]), tuple(shadowed[2][offset : offset + 3])
            assert min(a) > 16, ("Curved probe lacks measurable direct light", px, py, a)
            error = max(abs(u - v) for u, v in zip(a, b))
            assert error <= 2, ("curved receiver invented self-shadow", px, py, a, b, error)
            probes.append(
                {
                    "pixel": [px, py],
                    "geometric_light": geometric_light,
                    "shaded_light": shaded_light,
                    "maximum_caster_advance_m": maximum_advance,
                    "residual_bias_m": bias,
                    "unshadowed": a,
                    "shadowed": b,
                    "linear_visibility": sum(map(linear, b)) / sum(map(linear, a)),
                }
            )
    return probes


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("consumer", type=Path)
    p.add_argument("--output", type=Path, default=Path("build/verification/environment"))
    args = p.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    os.environ.setdefault("VK_LAYER_VALIDATE_SYNC", "1")
    run(args.consumer.resolve(), ["--environment", str(out)], out, "environment", timeout=80)
    images = {path.stem: read_ppm(path) for path in out.glob("*.ppm")}
    for name in ("baseline", "restored", "rejected", "inactive"):
        assert images["shadowed"] == images[f"scene-light-{name}"], (
            "Scene light selection changed accepted pixels", name
        )
    for first, second in (("baseline", "rotated"), ("rotated", "tinted"), ("tinted", "settings")):
        assert changed(images[f"scene-light-{first}"], images[f"scene-light-{second}"]) > 0.005, (
            "Scene light update had no visible effect", first, second
        )
    for other in [
        "shadowed-unculled",
        "invalid-preserved",
        "reference-shadowed",
        "restored-shadow",
        "camera-restored",
    ]:
        assert images["shadowed"] == images[other], f"Environment image changed: {other}"
    for other in ["detail-reference-shadowed", "detail-invalid-preserved", "detail-snapped"]:
        assert images["detail-shadowed"] == images[other], f"Detail shadow image changed: {other}"
    for other in ["detail-away", "detail-disabled"]:
        assert images["shadowed"] == images[other], f"Detail region damaged world coverage: {other}"
    assert images["sky"] == images["sky-shadows-disabled"], "Empty shadow pass changed the sky"
    curve = curved_probes(images)
    slope_errors = {}
    for prefix in ("slope", "steep-slope"):
        assert images[prefix + "-shadowed"] == images[prefix + "-reference-shadowed"], (
            prefix,
            "receiver backend mismatch",
        )
        slope_errors[prefix] = 0
        for x in (-3, -1.5, 0, 1.5, 3):
            for z in (-3, -1.5, 0, 1.5, 3):
                a = ground_pixel(images[prefix + "-unshadowed"], x, z, y=0.35 * x)
                b = ground_pixel(images[prefix + "-shadowed"], x, z, y=0.35 * x)
                error = max(abs(u - v) for u, v in zip(a, b))
                slope_errors[prefix] = max(slope_errors[prefix], error)
                assert error <= 2, (prefix, "receiver self-shadowing", x, z, a, b)
    for name in ("multi-scene-shadowed", "multi-scene-unculled", "multi-scene-rejected"):
        assert images[name] == images["shadowed"], (name, "multi-scene shadow/selection mismatch")
    assert images["multi-scene-unloaded"] == images["caster-hidden"], (
        "Unloaded caster retained rendering"
    )
    probes = []
    detail_probes = []
    # The 2x2 alpha mask has opaque diagonal quadrants and clear off-diagonal quadrants.
    for x, z, opaque in [
        (-0.7, -0.7, True),
        (0.7, -0.7, False),
        (-0.7, 0.7, False),
        (0.7, 0.7, True),
    ]:
        a = ground_pixel(images["unshadowed"], x, z)
        b = ground_pixel(images["shadowed"], x, z)
        loss = sum(a) - sum(b)
        assert loss > 90 if opaque else abs(loss) <= 6, (x, z, opaque, a, b, loss)
        assert ground_pixel(images["caster-hidden"], x, z) == a, (
            "Invisible caster retained its shadow"
        )
        for name in ["detail-shadowed", "detail-only", "detail-outside-world", "detail-boundary"]:
            # The translated fixture and camera have the same relative geometry.
            # Also sample the .16m blend strip at x=-.7 for detail-boundary.
            detailed = ground_pixel(images[name], x, z)
            difference = sum(a) - sum(detailed)
            assert difference > 90 if opaque else abs(difference) <= 6, (
                name,
                x,
                z,
                opaque,
                a,
                detailed,
            )
            detail_probes.append(
                {
                    "capture": name,
                    "position": [x, z],
                    "opaque": opaque,
                    "unshadowed": a,
                    "shadowed": detailed,
                }
            )
        moved = ground_pixel(images["camera-left"], x, z, eye=(-4, 6, 10))
        moved_loss = sum(a) - sum(moved)
        assert moved_loss > 90 if opaque else abs(moved_loss) <= 12, (
            "moving camera",
            x,
            z,
            opaque,
            a,
            moved,
        )
        probes.append({"position": [x, 0, z], "opaque": opaque, "unshadowed": a, "shadowed": b})
    out.joinpath("summary.json").write_text(
        json.dumps(
            {
                "probes": probes,
                "detail_probes": detail_probes,
                "curved_probes": curve,
                "slope_max_channel_error": slope_errors["slope"],
                "steep_slope_max_channel_error": slope_errors["steep-slope"],
                "exact_parity": True,
                "multi_scene_shadow_parity": True,
                "scene_light_direct_and_restored_parity": True,
                "scene_light_rejected_and_inactive_preserve_pixels": True,
                "scene_light_rotation_radiance_and_settings_change_pixels": True,
                "captures_compared_then_deleted": len(images),
                "validation_warnings": 0,
                "validation_errors": 0,
            },
            indent=2,
        )
        + "\n"
    )
    print(
        "PASS scene lights, masked shadows, planar/curved receivers, public API parity, sky, resize and rollback"
    )

    discard_captures(out)


if __name__ == "__main__":
    main()
