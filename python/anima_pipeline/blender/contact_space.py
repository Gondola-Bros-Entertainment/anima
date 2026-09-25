"""Extract a bounded handling layer and adapt its targets to the base pose.

Layer channels and anchor-relative target frames are baked at the declared
30 Hz source rate before normalized-phase resampling. No donor torso/leg
channels enter the layer. Blender's rig solves its existing IK constraints;
this module does not implement the eventual runtime contact solver.
"""

import json
import math
from functools import wraps

import bpy
from mathutils import Matrix

from .sampling import ActionSampler
from ..properties import AuthoringProperties


def without_deformation(function):
    @wraps(function)
    def evaluate(rig, *args, **kwargs):
        modifiers = [
            (modifier, modifier.show_viewport)
            for obj in bpy.data.objects
            if obj.type == "MESH"
            and any(m.type == "ARMATURE" and m.object == rig for m in obj.modifiers)
            for modifier in obj.modifiers
        ]
        try:
            for modifier, _ in modifiers:
                modifier.show_viewport = False
            return function(rig, *args, **kwargs)
        finally:
            for modifier, visible in modifiers:
                modifier.show_viewport = visible

    return evaluate


def matrix_values(matrix):
    return [list(row) for row in matrix]


def matrix_error(a, b):
    return max(abs(a[i][j] - b[i][j]) for i in range(4) for j in range(4))


@without_deformation
def extract(rig, action, source_curves, space, *, properties=AuthoringProperties()):
    end = round(action.frame_range[1])
    anchor = rig.pose.bones[space["anchor"]]
    for name in space["controls"]:
        control = rig.pose.bones[name]
        if control.constraints or control.rotation_mode != "QUATERNION":
            raise ValueError(f"Contact control needs unconstrained quaternion transforms: {name}")
    layer = {
        "version": 1,
        "sample_rate": 30,
        "frames": end,
        "space": json.loads(json.dumps(space)),
        "channels": [
            {
                "path": curve.data_path,
                "index": curve.array_index,
                "values": [curve.evaluate(f) for f in range(1, end + 1)],
            }
            for curve in sorted(source_curves, key=lambda c: (c.data_path, c.array_index))
        ],
        "targets": {name: [] for name in space["controls"]},
    }
    sampler = ActionSampler(rig, (action,), properties=properties)
    sampler.select(action)
    for frame in range(1, end + 1):
        bpy.context.scene.frame_set(frame)
        inverse = anchor.matrix.inverted()
        for name, samples in layer["targets"].items():
            samples.append(matrix_values(inverse @ rig.pose.bones[name].matrix))
    return layer


def sample_indices(frame, end, source_end):
    value = (frame - 1) * (source_end - 1) / (end - 1)
    first = math.floor(value)
    return first, min(first + 1, source_end - 1), value - first


def add_curve(bag, path, index, samples):
    curve = bag.fcurves.new(path, index=index)
    curve.keyframe_points.add(len(samples))
    for frame, (point, value) in enumerate(zip(curve.keyframe_points, samples), 1):
        point.co = frame, value
        point.interpolation = "LINEAR"
    curve.update()


@without_deformation
def compose(rig, result, bag, layer, end, *, properties=AuthoringProperties()):
    """The caller has copied the base and removed only the layer's owned curves."""
    for channel in layer["channels"]:
        values = channel["values"]
        samples = []
        for frame in range(1, end + 1):
            first, second, weight = sample_indices(frame, end, layer["frames"])
            samples.append(values[first] * (1 - weight) + values[second] * weight)
        add_curve(bag, channel["path"], channel["index"], samples)
    controls = layer["space"]["controls"]
    baked = {name: [] for name in controls}
    previous_rotations = {}
    sampler = ActionSampler(rig, (result,), properties=properties)
    maximum_error = 0
    # Collect before replacing any curves so frame traversal cannot feed the
    # previous frame's generated correction back into this build.
    for frame in range(1, end + 1):
        sampler.select(result)
        bpy.context.scene.frame_set(frame)
        anchor = rig.pose.bones[layer["space"]["anchor"]].matrix.copy()
        first, second, weight = sample_indices(frame, end, layer["frames"])
        goals = {}
        for name in controls:
            samples = layer["targets"][name]
            relative = Matrix(samples[first]).lerp(Matrix(samples[second]), weight)
            control = rig.pose.bones[name]
            goals[name] = anchor @ relative
            kwargs = (
                {
                    "parent_matrix": control.parent.matrix,
                    "parent_matrix_local": control.parent.bone.matrix_local,
                }
                if control.parent
                else {}
            )
            basis = control.bone.convert_local_to_pose(
                goals[name], control.bone.matrix_local, invert=True, **kwargs
            )
            control.matrix_basis = basis
            if matrix_error(control.matrix_basis, basis) > 1e-5:
                raise ValueError(
                    f"Contact conversion would discard local shear: {name}, frame {frame}"
                )
        bpy.context.view_layer.update()
        for name in controls:
            control = rig.pose.bones[name]
            error = matrix_error(control.matrix, goals[name])
            maximum_error = max(maximum_error, error)
            if error > 1e-5:
                raise ValueError(
                    f"Contact target was not reproduced: {name}, frame {frame}, error {error}"
                )
            rotation = control.rotation_quaternion.copy()
            if name in previous_rotations and rotation.dot(previous_rotations[name]) < 0:
                rotation.negate()
            previous_rotations[name] = rotation
            baked[name].append((tuple(control.location), tuple(rotation), tuple(control.scale)))
    paths = {
        rig.pose.bones[name].path_from_id(prop)
        for name in controls
        for prop in ("location", "rotation_quaternion", "scale")
    }
    for curve in list(bag.fcurves):
        if curve.data_path in paths:
            bag.fcurves.remove(curve)
    for name, frames in baked.items():
        control = rig.pose.bones[name]
        for field, (prop, count) in enumerate(
            (("location", 3), ("rotation_quaternion", 4), ("scale", 3))
        ):
            for index in range(count):
                add_curve(
                    bag,
                    control.path_from_id(prop),
                    index,
                    [sample[field][index] for sample in frames],
                )
    result[properties.contact_space] = json.dumps(layer["space"], sort_keys=True)
    return {
        "source_frames": layer["frames"],
        "output_frames": end,
        "target_controls": controls,
        "max_target_matrix_error": maximum_error,
    }
