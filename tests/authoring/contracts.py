"""Public authoring API tests using an isolated synthetic project."""

import copy
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from anima_pipeline.graph import MotionGraph, validate_evaluation
from anima_pipeline.properties import AuthoringProperties
from anima_pipeline.receipts import BuildContext
from anima_pipeline.resources import read_glb, write_glb, subset
from anima_pipeline.preservation import content
from anima_pipeline.bundles import copy_runtime_tree


def recipe():
    return {
        "version": 2,
        "id": "consumer.motion",
        "sample_rate": 30,
        "bodies": {
            "crawler": {
                "label": "Crawler",
                "rig": "control",
                "mesh_collection": "model",
                "asset_id": "crawler",
                "skeleton_id": "crawler.rig",
                "stage": "consumer",
            }
        },
        "masks": {"tool": ["appendage"]},
        "evaluation": "rig.json",
        "nodes": [
            {"id": "travel", "kind": "source", "action": "Glide"},
            {"id": "idle", "kind": "source", "action": "Pause"},
            {"id": "carry", "kind": "source", "action": "Tool", "mask": "tool"},
            {
                "id": "combined",
                "kind": "compose",
                "base": "travel",
                "overlay": "carry",
                "phase": "normalized",
            },
        ],
        "exports": {"drift": "travel", "pause": "idle"},
        "layers": {"carry": "combined"},
        "playback": {name: {"loop": True, "events": []} for name in ("drift", "pause")},
    }


class Contracts(unittest.TestCase):
    def test_preservation_checks_remain_enabled_in_optimized_python(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            accepted, candidate = root / "accepted.glb", root / "candidate.glb"
            source = {"asset": {"version": "2.0"}, "nodes": [{"name": "root"}]}
            write_glb(accepted, source, b"")
            write_glb(candidate, source, b"")
            command = [
                sys.executable,
                "-O",
                "-m",
                "anima_pipeline.preservation",
                str(accepted),
                str(candidate),
                "--out",
                str(root / "report.json"),
            ]
            environment = {
                **os.environ,
                "PYTHONPATH": str(Path(__file__).resolve().parents[2] / "python"),
            }
            result = subprocess.run(command, env=environment, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            source["nodes"][0]["translation"] = [1, 0, 0]
            write_glb(candidate, source, b"")
            result = subprocess.run(command, env=environment, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Candidate changes geometry", result.stderr)
            self.assertFalse(json.loads((root / "report.json").read_text())["transforms_unchanged"])

    def test_preservation_rejects_invalid_glb_header(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "invalid.glb"
            write_glb(path, {"asset": {"version": "2.0"}, "nodes": []}, b"")
            path.write_bytes(b"xxxx" + path.read_bytes()[4:])
            with self.assertRaisesRegex(ValueError, "complete GLB v2"):
                content(path)

    def test_body_export_options_are_typed_and_invalidate_resources(self):
        data = recipe()
        payloads = {"travel": "a", "idle": "b", "carry": "c"}
        before = MotionGraph(data).keys(payloads, "crawler", "rig")
        for option in ("preserve_hierarchy", "preserve_object_transform"):
            changed = copy.deepcopy(data)
            changed["bodies"]["crawler"][option] = True
            after = MotionGraph(changed).keys(payloads, "crawler", "rig")
            self.assertTrue(all(before[name] != after[name] for name in before))
            for invalid in (0, 1, None, "true"):
                changed["bodies"]["crawler"][option] = invalid
                with self.assertRaises(ValueError):
                    MotionGraph(changed)

    def test_dependency_invalidation_and_order(self):
        graph = MotionGraph(recipe())
        before = graph.keys({"travel": "a", "idle": "b", "carry": "c"}, "crawler", "rig")
        after = graph.keys({"travel": "changed", "idle": "b", "carry": "c"}, "crawler", "rig")
        self.assertEqual({k for k in before if before[k] != after[k]}, {"travel", "combined"})
        data = recipe()
        data["nodes"].reverse()
        self.assertEqual(
            MotionGraph(data).keys({"travel": "a", "idle": "b", "carry": "c"}, "crawler", "rig"),
            before,
        )

    def test_invalid_graphs_reject(self):
        for mutate in (
            lambda d: d.update(version=None),
            lambda d: d["nodes"][3].update(base="combined"),
            lambda d: d["nodes"][3].update(overlay="missing"),
            lambda d: d["masks"].update(tool=["appendage", "appendage"]),
            lambda d: d.update(evaluation="../outside.json"),
            lambda d: d["playback"].pop("pause"),
        ):
            data = recipe()
            mutate(data)
            with self.assertRaises(ValueError):
                MotionGraph(data)

    def test_explicit_anatomy_and_properties(self):
        evaluation = {
            "version": 1,
            "id": "rig",
            "parents": {"hub": None, "a": "hub", "b": "a", "tip": "b"},
            "masks": {"probe": ["a"]},
            "chains": {
                "reach": {"joints": ["a", "b", "tip"], "minimum_angle": 0, "maximum_angle": 3.1}
            },
        }
        validate_evaluation(evaluation)
        evaluation["parents"]["hub"] = "tip"
        with self.assertRaises(ValueError):
            validate_evaluation(evaluation)
        self.assertEqual(
            AuthoringProperties(sampling_defaults="custom.saved").sampling_defaults, "custom.saved"
        )
        with self.assertRaises(ValueError):
            AuthoringProperties(reference_speed="anima.render_group")
        self.assertNotIn("bpy", sys.modules)

    def make_build(self, root):
        (root / "compiler.py").write_text("consumer policy")
        context = BuildContext(root, {"consumer/policy.py": root / "compiler.py"})
        source = root / "crawler.blend"
        source.write_bytes(b"fixture source")
        spec = root / "recipe.json"
        spec.write_text(json.dumps(recipe()))
        (root / "rig.json").write_text(
            json.dumps(
                {"version": 1, "id": "rig", "parents": {"hub": None}, "masks": {}, "chains": {}}
            )
        )
        outputs = [
            root / ("crawler" + suffix)
            for suffix in (".glb", ".asset.json", ".motion.json", ".motion.glb")
        ]
        for path in outputs:
            path.write_bytes(b"fixture output")
        receipt = root / "crawler.build.json"
        context.write_receipt(receipt, source, spec, "crawler", {}, outputs, {"version": "fixture"})
        return context, source, spec, outputs, receipt

    def test_receipt_tracks_engine_and_consumer_and_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            context, source, spec, outputs, receipt = self.make_build(root)
            value = context.verify_receipt(receipt, source, spec)
            self.assertIn("anima_pipeline/blender/bake.py", value["inputs"]["compiler"])
            for path in (source, root / "compiler.py", root / "rig.json", *outputs):
                before = path.read_bytes()
                path.write_bytes(before + b"changed")
                with self.assertRaises(ValueError):
                    context.verify_receipt(receipt, source, spec)
                path.write_bytes(before)
            data = json.loads(receipt.read_text())
            data["version"] = None
            receipt.write_text(json.dumps(data))
            with self.assertRaises(ValueError):
                context.verify_receipt(receipt, source, spec)

    def test_receipt_rejects_missing_or_redirected_resources(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            context, source, spec, outputs, receipt = self.make_build(root)
            original = receipt.read_text()
            data = json.loads(original)
            del data["outputs"]["crawler.motion.glb"]
            receipt.write_text(json.dumps(data))
            with self.assertRaises(ValueError):
                context.verify_receipt(receipt, source, spec)
            receipt.write_text(original)
            outputs[0].unlink()
            try:
                outputs[0].symlink_to(source)
            except OSError:
                self.skipTest("Symlink creation unavailable")
            with self.assertRaises(ValueError):
                context.verify_receipt(receipt, source, spec)

    def test_bundle_includes_nested_resources_without_build_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            (source / "nested/fit").mkdir(parents=True)
            (source / "nested/fit/shell.glb").write_bytes(b"fit")
            (source / "body.build.json").write_text("{}")
            (source / "body.reference.glb").write_bytes(b"evidence")
            (source / "body.blend").write_bytes(b"source")
            copy_runtime_tree(source, root / "bundle")
            self.assertEqual(
                [
                    p.relative_to(root / "bundle").as_posix()
                    for p in (root / "bundle").rglob("*")
                    if p.is_file()
                ],
                ["nested/fit/shell.glb"],
            )

    def test_motion_split_remaps_channels_and_preserves_payload(self):
        binary = struct.pack("<8f", 0, 1, 0, 0, 0, 0.25, 0, 0)
        source = {
            "asset": {"version": "2.0"},
            "nodes": [{"name": "unused"}, {"name": "hub", "children": [2]}, {"name": "tip"}],
            "buffers": [{"byteLength": len(binary)}],
            "bufferViews": [
                {"buffer": 0, "byteLength": 8},
                {"buffer": 0, "byteOffset": 8, "byteLength": 24},
            ],
            "accessors": [
                {"bufferView": 0, "componentType": 5126, "count": 2, "type": "SCALAR"},
                {"bufferView": 1, "componentType": 5126, "count": 2, "type": "VEC3"},
            ],
            "animations": [
                {
                    "name": "move",
                    "samplers": [{"input": 0, "output": 1}],
                    "channels": [{"sampler": 0, "target": {"node": 2, "path": "translation"}}],
                }
            ],
        }
        result, payload = subset(source, binary, animations=source["animations"])
        self.assertEqual(payload, binary)
        self.assertEqual([n["name"] for n in result["nodes"]], ["hub", "tip"])
        self.assertEqual(result["animations"][0]["channels"][0]["target"]["node"], 1)
        self.assertNotIn("meshes", result)
        self.assertNotIn("skins", result)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "motion.glb"
            write_glb(path, result, payload)
            decoded, raw = read_glb(path)
            self.assertEqual(raw, binary)
            self.assertEqual(decoded["animations"], result["animations"])

    def test_texture_remapping_preserves_identity_and_detects_image_changes(self):
        binary = (
            struct.pack("<9f3H", 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 2)
            + b"xx"
            + b"firstimg"
            + b"secondim"
        )
        source = {
            "asset": {"version": "2.0"},
            "nodes": [{"name": "shell", "mesh": 0}],
            "buffers": [{"byteLength": len(binary)}],
            "bufferViews": [
                {"buffer": 0, "byteLength": 36},
                {"buffer": 0, "byteOffset": 36, "byteLength": 6},
                {"buffer": 0, "byteOffset": 44, "byteLength": 8},
                {"buffer": 0, "byteOffset": 52, "byteLength": 8},
            ],
            "accessors": [
                {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
                {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"},
            ],
            "images": [
                {"bufferView": 2, "mimeType": "image/png"},
                {"bufferView": 3, "mimeType": "image/png"},
            ],
            "textures": [{"source": 0}, {"source": 1, "sampler": 0}],
            "samplers": [{"wrapS": 33071}],
            "materials": [
                {"pbrMetallicRoughness": {"baseColorTexture": {"index": 1, "texCoord": 0}}}
            ],
            "meshes": [
                {"primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "material": 0}]}
            ],
        }
        split, payload = subset(source, binary, mesh_nodes=[0])
        self.assertEqual(
            split["materials"][0]["pbrMetallicRoughness"]["baseColorTexture"]["index"], 0
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            before = root / "before.glb"
            after = root / "after.glb"
            write_glb(before, source, binary)
            write_glb(after, split, payload)
            self.assertEqual(content(before), content(after))
            view = split["bufferViews"][split["images"][0]["bufferView"]]
            changed = bytearray(payload)
            changed[view["byteOffset"]] ^= 1
            write_glb(after, split, changed)
            self.assertNotEqual(content(before)[0], content(after)[0])
            split["samplers"][0]["wrapS"] = 10497
            write_glb(after, split, payload)
            self.assertNotEqual(content(before)[0], content(after)[0])


if __name__ == "__main__":
    unittest.main()
