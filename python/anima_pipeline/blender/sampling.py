"""Evaluate saved rig actions without inheriting unkeyed values from another clip.

Capture once after opening the source and muting its NLA tracks. Actions are
partial poses: Blender deliberately retains channels that an action does not
key, including Rigify IK/FK switches. Each selection starts from the same saved
state; switches that the selected action *does* key still take precedence.
"""

import json
import re

from ..properties import AuthoringProperties


def all_fcurves(action):
    for layer in action.layers:
        for strip in layer.strips:
            for bag in strip.channelbags:
                yield from bag.fcurves


class ActionSampler:
    def __init__(self, rig, actions, *, properties=AuthoringProperties()):
        self.properties = properties
        self.rig = rig
        actions = tuple(actions)
        frozen = (
            json.loads(rig[self.properties.sampling_defaults])
            if self.properties.sampling_defaults in rig
            else None
        )
        if frozen is not None and frozen.get("version") != 1:
            raise ValueError("Unsupported saved animation defaults version")
        saved = frozen["values"] if frozen is not None else {}
        paths = set()
        transforms = (
            "location",
            "rotation_mode",
            "rotation_euler",
            "rotation_quaternion",
            "rotation_axis_angle",
            "scale",
        )
        for owner in (rig, *rig.pose.bones):
            paths.update(owner.path_from_id(name) for name in transforms)
            for key, value in owner.items():
                if owner == rig and key == self.properties.sampling_defaults:
                    continue
                if isinstance(value, (int, float, str)) or hasattr(value, "to_list"):
                    prefix = "" if owner == rig else owner.path_from_id()
                    paths.add(prefix + "[" + json.dumps(key) + "]")
        animated = {curve.data_path for action in actions for curve in all_fcurves(action)}
        if frozen is not None and animated - saved.keys():
            raise ValueError(
                "New animated channels need explicit defaults: "
                + ", ".join(sorted(animated - saved.keys()))
            )
        paths.update(animated)
        self.state = []
        self.defaults = {}
        for path in sorted(paths):
            value = rig.path_resolve(path)
            if not isinstance(value, (int, float, str)):
                value = tuple(value)
            custom = re.fullmatch(r'(.*)\["((?:\\.|[^"\\])*)"\]', path)
            if custom:
                parent_path, escaped = custom.groups()
                parent = rig.path_resolve(parent_path) if parent_path else rig
                key = json.loads('"' + escaped + '"')
                # RNA may expose an integer ID property as an enum label.
                # Restore its stored value through the same ID-property API.
                value = parent[key]
                if not isinstance(value, (int, float, str)):
                    value = tuple(value)
                value = saved.get(path, value)
                self.state.append((parent, key, value, True))
            else:
                parent_path, _, name = path.rpartition(".")
                parent = rig.path_resolve(parent_path) if parent_path else rig
                value = saved.get(path, value)
                self.state.append((parent, name, value, False))
            self.defaults[path] = value

    def freeze_defaults(self):
        """Store the source defaults before posing, for independent save/reopen/export.

        Only explicit authoring-workspace preparation calls this. Existing source
        files retain their original fresh-open behavior and are never rewritten.
        """
        self.rig[self.properties.sampling_defaults] = json.dumps(
            {"version": 1, "values": self.defaults}, separators=(",", ":")
        )

    def select(self, action):
        self.rig.animation_data.action = None
        for owner, name, value, custom in self.state:
            if custom:
                owner[name] = value
            else:
                setattr(owner, name, value)
        self.rig.animation_data.action = action
