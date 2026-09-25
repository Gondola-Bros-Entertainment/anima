"""Bake control rigs to standalone GLB resources with linear skinning.

Sample evaluated joint transforms and export only the selected runtime meshes
and skeleton. The saved authoring source is read-only.
"""

import hashlib
import json

import bpy
from mathutils import Matrix
from .sampling import ActionSampler, all_fcurves
from ..properties import AuthoringProperties

MATRIX_TOLERANCE = 1e-4
WEIGHT_EPSILON = 1e-6


def active(obj):
    if bpy.context.object and bpy.context.object.mode != "OBJECT":
        bpy.ops.object.mode_set(mode="OBJECT")
    bpy.ops.object.select_all(action="DESELECT")
    obj.hide_set(False)
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj


def four_weights(entries):
    entries = sorted(
        ((n, w) for n, w in entries if w > WEIGHT_EPSILON), key=lambda v: v[1], reverse=True
    )[:4]
    total = sum(w for _, w in entries)
    if total <= 0:
        raise ValueError("Unweighted vertex; binding is incomplete")
    return [(n, w / total) for n, w in entries]


def export_profile(
    source,
    out,
    label,
    *,
    mesh_collection,
    rig_name,
    stem,
    asset_id,
    skeleton_id,
    stage,
    material_converter,
    policies=None,
    prepare_actions=None,
    rest_only=False,
    render_group=None,
    properties=AuthoringProperties(),
    root_joint="root",
    initial_clip=None,
    prepare_mesh=None,
    mesh_name=None,
    preserve_hierarchy=False,
    preserve_object_transform=False,
):
    if not rest_only and not (prepare_actions and policies is not None):
        raise ValueError("Animated exports require registered motion and playback policy")
    if not isinstance(root_joint, str) or not root_joint:
        raise ValueError("A root joint name is required")
    bpy.ops.wm.open_mainfile(filepath=str(source), use_scripts=False)
    scene = bpy.context.scene
    scene.render.use_simplify = False
    scene.render.fps = 30
    author = bpy.data.objects[rig_name]
    author.data.pose_position = "POSE"
    for track in author.animation_data.nla_tracks:
        track.mute = True
    candidates = bpy.data.collections[mesh_collection].objects
    objects = [
        o
        for o in candidates
        if o.type == "MESH" and (not o.hide_render or o.get(properties.render_group))
    ]
    if render_group:
        objects = [o for o in objects if o.get(properties.render_group) == render_group]
        if not objects:
            raise ValueError(
                "No meshes in the requested fitted garment render group: " + render_group
            )
    joints = [b for b in author.data.bones if b.use_deform]
    if not joints or any(b.bbone_segments != 1 for b in joints):
        raise ValueError("Exports require deform bones with one segment per bone")
    if preserve_hierarchy and any(b.inherit_scale != "FULL" for b in joints):
        raise ValueError("Hierarchical exports require full inherited scale")
    names = {b.name for b in joints}
    actions = {} if rest_only else prepare_actions()
    from .composition import frame_end

    clips = {c: frame_end(action) for c, action in actions.items()}
    if not rest_only and set(policies) != set(actions):
        raise ValueError("Playback policy must cover exactly the compiled outputs")
    sampler = ActionSampler(author, actions.values(), properties=properties)
    # Only bone evaluation is needed during sampling; avoid repeatedly skinning
    # and subdividing the authoring meshes as a side effect.
    states = [(m, m.show_viewport) for o in objects for m in o.modifiers]
    for modifier, _ in states:
        modifier.show_viewport = False
    samples = {}
    for clip, end in clips.items():
        sampler.select(actions[clip])
        samples[clip] = []
        for frame in range(1, end + 1):
            scene.frame_set(frame)
            samples[clip].append({b.name: author.pose.bones[b.name].matrix.copy() for b in joints})
        print("SAMPLED", label, clip, flush=True)
    for modifier, state in states:
        modifier.show_viewport = state
    author.animation_data.action = None
    author.data.pose_position = "REST"
    bpy.context.view_layer.update()

    collection = bpy.data.collections.new(label + " | runtime bake")
    scene.collection.children.link(collection)
    armature = bpy.data.armatures.new(label + " runtime skeleton")
    runtime = bpy.data.objects.new(label + " runtime skeleton", armature)
    collection.objects.link(runtime)
    active(runtime)
    bpy.ops.object.mode_set(mode="EDIT")
    if root_joint not in names:
        origin = armature.edit_bones.new(root_joint)
        origin.head, origin.tail = (0, 0, 0), (0, 0, 0.1)
    for bone in joints:
        new = armature.edit_bones.new(bone.name)
        # A newly created edit bone has zero length. Setting its matrix before
        # giving it an axis loses the direction; a later length assignment then
        # produces a different bind skeleton despite matching animated matrices.
        new.head, new.tail = bone.head_local, bone.tail_local
        new.matrix = bone.matrix_local
        new.use_deform = True
    origin = armature.edit_bones[root_joint]
    for bone in joints:
        if bone.name == root_joint:
            if bone.parent is not None:
                raise ValueError("The source root joint must have no parent")
            continue
        parent = bone.parent
        while parent and parent.name not in names:
            parent = parent.parent
        # Rigify suppresses inherited scale on several deformation chains.
        # glTF has no equivalent flag: nesting their nonuniform scales can
        # introduce shear. Bake independent world-space joints under one root
        # for skinning; the evaluation contract retains semantic ancestry.
        armature.edit_bones[bone.name].parent = (
            armature.edit_bones[parent.name] if preserve_hierarchy and parent else origin
        )
        armature.edit_bones[bone.name][properties.semantic_parent] = (
            parent.name if parent else root_joint
        )
    bpy.ops.object.mode_set(mode="OBJECT")
    bind_errors = {
        bone.name: max(
            abs(armature.bones[bone.name].matrix_local[i][j] - bone.matrix_local[i][j])
            for i in range(4)
            for j in range(4)
        )
        for bone in joints
    }
    # Allow float32 edit-bone axis reconstruction error within the pose tolerance.
    if not max(bind_errors.values()) < MATRIX_TOLERANCE:
        raise ValueError(f"Runtime bind skeleton differs from source: {bind_errors}")
    skin_names = names | {root_joint}
    baked = []
    for obj in objects:
        for modifier in obj.modifiers:
            if modifier.type == "ARMATURE":
                modifier.show_viewport = False
        if prepare_mesh:
            prepare_mesh(obj)
        bpy.context.view_layer.update()
        graph = bpy.context.evaluated_depsgraph_get()
        mesh = bpy.data.meshes.new_from_object(
            obj.evaluated_get(graph), preserve_all_data_layers=True, depsgraph=graph
        )
        copy = obj.copy()
        copy.data = mesh
        copy.name = mesh_name(label, obj) if mesh_name else label + ".runtime." + obj.name
        copy.data.name = copy.name
        copy.hide_render = False
        copy.hide_set(False)
        collection.objects.link(copy)
        copy.modifiers.clear()
        copy.parent = runtime
        copy.matrix_parent_inverse = Matrix.Identity(4)
        copy.matrix_world = author.matrix_world.inverted() @ obj.matrix_world
        # Subdivision interpolates skin weights and can create more than four
        # influences. Explicitly prune/normalize the baked mesh before export.
        values = [
            four_weights(
                (copy.vertex_groups[g.group].name, g.weight)
                for g in v.groups
                if copy.vertex_groups[g.group].name in skin_names
            )
            for v in mesh.vertices
        ]
        copy.vertex_groups.clear()
        groups = {n: copy.vertex_groups.new(name=n) for n in sorted(skin_names)}
        for i, weights in enumerate(values):
            for n, weight in weights:
                groups[n].add([i], weight, "REPLACE")
        modifier = copy.modifiers.new("Linear skin", "ARMATURE")
        modifier.object = runtime
        modifier.use_deform_preserve_volume = False
        for slot in copy.material_slots:
            slot.material = material_converter(slot.material, mesh)
        # Blender distinguishes the editing-active attribute from the render
        # attribute. glTF's ACTIVE option reads render_color_index.
        if mesh.color_attributes.active_color:
            mesh.color_attributes.render_color_index = mesh.color_attributes.active_color_index
        baked.append(copy)

    if preserve_object_transform:
        runtime.matrix_world = author.matrix_world.copy()

    runtime.animation_data_create()
    baked_actions = {}
    for clip, frames in samples.items():
        action = bpy.data.actions.new(clip)
        action.use_fake_user = True
        runtime.animation_data.action = action
        for frame, matrices in enumerate(frames, 1):
            matrices = {root_joint: Matrix.Identity(4), **matrices}
            for bone in armature.bones:
                parent = bone.parent
                basis = bone.convert_local_to_pose(
                    matrices[bone.name],
                    bone.matrix_local,
                    parent_matrix=(matrices[parent.name] if parent else Matrix.Identity(4)),
                    parent_matrix_local=(parent.matrix_local if parent else Matrix.Identity(4)),
                    invert=True,
                )
                pose = runtime.pose.bones[bone.name]
                pose.rotation_mode = "QUATERNION"
                pose.matrix_basis = basis
                for channel in ("location", "rotation_quaternion", "scale"):
                    pose.keyframe_insert(channel, frame=frame, group=bone.name)
        for curve in all_fcurves(action):
            for point in curve.keyframe_points:
                point.interpolation = "LINEAR"
        runtime.animation_data.action = None
        track = runtime.animation_data.nla_tracks.new()
        track.name = clip
        track.strips.new(clip, 1, action)
        track.mute = True
        baked_actions[clip] = action
    runtime.animation_data.action = (
        baked_actions.get(initial_clip)
        if initial_clip
        else next(iter(baked_actions.values()), None)
    )
    scene.frame_set(1)
    bpy.context.view_layer.update()
    # Check that removing controls and constraints preserved joint motion.
    errors = {}
    for clip, frames in samples.items():
        runtime.animation_data.action = baked_actions[clip]
        maximum = 0
        for frame in (1, (len(frames) + 1) // 2, len(frames)):
            scene.frame_set(frame)
            for name, expected in frames[frame - 1].items():
                actual = runtime.pose.bones[name].matrix
                maximum = max(
                    maximum,
                    max(abs(actual[i][j] - expected[i][j]) for i in range(4) for j in range(4)),
                )
        errors[clip] = maximum
        if not maximum < MATRIX_TOLERANCE:
            raise ValueError(f"{label}: {clip} deformation bake mismatch: {maximum}")
    runtime.animation_data.action = (
        baked_actions.get(initial_clip)
        if initial_clip
        else next(iter(baked_actions.values()), None)
    )
    scene.frame_set(1)
    # Export from an isolated scene. Even selection-only glTF export walks
    # armatures elsewhere in the scene while discovering animation/skin data.
    for obj in list(bpy.data.objects):
        if obj != runtime and obj not in baked:
            bpy.data.objects.remove(obj, do_unlink=True)
    for action in list(bpy.data.actions):
        if action not in baked_actions.values():
            bpy.data.actions.remove(action)
    # Source names may equal export IDs (for example Idle on a creature rig).
    # Reclaim the declared IDs after isolating the baked actions so Blender's
    # temporary .001 collision suffix never becomes part of the runtime contract.
    for clip, action in baked_actions.items():
        action.name = clip
    active(runtime)
    for obj in baked:
        obj.select_set(True)
    output = out / (stem + ".glb")
    bpy.ops.export_scene.gltf(
        filepath=str(output),
        export_format="GLB",
        use_selection=True,
        export_texcoords=True,
        export_normals=True,
        export_materials="EXPORT",
        export_extras=True,
        export_vertex_color="ACTIVE",
        export_all_vertex_colors=False,
        export_animations=bool(baked_actions),
        export_frame_range=False,
        export_force_sampling=True,
        export_animation_mode="ACTIONS",
        export_skins=True,
        export_yup=True,
        export_anim_slide_to_zero=True,
        # Removing a scaled armature folds its scale into the root bind, but
        # Blender's glTF exporter leaves animated root translations unscaled.
        # Keep the object node when its transform is part of the contract.
        export_armature_object_remove=not preserve_object_transform,
        export_cameras=False,
        export_lights=False,
        export_morph=False,
    )
    signature_data = [
        (
            b.name,
            b.parent.name if b.parent else None,
            [list(row) for row in b.matrix_local],
        )
        for b in armature.bones
    ]
    signature = hashlib.sha256(json.dumps(signature_data, sort_keys=True).encode()).hexdigest()
    manifest = {
        "schema_version": 1,
        "asset_id": asset_id,
        "stage": stage,
        "source": source.name,
        "model": output.name,
        "units": "meters",
        "skeleton": {
            "id": skeleton_id,
            "joint_count": len(armature.bones),
            "bind_signature": signature,
        },
        "clips": [
            {
                "name": c,
                **policies[c],
            }
            for c in clips
        ],
        "equipment": [],
    }
    (out / (stem + ".asset.json")).write_text(json.dumps(manifest, indent=2) + "\n")
    # This derivative is useful for independent GLB round-trip comparisons.
    bpy.ops.wm.save_as_mainfile(filepath=str(out / (stem + "-baked.blend")), compress=True)
    return {
        "glb_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
        "joint_bake_max_errors": errors,
        "source_bind_matrix_max_error": max(bind_errors.values()),
        "vertices": sum(len(o.data.vertices) for o in baked),
        "mesh_objects": len(baked),
        "joints": len(armature.bones),
        "manifest": manifest,
    }
