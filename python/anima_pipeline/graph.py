"""Versioned motion DAG and evaluation contracts; no Blender dependency."""

import hashlib
import json
import math
import re
from pathlib import Path


def digest(value):
    return hashlib.sha256(
        json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()
    ).hexdigest()


def file_hash(path):
    checksum = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            checksum.update(chunk)
    return checksum.hexdigest()


def fields(value, required, optional=()):
    if (
        not isinstance(value, dict)
        or set(value) - set(required) - set(optional)
        or set(required) - set(value)
    ):
        raise ValueError(f"Invalid fields: expected {required}, optional {optional}; got {value}")


class MotionGraph:
    def __init__(self, data):
        fields(
            data,
            (
                "version",
                "id",
                "sample_rate",
                "bodies",
                "masks",
                "nodes",
                "exports",
                "layers",
                "playback",
                "evaluation",
            ),
            ("equipment",),
        )
        for name, gear in data.get("equipment", {}).items():
            if not re.fullmatch(r"[a-zA-Z0-9][a-zA-Z0-9_.-]*", name):
                raise ValueError("Equipment IDs must be safe stable file stems")
            fields(gear, ("slot", "render_group"))
            if not all(
                isinstance(v, str) and re.fullmatch(r"[a-zA-Z0-9][a-zA-Z0-9_.-]*", v)
                for v in gear.values()
            ):
                raise ValueError("Equipment needs an explicit slot and render group")
        if "evaluation" in data:
            path = Path(data["evaluation"])
            if path.is_absolute() or ".." in path.parts or path.suffix != ".json":
                raise ValueError("Evaluation definition must be a repository-relative JSON path")
        if data["version"] != 2 or data["sample_rate"] != 30:
            raise ValueError("Unsupported motion contract version or sample rate")
        if not data["id"] or not data["bodies"] or not data["exports"]:
            raise ValueError("Contract identity, body adapters and exports are required")
        for name, body in data["bodies"].items():
            if not isinstance(name, str) or not re.fullmatch(r"[a-zA-Z0-9][a-zA-Z0-9_.-]*", name):
                raise ValueError("Body IDs must be safe stable file stems")
            strings = ("label", "rig", "mesh_collection", "asset_id", "skeleton_id", "stage")
            options = ("preserve_hierarchy", "preserve_object_transform")
            fields(body, strings, options)
            if not all(isinstance(body[key], str) and body[key] for key in strings):
                raise ValueError("Body adapter values must be nonempty strings")
            if any(key in body and not isinstance(body[key], bool) for key in options):
                raise ValueError("Body export preservation options must be boolean")
        for mask in data["masks"].values():
            if (
                not isinstance(mask, list)
                or not mask
                or len(mask) != len(set(mask))
                or not all(isinstance(n, str) and n for n in mask)
            ):
                raise ValueError("Masks must contain unique, explicit control names")
        self.data = data
        self.nodes = {}
        for node in data["nodes"]:
            kind = node.get("kind")
            if kind == "source":
                fields(node, ("id", "kind", "action"), ("mask", "contact_space"))
                if "mask" in node and node["mask"] not in data["masks"]:
                    raise ValueError(f"Unknown mask: {node['mask']}")
                if "contact_space" in node:
                    space = node["contact_space"]
                    fields(space, ("anchor", "controls", "sampling"))
                    controls = space["controls"]
                    mask = data["masks"].get(node.get("mask"), [])
                    if (
                        space["sampling"] != "baked_30hz"
                        or not isinstance(space["anchor"], str)
                        or not space["anchor"]
                        or space["anchor"] in mask
                        or not isinstance(controls, list)
                        or not controls
                        or not all(isinstance(n, str) and n in mask for n in controls)
                        or len(controls) != len(set(controls))
                    ):
                        raise ValueError(
                            "Contact space needs an unowned anchor and unique owned controls at 30 Hz"
                        )
            elif kind == "compose":
                fields(node, ("id", "kind", "base", "overlay", "phase"))
                if node["phase"] != "normalized":
                    raise ValueError("Composition requires explicit normalized phase mapping")
            else:
                raise ValueError(f"Unsupported node kind: {kind}")
            if not node["id"] or node["id"] in self.nodes:
                raise ValueError(f"Duplicate/empty node identity: {node['id']}")
            self.nodes[node["id"]] = node
        self.order = []
        visiting = set()

        def visit(name):
            if name not in self.nodes:
                raise ValueError(f"Missing dependency: {name}")
            if name in visiting:
                raise ValueError(f"Cyclic motion dependency: {name}")
            if name in self.order:
                return
            visiting.add(name)
            for dependency in self.dependencies(name):
                visit(dependency)
            visiting.remove(name)
            self.order.append(name)

        for name in sorted(self.nodes):
            visit(name)
        for node in self.nodes.values():
            if node["kind"] == "compose":
                if "mask" in self.nodes[node["base"]]:
                    raise ValueError("Composition requires a full-pose base")
                overlay = self.nodes[node["overlay"]]
                if overlay["kind"] != "source" or "mask" not in overlay:
                    raise ValueError("An overlay must be an explicitly masked source")
        if not isinstance(data["layers"], dict) or not isinstance(data["playback"], dict):
            raise ValueError("Layers and playback must be explicit mappings")
        if set(data["playback"]) != set(data["exports"]):
            raise ValueError("Playback policy must cover every base motion/action")
        for clip, name in data["exports"].items():
            if (
                name not in self.nodes
                or self.nodes[name]["kind"] != "source"
                or "mask" in self.nodes[name]
            ):
                raise ValueError("Base motion exports require a full-pose source")
            policy = data["playback"][clip]
            fields(policy, ("loop", "events"), ("reference_speed", "in_place"))
            if not isinstance(policy["loop"], bool) or not isinstance(policy["events"], list):
                raise ValueError("Invalid playback policy")
            speed = policy.get("reference_speed", 1)
            if not isinstance(speed, (int, float)) or not math.isfinite(speed) or speed <= 0:
                raise ValueError("Invalid reference speed")
            previous = -1
            for cue in policy["events"]:
                fields(cue, ("time", "name"))
                if (
                    not isinstance(cue["time"], (int, float))
                    or not math.isfinite(cue["time"])
                    or cue["time"] < 0
                    or cue["time"] < previous
                    or not cue["name"]
                ):
                    raise ValueError("Invalid motion cue")
                previous = cue["time"]
        for owner, reference in data["layers"].items():
            if (
                owner not in self.nodes
                or reference not in self.nodes
                or self.nodes[reference]["kind"] != "compose"
                or self.nodes[reference]["overlay"] != owner
            ):
                raise ValueError("Layer requires its explicit compiled reference context")
        reachable = set()

        def mark(name):
            if name in reachable:
                return
            reachable.add(name)
            for child in self.dependencies(name):
                mark(child)

        for name in (*data["exports"].values(), *data["layers"].values()):
            mark(name)
        if reachable != set(self.nodes):
            raise ValueError("Unowned or unused motion source")

    def dependencies(self, name):
        node = self.nodes[name]
        return [node[k] for k in ("base", "overlay") if k in node]

    def keys(self, payloads, body, rig_key):
        if body not in self.data["bodies"]:
            raise ValueError(f"Unknown body adapter: {body}")
        expected = {n for n, v in self.nodes.items() if v["kind"] == "source"}
        if set(payloads) != expected:
            raise ValueError("Motion fingerprints do not cover every source exactly once")
        keys = {}
        for name in self.order:
            node = self.nodes[name]
            definition = node
            mask = self.data["masks"].get(node.get("mask"))
            keys[name] = digest(
                [
                    self.data["version"],
                    self.data["sample_rate"],
                    body,
                    self.data["bodies"][body],
                    rig_key,
                    definition,
                    mask,
                    payloads.get(name),
                    {d: keys[d] for d in self.dependencies(name)},
                ]
            )
        return keys


def load_graph(path):
    return MotionGraph(json.loads(Path(path).read_text()))


def validate_evaluation(data):
    fields(data, ("version", "id", "parents", "masks", "chains"))
    if data["version"] != 1 or not isinstance(data["id"], str) or not data["id"]:
        raise ValueError("Invalid evaluation identity")
    parents = data["parents"]
    if not isinstance(parents, dict) or not 1 <= len(parents) <= 512:
        raise ValueError("Invalid evaluation joint count")

    def ancestry(name):
        result = []
        while name is not None:
            if not isinstance(name, str) or not name or name not in parents or name in result:
                raise ValueError("Missing or cyclic evaluation joint")
            result.append(name)
            name = parents[name]
        return result

    chains = {name: ancestry(name) for name in parents}
    for name, roots in data["masks"].items():
        if (
            not name
            or not isinstance(roots, list)
            or not roots
            or any(n not in parents for n in roots)
            or len(set(roots)) != len(roots)
        ):
            raise ValueError("Invalid evaluation mask roots")
    for name, chain in data["chains"].items():
        fields(chain, ("joints", "minimum_angle", "maximum_angle"))
        joints = chain["joints"]
        if (
            not name
            or len(joints) != 3
            or len(set(joints)) != 3
            or any(n not in parents for n in joints)
        ):
            raise ValueError("Invalid contact chain joints")
        if joints[0] not in chains[joints[1]] or joints[1] not in chains[joints[2]]:
            raise ValueError("Contact chain must follow evaluation ancestry")
        lower, upper = chain["minimum_angle"], chain["maximum_angle"]
        if (
            not all(isinstance(v, (int, float)) and math.isfinite(v) for v in (lower, upper))
            or not 0 <= lower < upper <= math.pi
        ):
            raise ValueError("Invalid contact angle limits")
    return data
