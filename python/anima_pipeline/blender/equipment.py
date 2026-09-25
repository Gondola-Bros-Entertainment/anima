"""Export one independently authored static or animated equipment asset.

The recipe names its collection, optional animation rig and exact source actions.
Standard glTF materials, meshes and skinning are retained. Character clips are
not rebuilt, and the saved source is never changed.
"""

import hashlib
import json
from pathlib import Path

import bpy
from ..receipts import compiler_sources
from ..graph import file_hash


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def export(source, recipe_path, output):
    source, recipe_path, output = map(lambda p: Path(p).resolve(), (source, recipe_path, output))
    recipe = json.loads(recipe_path.read_text())
    if (
        set(recipe) != {"version", "id", "collection", "rig", "clips", "visual"}
        or recipe["version"] != 1
    ):
        raise ValueError("Equipment recipe needs explicit version/id/collection/rig/clips/visual")
    stem = recipe["id"]
    if not stem or any(c not in "abcdefghijklmnopqrstuvwxyz0123456789.-_" for c in stem):
        raise ValueError("Equipment ID must be a safe filename")
    if not isinstance(recipe["clips"], dict) or bool(recipe["clips"]) != bool(recipe["rig"]):
        raise ValueError("Animated equipment needs an explicit rig and source clips")
    inputs = {
        "source": digest(source),
        "recipe": digest(recipe_path),
        "compiler": {name: file_hash(path) for name, path in compiler_sources().items()},
    }
    output.mkdir(parents=True, exist_ok=False)
    bpy.ops.wm.open_mainfile(filepath=str(source), use_scripts=False)
    scene = bpy.context.scene
    scene.render.fps = 30
    collection = bpy.data.collections[recipe["collection"]]
    objects = set(collection.all_objects)
    if not any(o.type == "MESH" for o in objects):
        raise ValueError("Equipment collection has no mesh")
    rig = bpy.data.objects[recipe["rig"]] if recipe["rig"] else None
    selected = set(objects)
    if rig:
        if rig.type != "ARMATURE":
            raise ValueError("Equipment animation rig must be an armature")
        selected.add(rig)
    # Require a self-contained export rather than silently dropping external
    # constraints, parents or deformation rigs from the selected object set.
    for obj in selected:
        if obj.parent and obj.parent not in selected:
            raise ValueError("Equipment has a parent outside its export collection: " + obj.name)
        for modifier in obj.modifiers:
            if modifier.type == "ARMATURE" and modifier.object != rig:
                raise ValueError("Equipment has an undeclared skin rig: " + obj.name)
        if obj.animation_data and obj != rig:
            raise ValueError("Put equipment motion on the declared rig: " + obj.name)
    actions = {}
    if rig:
        rig.data.pose_position = "POSE"
        rig.animation_data_create()
        for track in list(rig.animation_data.nla_tracks):
            rig.animation_data.nla_tracks.remove(track)
        for clip, action_name in recipe["clips"].items():
            if not clip or not isinstance(action_name, str):
                raise ValueError("Equipment clip needs a name and a source action")
            source_action = bpy.data.actions[action_name]
            if source_action.frame_range[0] != 1 or source_action.frame_range[1] <= 1:
                raise ValueError("Equipment actions need a positive interval beginning at frame 1")
            action = source_action.copy()
            actions[clip] = action
        rig.animation_data.action = None
    for obj in list(bpy.data.objects):
        if obj not in selected:
            bpy.data.objects.remove(obj, do_unlink=True)
    for action in list(bpy.data.actions):
        if action not in actions.values():
            bpy.data.actions.remove(action)
    if rig:
        for clip, action in actions.items():
            action.name = clip
            track = rig.animation_data.nla_tracks.new()
            track.name = clip
            track.strips.new(clip, 1, action)
            track.mute = True
        rig.animation_data.action = next(iter(actions.values()))
    scene.frame_set(1)
    bpy.ops.object.select_all(action="DESELECT")
    for obj in selected:
        obj.hide_set(False)
        obj.hide_viewport = False
        obj.select_set(True)
    bpy.context.view_layer.objects.active = rig or next(o for o in selected if o.type == "MESH")
    target = output / (stem + ".glb")
    bpy.ops.export_scene.gltf(
        filepath=str(target),
        export_format="GLB",
        use_selection=True,
        export_yup=True,
        export_cameras=False,
        export_lights=False,
        export_materials="EXPORT",
        export_animations=bool(actions),
        export_animation_mode="ACTIONS",
        export_force_sampling=True,
        export_frame_range=False,
        export_skins=True,
        # Keep the authored object transform on animated props;
        # folding its scale into binds loses root translation scale.
        export_armature_object_remove=False,
        export_anim_slide_to_zero=True,
    )
    visual = dict(recipe["visual"], model=target.name)
    (output / (stem + ".visual.json")).write_text(json.dumps(visual, indent=2) + "\n")
    if inputs != {
        "source": digest(source),
        "recipe": digest(recipe_path),
        "compiler": {name: file_hash(path) for name, path in compiler_sources().items()},
    }:
        raise ValueError("Equipment source/recipe/exporter changed during export")
    receipt = {
        "version": 2,
        "id": stem,
        "status": "exported",
        "inputs": inputs,
        "source": str(source),
        "recipe": str(recipe_path),
        "blender": bpy.app.version_string,
        "outputs": {p.name: digest(p) for p in (target, output / (stem + ".visual.json"))},
        "clips": list(actions),
        "review": "pending native validation and visual review",
    }
    (output / (stem + ".build.json")).write_text(json.dumps(receipt, indent=2) + "\n")
    print("EQUIPMENT_EXPORTED", stem, flush=True)
