"""Independent Blender consumer: generates its own actors, fits, props and recipes.

Copy this file to a separate directory and run Blender with --engine and --out.
It uses public Anima Python modules and no game code, data, names or defaults.
"""

import argparse
import copy
import json
import math
import struct
from pathlib import Path
import sys
import bpy
from mathutils import Matrix, Quaternion, Vector

p = argparse.ArgumentParser()
p.add_argument("--engine", type=Path, required=True)
p.add_argument("--out", type=Path, required=True)
a = p.parse_args(sys.argv[sys.argv.index("--") + 1 :])
sys.path.insert(0, str(a.engine.resolve() / "python"))
from anima_pipeline.properties import AuthoringProperties
from anima_pipeline.receipts import BuildContext
from anima_pipeline.graph import file_hash
from anima_pipeline.validation import qualify_change
from anima_pipeline.blender.sampling import ActionSampler
from anima_pipeline.blender.composition import MotionCompiler
from anima_pipeline.blender.actor import compile_actor
from anima_pipeline.blender.fitted import export_fitted
from anima_pipeline.blender.equipment import export as export_equipment
from anima_pipeline.resources import read_glb

project = a.out.resolve()
project.mkdir(parents=True, exist_ok=False)
context = BuildContext(project, {"consumer/source.py": Path(__file__)})
identity = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]


def write(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def make_source(name, limbs, properties, preserve=False):
    directory = project / name
    directory.mkdir()
    bpy.ops.wm.read_factory_settings(use_empty=True)
    model = bpy.data.collections.new("model")
    bpy.context.scene.collection.children.link(model)
    armature = bpy.data.armatures.new("mechanism")
    rig = bpy.data.objects.new("controller", armature)
    bpy.context.scene.collection.objects.link(rig)
    if preserve:
        rig.scale = (1.8, 1.8, 1.8)
    rig.select_set(True)
    bpy.context.view_layer.objects.active = rig
    bpy.ops.object.mode_set(mode="EDIT")
    origin = armature.edit_bones.new("origin")
    origin.head = (0, 0, 0)
    origin.tail = (0, 0, 1)
    names = ["origin"]
    parents = {"origin": None}
    for limb in range(limbs):
        parent = origin
        for joint in range(3):
            key = "part." + str(3 * limb + joint)
            names.append(key)
            parents[key] = parent.name
            bone = armature.edit_bones.new(key)
            bone.head = ((limb % 2 * 2 - 1) * 0.5, limb // 2 * 0.4, 0.9 - joint * 0.25)
            bone.tail = (bone.head.x, bone.head.y + 0.04, bone.head.z - 0.25)
            bone.parent = parent
            parent = bone
    bpy.ops.object.mode_set(mode="OBJECT")
    rig.animation_data_create()
    material = bpy.data.materials.new("shell surface")
    material.use_nodes = True
    material.node_tree.nodes.get("Principled BSDF").inputs["Base Color"].default_value = (
        0.2,
        0.5,
        0.8,
        1,
    )
    for group, scale in (("body", 1.0), ("shell", 1.1)):
        mesh = bpy.data.meshes.new(group)
        mesh.from_pydata(
            [
                (x * scale, y * scale, z)
                for x, y, z in [(-0.2, -0.1, 0.3), (0.2, -0.1, 0.3), (0, 0.2, 0.7), (0, 0, 1)]
            ],
            [],
            [(0, 1, 2), (0, 3, 1), (1, 3, 2), (2, 3, 0)],
        )
        mesh.update()
        obj = bpy.data.objects.new(group, mesh)
        model.objects.link(obj)
        obj[properties.render_group] = group
        obj.parent = rig
        for key, weight in (("origin", 0.2), ("part.0", 0.8)):
            obj.vertex_groups.new(name=key).add(list(range(4)), weight, "REPLACE")
        modifier = obj.modifiers.new("linear skin", "ARMATURE")
        modifier.object = rig
        # Distinct embedded textures force resource-local material/image index
        # remapping when body and shell are separated.
        surface = material.copy()
        surface.name = group + " surface"
        texture = bpy.data.images.new(group + " pixel", width=1, height=1)
        texture.pixels[:] = [0.2, 0.5, 0.8, 1] if group == "body" else [0.8, 0.3, 0.1, 1]
        texture.pack()
        node = surface.node_tree.nodes.new("ShaderNodeTexImage")
        node.image = texture
        surface.node_tree.links.new(
            node.outputs["Color"],
            surface.node_tree.nodes.get("Principled BSDF").inputs["Base Color"],
        )
        uv = mesh.uv_layers.new(name="UVMap")
        for loop in uv.data:
            loop.uv = (0.5, 0.5)
        obj.data.materials.append(surface)
    actions = []
    for title in ("Glide", "Signal", "Tool"):
        for bone in rig.pose.bones:
            bone.rotation_mode = "QUATERNION"
            bone.location = (0, 0, 0)
            bone.rotation_quaternion = (1, 0, 0, 0)
            bone.scale = (1, 1, 1)
        action = bpy.data.actions.new(title)
        action.use_fake_user = True
        rig.animation_data.action = action
        for frame, phase in ((1, 0.0), (7, 0.5), (13, 1.0)):
            if title == "Glide":
                bone = rig.pose.bones["origin"]
                bone.location = (phase * 0.24, 0, 0)
                bone.keyframe_insert("location", frame=frame)
            else:
                bone = rig.pose.bones["part.0"]
                bone.rotation_quaternion = Quaternion(
                    (0, 1, 0), phase * (0.3 if title == "Tool" else -0.2)
                )
                bone.keyframe_insert("rotation_quaternion", frame=frame)
        for layer in action.layers:
            for strip in layer.strips:
                for bag in strip.channelbags:
                    for curve in bag.fcurves:
                        for point in curve.keyframe_points:
                            point.interpolation = "LINEAR"
        actions.append(action)
    rig.animation_data.action = None
    for bone in rig.pose.bones:
        bone.location = (0, 0, 0)
        bone.rotation_quaternion = (1, 0, 0, 0)
        bone.scale = (1, 1, 1)
    ActionSampler(rig, actions, properties=properties).freeze_defaults()
    bpy.context.scene.frame_set(1)
    bpy.context.view_layer.update()
    source = directory / "source.blend"
    bpy.ops.wm.save_as_mainfile(filepath=str(source))
    evaluation = {
        "version": 1,
        "id": name + ".evaluation",
        "parents": parents,
        "masks": {"tool": ["part.0"]},
        "chains": {},
    }
    write(directory / "evaluation.json", evaluation)
    recipe = {
        "version": 2,
        "id": name + ".motion",
        "sample_rate": 30,
        "bodies": {
            name: {
                "label": name,
                "rig": "controller",
                "mesh_collection": "model",
                "asset_id": name + ".actor",
                "skeleton_id": name + ".rig",
                "stage": "consumer",
                "preserve_hierarchy": preserve,
                "preserve_object_transform": preserve,
            }
        },
        "masks": {"tool": ["part.0", "part.1", "part.2"]},
        "evaluation": name + "/evaluation.json",
        "nodes": [
            {"id": "travel", "kind": "source", "action": "Glide"},
            {"id": "signal", "kind": "source", "action": "Signal"},
            {
                "id": "carry.tool",
                "kind": "source",
                "action": "Tool",
                "mask": "tool",
                "contact_space": {
                    "anchor": "origin",
                    "controls": ["part.0"],
                    "sampling": "baked_30hz",
                },
            },
            {
                "id": "reference",
                "kind": "compose",
                "base": "travel",
                "overlay": "carry.tool",
                "phase": "normalized",
            },
        ],
        "exports": {"drift": "travel", "signal": "signal"},
        "layers": {"carry.tool": "reference"},
        "playback": {
            "drift": {"loop": True, "events": [], "reference_speed": 0.6},
            "signal": {"loop": False, "events": []},
        },
        "equipment": {"shell": {"slot": "surface", "render_group": "shell"}},
    }
    spec = directory / "recipe.json"
    write(spec, recipe)
    return directory, source, spec, len(names)


def compile(source, spec, name, destination, properties):
    return compile_actor(
        source,
        spec,
        name,
        destination,
        context=context,
        material_converter=lambda material, mesh: material,
        properties=properties,
        root_joint="origin",
    )


def check_scaled_motion(source, motion_path, properties, clip="drift"):
    """Compare exported world transforms with an independently sampled source.

    This catches losing object scale on animated root translations, which a
    comparison of body and motion exports to each other would not detect.
    """
    bpy.ops.wm.open_mainfile(filepath=str(source), use_scripts=False)
    rig = bpy.data.objects["controller"]
    sampler = ActionSampler(rig, (), properties=properties)
    sampler.select(bpy.data.actions["Glide"])
    bpy.context.scene.frame_set(7)
    bpy.context.view_layer.update()
    conversion = Matrix.Rotation(-math.pi / 2, 4, "X")
    expected = {bone.name: conversion @ rig.matrix_world @ bone.matrix for bone in rig.pose.bones}
    document, binary = read_glb(motion_path)

    def values(index):
        accessor = document["accessors"][index]
        assert accessor["componentType"] == 5126
        width = {"SCALAR": 1, "VEC3": 3, "VEC4": 4}[accessor["type"]]
        view = document["bufferViews"][accessor["bufferView"]]
        offset = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
        stride = view.get("byteStride", width * 4)
        return [
            struct.unpack_from("<" + "f" * width, binary, offset + i * stride)
            for i in range(accessor["count"])
        ]

    nodes = copy.deepcopy(document["nodes"])
    motion = next(value for value in document["animations"] if value["name"] == clip)
    for channel in motion["channels"]:
        track = motion["samplers"][channel["sampler"]]
        times = values(track["input"])
        data = values(track["output"])
        high = next((i for i, time in enumerate(times) if time[0] >= 0.2), len(times) - 1)
        low = max(0, high - 1)
        span = times[high][0] - times[low][0]
        weight = min(1.0, max(0.0, (0.2 - times[low][0]) / span)) if span else 0.0
        path = channel["target"]["path"]
        if path == "rotation":
            a, b = data[low], data[high]
            q = Quaternion((a[3], *a[:3])).slerp(Quaternion((b[3], *b[:3])), weight)
            value = (q.x, q.y, q.z, q.w)
        else:
            value = tuple(a + (b - a) * weight for a, b in zip(data[low], data[high]))
        nodes[channel["target"]["node"]][path] = value
    parents = {child: i for i, node in enumerate(nodes) for child in node.get("children", [])}
    worlds = {}

    def world(index):
        if index not in worlds:
            node = nodes[index]
            q = node.get("rotation", (0, 0, 0, 1))
            local = Matrix.LocRotScale(
                Vector(node.get("translation", (0, 0, 0))),
                Quaternion((q[3], *q[:3])),
                Vector(node.get("scale", (1, 1, 1))),
            )
            if "matrix" in node:
                local = Matrix([node["matrix"][i : i + 4] for i in range(0, 16, 4)]).transposed()
            worlds[index] = world(parents[index]) @ local if index in parents else local
        return worlds[index]

    for name, matrix in expected.items():
        index = next(i for i, node in enumerate(nodes) if node["name"] == name)
        assert (
            max(abs(world(index)[r][c] - matrix[r][c]) for r in range(4) for c in range(4)) < 1e-5
        ), name
        parent = rig.data.bones[name].parent
        if parent:
            assert nodes[parents[index]]["name"] == parent.name, "Hierarchy was flattened"


reports = {}
for name, limbs, preserve in (("pilot", 2, False), ("crawler", 4, False), ("scaled", 2, True)):
    properties = (
        AuthoringProperties()
        if name == "pilot"
        else AuthoringProperties(
            **{field: "crawler." + field for field in AuthoringProperties.__dataclass_fields__}
        )
    )
    directory, source, spec, joints = make_source(name, limbs, properties, preserve)
    source_hash = file_hash(source)
    first = compile(source, spec, name, directory / "compiled", properties)
    second = compile(source, spec, name, directory / "repeat", properties)
    assert first["manifest"]["skeleton"]["joint_count"] == joints
    before = context.verify_receipt(
        directory / "compiled" / (name + ".build.json"), source, spec, body=name
    )
    after = context.verify_receipt(
        directory / "repeat" / (name + ".build.json"), source, spec, body=name
    )
    assert before["outputs"] == after["outputs"], "Clean builds differed"
    assert file_hash(source) == source_hash, "Compilation edited authored source"
    if preserve:
        check_scaled_motion(source, directory / "compiled" / (name + ".motion.glb"), properties)
    # The same source owners sample identically when compiled in reverse order.
    bpy.ops.wm.open_mainfile(filepath=str(source), use_scripts=False)
    compiler = MotionCompiler(name, spec, properties=properties)
    rig = compiler.rig
    normal = compiler.exports()
    sampler = ActionSampler(rig, compiler.sources.values(), properties=properties)

    def sample(actions):
        values = {}
        for key, action in actions.items():
            sampler.select(action)
            bpy.context.scene.frame_set(7)
            bpy.context.view_layer.update()
            values[key] = [[list(row) for row in bone.matrix] for bone in rig.pose.bones]
        return values

    expected = sample(normal)
    reverse = MotionCompiler(name, spec, properties=properties)
    reordered = {
        key: reverse.build(owner) for key, owner in reversed(list(reverse.outputs().items()))
    }
    assert sample(reordered) == expected, "Source evaluation depended on build order"
    # One diagnostic travel edit reaches its dependent layer context, preserving
    # the independently authored signal, fitted geometry and bind data.
    source_bytes = source.read_bytes()
    bpy.ops.wm.open_mainfile(filepath=str(source), use_scripts=False)
    action = bpy.data.actions["Glide"]
    for layer in action.layers:
        for strip in layer.strips:
            for bag in strip.channelbags:
                for curve in bag.fcurves:
                    if curve.array_index == 0:
                        curve.keyframe_points[1].co.y += 0.025
                        curve.update()
    changed = directory / "changed.blend"
    bpy.ops.wm.save_as_mainfile(filepath=str(changed))
    compile(changed, spec, name, directory / "changed", properties)
    change = qualify_change(
        directory / "compiled" / (name + ".asset.json"),
        directory / "changed" / (name + ".asset.json"),
        ("drift", "carry.tool"),
        accepted_garments=directory / "compiled/compiled-fits.json",
        candidate_garments=directory / "changed/compiled-fits.json",
    )
    assert set(change["changed_motion_resources"]) == {"drift", "carry.tool"}, change
    assert source.read_bytes() == source_bytes
    fitted = export_fitted(
        source,
        spec,
        name,
        name,
        "shell",
        "extra-shell",
        "surface",
        directory / "fitted",
        context=context,
        material_converter=lambda material, mesh: material,
        properties=properties,
        root_joint="origin",
    )
    assert fitted["animation_clips"] == 0
    assert (
        json.loads((directory / "fitted" / (name + ".extra-shell.asset.json")).read_text())[
            "skeleton"
        ]
        == first["manifest"]["skeleton"]
    )
    equipment = {
        "version": 1,
        "id": "instrument",
        "collection": "model",
        "rig": "controller",
        "clips": {"extend": "Glide" if preserve else "Tool"},
        "visual": {
            "id": "instrument",
            "primary_grip": identity,
            "markers": {"tip": identity},
            "primary_node": "origin",
            "marker_nodes": {"tip": "part.2"},
            "animation_tracks": {"pulse": "extend"},
        },
    }
    write(directory / "equipment.recipe.json", equipment)
    export_equipment(source, directory / "equipment.recipe.json", directory / "equipment")
    if preserve:
        check_scaled_motion(source, directory / "equipment/instrument.glb", properties, "extend")
    write(
        directory / "actor.profile.json",
        {
            "version": 1,
            "id": name,
            "manifest": "compiled/" + name + ".asset.json",
            "capabilities": ["signal"],
            "sockets": {"tool": {"node": "part.2", "frame": None}},
        },
    )
    visual = json.loads((directory / "equipment/instrument.visual.json").read_text())
    visual["model"] = "equipment/instrument.glb"
    write(
        directory / "attachments.json",
        {
            "schema_version": 2,
            "units": "meters",
            "empty_handling": "free",
            "defaults": {"tool": "probe"},
            "handling": [
                {"id": "free", "socket": "", "carry": ""},
                {"id": "tool", "socket": "tool", "carry": "carry.tool"},
            ],
            "visuals": [visual],
            "items": [
                {
                    "id": "probe",
                    "category": "instrument",
                    "visual": "instrument",
                    "handling": "tool",
                }
            ],
        },
    )
    write(
        directory / "actions.json",
        {
            "schema_version": 1,
            "actions": [
                {
                    "id": "signal",
                    "handling": ["tool"],
                    "roles": {"tool": ["tool"]},
                    "phases": [
                        {
                            "id": "prepare",
                            "duration": 0.2,
                            "layers": [{"clip": "signal", "mask": "tool", "interval": [0, 0.5]}],
                        },
                        {
                            "id": "hold",
                            "duration": 0.3,
                            "held": True,
                            "layers": [{"clip": "signal", "mask": "tool", "interval": [0.5, 0.5]}],
                            "props": [{"role": "tool", "track": "pulse", "interval": [0, 1]}],
                        },
                        {
                            "id": "release",
                            "duration": 0.2,
                            "layers": [{"clip": "signal", "mask": "tool", "interval": [0.5, 1]}],
                        },
                    ],
                }
            ],
        },
    )
    reports[name] = {
        "joints": joints,
        "clean_build_exact": True,
        "reverse_order_exact": True,
        "source_unchanged": True,
        "scaled_hierarchy_world_parity": preserve,
        "change": change,
        "fitted_export": True,
        "animated_equipment_export": True,
        "receipt": before["build_key"],
    }
write(
    project / "verification.json",
    {"standalone": True, "game_dependencies": False, "actors": reports},
)
print(
    "PASS independent authoring consumer: two anatomies, exact repeat/order, source propagation, fitted meshes, animated props and receipts",
    flush=True,
)
