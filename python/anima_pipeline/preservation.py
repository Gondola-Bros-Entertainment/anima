"""Require an animation extension to preserve an accepted character GLB exactly.

Compare decoded accessor payloads, independent of buffer placement. Includes
mesh attributes, indices, materials, joints/bind matrices and existing clips.
"""

import argparse
import copy
import hashlib
import json
from pathlib import Path

from .resources import read_glb


def content(path):
    gltf, binary = read_glb(path)

    def accessor(index):
        value = gltf["accessors"][index]
        view = gltf["bufferViews"][value["bufferView"]]
        scalar = {5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4}[value["componentType"]]
        count = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}[value["type"]]
        width = scalar * count
        offset = view.get("byteOffset", 0) + value.get("byteOffset", 0)
        stride = view.get("byteStride", width)
        payload = b"".join(
            binary[offset + k * stride : offset + k * stride + width] for k in range(value["count"])
        )
        return (
            value["type"],
            value["componentType"],
            value["count"],
            value.get("normalized", False),
            hashlib.sha256(payload).hexdigest(),
        )

    def texture(index):
        value = copy.deepcopy(gltf["textures"][index])
        image = copy.deepcopy(gltf["images"][value.pop("source")])
        if "bufferView" not in image or "uri" in image:
            raise ValueError("Preservation requires embedded image data")
        view = gltf["bufferViews"][image.pop("bufferView")]
        offset = view.get("byteOffset", 0)
        payload = binary[offset : offset + view["byteLength"]]
        if view.get("buffer", 0) != 0 or len(payload) != view["byteLength"]:
            raise ValueError("Invalid embedded image buffer")
        image["sha256"] = hashlib.sha256(payload).hexdigest()
        value["source"] = image
        if "sampler" in value:
            value["sampler"] = copy.deepcopy(gltf["samplers"][value["sampler"]])
        return value

    def material(value):
        # Splitting resources can renumber textures, images and samplers.
        # Compare their referenced content, including image bytes, rather than
        # accepting/rejecting a material based on its local array indices.
        if isinstance(value, dict):
            return {
                key: (
                    {**child, "index": texture(child["index"])}
                    if key.endswith("Texture") and isinstance(child, dict) and "index" in child
                    else material(child)
                )
                for key, child in value.items()
            }
        if isinstance(value, list):
            return [material(child) for child in value]
        return value

    nodes = gltf["nodes"]
    meshes = {
        node["name"]: [
            {
                "attributes": {
                    name: accessor(index) for name, index in primitive["attributes"].items()
                },
                "indices": accessor(primitive["indices"]) if "indices" in primitive else None,
                "material": material(gltf["materials"][primitive["material"]])
                if "material" in primitive
                else None,
            }
            for primitive in gltf["meshes"][node["mesh"]]["primitives"]
        ]
        for node in nodes
        if "mesh" in node
    }
    transforms = {
        node["name"]: {
            key: node[key] for key in ("matrix", "translation", "rotation", "scale") if key in node
        }
        for node in nodes
    }
    clips = {
        clip["name"]: sorted(
            [
                (
                    nodes[channel["target"]["node"]]["name"],
                    channel["target"]["path"],
                    accessor(clip["samplers"][channel["sampler"]]["input"]),
                    accessor(clip["samplers"][channel["sampler"]]["output"]),
                    clip["samplers"][channel["sampler"]].get("interpolation", "LINEAR"),
                )
                for channel in clip["channels"]
            ]
        )
        for clip in gltf.get("animations", [])
    }
    skins = [
        (
            list(nodes[index]["name"] for index in skin["joints"]),
            accessor(skin["inverseBindMatrices"]),
        )
        for skin in gltf.get("skins", [])
    ]
    return meshes, clips, skins, transforms


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("accepted", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument(
        "--allow-clip-change",
        action="append",
        default=[],
        help="Explicitly permit an intentional animation revision; geometry checks remain strict",
    )
    args = parser.parse_args()
    accepted, candidate = content(args.accepted), content(args.candidate)
    report = {
        "meshes_unchanged": accepted[0] == candidate[0],
        "skins_unchanged": accepted[2] == candidate[2],
        "transforms_unchanged": accepted[3] == candidate[3],
        "original_clips": {
            name: clip == candidate[1].get(name) for name, clip in accepted[1].items()
        },
        "new_clips": sorted(set(candidate[1]) - set(accepted[1])),
        "allowed_clip_changes": sorted(set(args.allow_clip_change)),
    }
    args.out.write_text(json.dumps(report, indent=2) + "\n")
    if not all(
        report[key] for key in ("meshes_unchanged", "skins_unchanged", "transforms_unchanged")
    ):
        raise ValueError("Candidate changes geometry, materials, bind skeleton or transforms")
    if not set(args.allow_clip_change) <= set(accepted[1]):
        raise ValueError("Unknown allowlisted clip")
    if not set(accepted[1]) <= set(candidate[1]):
        raise ValueError("Removed original clip")
    if not all(
        unchanged or name in args.allow_clip_change
        for name, unchanged in report["original_clips"].items()
    ):
        raise ValueError("Candidate changes an original clip without explicit permission")
    print(
        "PASS preserved character geometry, materials, bind skeleton and all non-allowlisted animations:",
        args.candidate,
    )
