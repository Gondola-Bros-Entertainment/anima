"""Compile declared control-space motion layers on the saved Blender rig.

The mask replaces named controls (including their IK/FK switches); it never
replaces the torso or legs implicitly. Donor curves, including modifiers, are
evaluated at the export sample rate. This is baked override composition, not
runtime additive blending or a contact solver. Saved actions stay read-only.
"""

import json
from collections import OrderedDict

import bpy

from ..graph import digest, load_graph
from ..properties import AuthoringProperties
from .sampling import ActionSampler
from . import contact_space

# Numeric layer data only: no RNA pointers survive file loads. The key includes
# all sampled donor channels (including the anchor's inputs) and the rig contract.
# Compiled actions are always rebuilt; this bounded cache accelerates extraction.
CONTACT_CACHE = OrderedDict()


def channelbag(action):
    if len(action.slots) != 1 or len(action.layers) != 1 or len(action.layers[0].strips) != 1:
        raise ValueError(f"Expected a single-slot, single-layer source action: {action.name}")
    bags = action.layers[0].strips[0].channelbags
    if len(bags) != 1:
        raise ValueError(f"Ambiguous source channels: {action.name}")
    return bags[0]


def frame_end(action):
    first, last = action.frame_range
    if abs(first - 1) > 1e-6 or abs(last - round(last)) > 1e-6 or last < 2:
        raise ValueError(f"Motion needs integral frames starting at 1: {action.name}")
    return round(last)


def owns(path, controls):
    return any(path.startswith("pose.bones[" + json.dumps(n) + "]") for n in controls)


def curves(action, controls=None):
    result = list(channelbag(action).fcurves)
    if len({(f.data_path, f.array_index) for f in result}) != len(result):
        raise ValueError(f"Duplicate source channel: {action.name}")
    if any(f.mute or not f.is_valid for f in result):
        raise ValueError(f"Muted or invalid source curve: {action.name}")
    return [f for f in result if controls is None or owns(f.data_path, controls)]


def fingerprint(action, controls=None, *, properties=AuthoringProperties()):
    end = frame_end(action)
    values = [
        [f.data_path, f.array_index, [f.evaluate(t) for t in range(1, end + 1)]]
        for f in sorted(curves(action, controls), key=lambda f: (f.data_path, f.array_index))
    ]
    return digest([end, action.get(properties.reference_speed), values])


def rig_fingerprint(rig, *, properties=AuthoringProperties()):
    if properties.sampling_defaults not in rig:
        raise ValueError("Versioned motion builds require frozen action sampling defaults")

    def rna_values(value):
        result = {}
        for prop in value.bl_rna.properties:
            if prop.identifier == "rna_type" or (prop.is_readonly and prop.type != "COLLECTION"):
                continue
            item = getattr(value, prop.identifier)
            if prop.type == "POINTER":
                result[prop.identifier] = (
                    item.name_full if hasattr(item, "name_full") else getattr(item, "name", None)
                )
            elif prop.type == "COLLECTION":
                result[prop.identifier] = [rna_values(child) for child in item]
            elif prop.type in ("BOOLEAN", "INT", "FLOAT", "STRING", "ENUM"):
                result[prop.identifier] = list(item) if getattr(prop, "is_array", False) else item
        return result

    drivers = [
        [f.data_path, f.array_index, rna_values(f.driver), [rna_values(m) for m in f.modifiers]]
        for f in rig.animation_data.drivers
    ]
    return digest(
        [
            json.loads(rig[properties.sampling_defaults]),
            [
                [
                    b.name,
                    b.parent.name if b.parent else None,
                    b.use_deform,
                    b.inherit_scale,
                    b.use_inherit_rotation,
                    b.use_local_location,
                    b.length,
                    b.bbone_segments,
                    [list(row) for row in b.matrix_local],
                ]
                for b in rig.data.bones
            ],
            [[b.name, [rna_values(c) for c in b.constraints]] for b in rig.pose.bones],
            [rna_values(c) for c in rig.constraints],
            drivers,
        ]
    )


class MotionCompiler:
    def __init__(self, body, recipe, *, properties=AuthoringProperties()):
        self.properties = properties
        self.graph = load_graph(recipe)
        self.body = body
        self.adapter = self.graph.data["bodies"][body]
        self.rig = bpy.data.objects[self.adapter["rig"]]
        self.rig.data.pose_position = "POSE"
        for track in self.rig.animation_data.nla_tracks:
            track.mute = True
        for name, mask in self.graph.data["masks"].items():
            missing = set(mask) - set(self.rig.pose.bones.keys())
            if missing:
                raise ValueError(f"Unknown controls in {name}: {sorted(missing)}")
        self.sources = {}
        self.payloads = {}
        self.contact_layers = {}
        self.contact_reports = {}
        for name in self.graph.order:
            node = self.graph.nodes[name]
            if "action" not in node:
                continue
            action_name = node["action"].format(body=self.adapter["label"])
            action = bpy.data.actions.get(action_name)
            if action is None:
                raise ValueError(f"Missing explicit source action: {action_name}")
            self.sources[name] = action
            controls = self.graph.data["masks"].get(node.get("mask"))
            if "contact_space" in node:
                if node["contact_space"]["anchor"] not in self.rig.pose.bones:
                    raise ValueError("Unknown contact anchor: " + node["contact_space"]["anchor"])
            else:
                self.payloads[name] = fingerprint(action, controls, properties=properties)
        # Rigify drivers evaluate constraint influence/mute from the current
        # IK/FK controls. Hash the rig in its frozen default state, otherwise
        # merely scrubbing or switching actions changes the build identity.
        # Source curves and driver definitions remain part of the contract.
        sampler = ActionSampler(self.rig, self.sources.values(), properties=properties)
        sampler.select(None)
        bpy.context.view_layer.update()
        self.rig_key = rig_fingerprint(self.rig, properties=properties)
        for name, action in self.sources.items():
            node = self.graph.nodes[name]
            if "contact_space" not in node:
                continue
            controls = self.graph.data["masks"][node["mask"]]
            cache_key = digest(
                [
                    self.rig_key,
                    fingerprint(action, properties=properties),
                    controls,
                    node["contact_space"],
                ]
            )
            if cache_key not in CONTACT_CACHE:
                CONTACT_CACHE[cache_key] = contact_space.extract(
                    self.rig,
                    action,
                    curves(action, controls),
                    node["contact_space"],
                    properties=properties,
                )
                if len(CONTACT_CACHE) > 24:
                    CONTACT_CACHE.popitem(last=False)
            CONTACT_CACHE.move_to_end(cache_key)
            layer = CONTACT_CACHE[cache_key]
            self.contact_layers[name] = layer
            self.payloads[name] = digest(layer)
        sampler.select(None)
        bpy.context.view_layer.update()
        self.keys = self.graph.keys(self.payloads, body, self.rig_key)
        self.compiled = {}

    def build(self, name):
        if name in self.compiled:
            return self.compiled[name]
        node = self.graph.nodes[name]
        if node["kind"] != "compose":
            result = self.sources[name]
        else:
            base = self.build(node["base"])
            overlay = self.build(node["overlay"])
            overlay_node = self.graph.nodes[node["overlay"]]
            controls = self.graph.data["masks"][overlay_node["mask"]]
            end, donor_end = frame_end(base), frame_end(overlay)
            result = base.copy()
            result.name = "Generated · " + name
            result[self.properties.generated_from] = self.keys[name]
            result[self.properties.motion_node] = name
            bag = channelbag(result)
            for curve in list(bag.fcurves):
                if owns(curve.data_path, controls):
                    bag.fcurves.remove(curve)
            if node["overlay"] in self.contact_layers:
                self.contact_reports[name] = contact_space.compose(
                    self.rig,
                    result,
                    bag,
                    self.contact_layers[node["overlay"]],
                    end,
                    properties=self.properties,
                )
            else:
                for donor in curves(overlay, controls):
                    curve = bag.fcurves.new(donor.data_path, index=donor.array_index)
                    curve.keyframe_points.add(end)
                    for frame, point in enumerate(curve.keyframe_points, 1):
                        donor_frame = 1 + (frame - 1) * (donor_end - 1) / (end - 1)
                        point.co = (frame, donor.evaluate(donor_frame))
                        point.interpolation = "LINEAR"
                    curve.update()
            if frame_end(result) != end:
                raise ValueError(f"Composition changed base timing: {name}")
        self.compiled[name] = result
        return result

    def exports(self):
        return {clip: self.build(name) for clip, name in self.outputs().items()}

    def outputs(self):
        return {**self.graph.data["exports"], **self.graph.data["layers"]}

    def preview(self, base, layer):
        if not layer:
            return self.build(base)
        if (
            base not in self.graph.data["exports"].values()
            or layer not in self.graph.data["layers"]
        ):
            raise ValueError("Preview requires registered motion and handling owners")
        name = "preview." + base + "." + layer
        self.graph.nodes[name] = {
            "id": name,
            "kind": "compose",
            "base": base,
            "overlay": layer,
            "phase": "normalized",
        }
        self.keys[name] = digest([self.keys[base], self.keys[layer]])
        return self.build(name)

    def report(self):
        return {
            "contract": self.graph.data["id"],
            "body": self.body,
            "rig_key": self.rig_key,
            "sample_rate": self.graph.data["sample_rate"],
            "node_keys": self.keys,
            "exports": self.graph.data["exports"],
            "layers": self.graph.data["layers"],
            "contact_layers": {
                name: {
                    "key": self.keys[name],
                    "frames": layer["frames"],
                    "space": layer["space"],
                    "channels": len(layer["channels"]),
                }
                for name, layer in self.contact_layers.items()
            },
            "contact_compositions": self.contact_reports,
        }


def prepare_export(body, recipe, *, properties=AuthoringProperties()):
    compiler = MotionCompiler(body, recipe, properties=properties)
    actions = compiler.exports()
    return actions, compiler.report()
