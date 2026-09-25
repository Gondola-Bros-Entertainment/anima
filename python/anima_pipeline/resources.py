"""Split a compiler reference bake into independent production resources.

The reference bake is qualification evidence only. Production body models have
no animations/equippable garments; motion resources have no render payloads.
"""

import copy
import json
import struct
from pathlib import Path

_GLB_HEADER = struct.Struct("<4sII")
_CHUNK_HEADER = struct.Struct("<II")
_GLB_VERSION = 2
_JSON_CHUNK = int.from_bytes(b"JSON", "little")
_BINARY_CHUNK = int.from_bytes(b"BIN\0", "little")


def read_glb(path):
    data = Path(path).read_bytes()
    if _GLB_HEADER.unpack_from(data) != (b"glTF", _GLB_VERSION, len(data)):
        raise ValueError("Expected a complete GLB v2")
    size, kind = _CHUNK_HEADER.unpack_from(data, _GLB_HEADER.size)
    if kind != _JSON_CHUNK:
        raise ValueError("Missing GLB JSON")
    json_start = _GLB_HEADER.size + _CHUNK_HEADER.size
    json_end = json_start + size
    value = json.loads(data[json_start:json_end])
    length, kind = _CHUNK_HEADER.unpack_from(data, json_end)
    binary_start = json_end + _CHUNK_HEADER.size
    if kind != _BINARY_CHUNK or binary_start + length != len(data) or len(value["buffers"]) != 1:
        raise ValueError("Resources require one embedded GLB buffer")
    return value, data[binary_start:]


def write_glb(path, value, binary):
    value["buffers"] = [{"byteLength": len(binary)}]
    encoded = json.dumps(value, separators=(",", ":")).encode()
    encoded += b" " * (-len(encoded) % 4)
    binary += b"\0" * (-len(binary) % 4)
    total_size = _GLB_HEADER.size + 2 * _CHUNK_HEADER.size + len(encoded) + len(binary)
    Path(path).write_bytes(
        _GLB_HEADER.pack(b"glTF", _GLB_VERSION, total_size)
        + _CHUNK_HEADER.pack(len(encoded), _JSON_CHUNK)
        + encoded
        + _CHUNK_HEADER.pack(len(binary), _BINARY_CHUNK)
        + binary
    )


def subset(gltf, binary, mesh_nodes=(), animations=(), skin=False):
    """Copy referenced payloads exactly and rebuild every affected index."""
    mesh_nodes = set(mesh_nodes)
    animations = copy.deepcopy(list(animations))
    parents = {
        child: i for i, node in enumerate(gltf["nodes"]) for child in node.get("children", [])
    }
    keep = set(mesh_nodes)
    # Motion resources retain the bind skeleton, including joints without keys.
    for binding in gltf.get("skins", []):
        keep.update(binding["joints"])
        if "skeleton" in binding:
            keep.add(binding["skeleton"])
    for animation in animations:
        keep.update(c["target"]["node"] for c in animation["channels"])
    for node in list(keep):
        while node in parents:
            node = parents[node]
            keep.add(node)
    node_map = {old: new for new, old in enumerate(sorted(keep))}
    mesh_ids = sorted({gltf["nodes"][i]["mesh"] for i in mesh_nodes})
    mesh_map = {old: new for new, old in enumerate(mesh_ids)}
    meshes = copy.deepcopy([gltf["meshes"][i] for i in mesh_ids])
    nodes = copy.deepcopy([gltf["nodes"][i] for i in sorted(keep)])
    for old, node in zip(sorted(keep), nodes):
        children = [node_map[n] for n in node.get("children", []) if n in keep]
        node.pop("children", None)
        if children:
            node["children"] = children
        if old in mesh_nodes:
            node["mesh"] = mesh_map[node["mesh"]]
        else:
            node.pop("mesh", None)
            node.pop("skin", None)
    skins = copy.deepcopy(gltf.get("skins", [])) if skin else []
    accessors, material_ids = set(), set()
    for mesh in meshes:
        for primitive in mesh["primitives"]:
            if primitive.get("extensions"):
                raise ValueError(
                    "Compressed primitives require decompression before resource packaging"
                )
            accessors.update(primitive["attributes"].values())
            for target in primitive.get("targets", []):
                accessors.update(target.values())
            if "indices" in primitive:
                accessors.add(primitive["indices"])
            if "material" in primitive:
                material_ids.add(primitive["material"])
    for binding in skins:
        accessors.add(binding["inverseBindMatrices"])
        binding["joints"] = [node_map[n] for n in binding["joints"]]
        if "skeleton" in binding:
            binding["skeleton"] = node_map[binding["skeleton"]]
    for animation in animations:
        for channel in animation["channels"]:
            channel["target"]["node"] = node_map[channel["target"]["node"]]
        used = sorted({c["sampler"] for c in animation["channels"]})
        remap = {old: new for new, old in enumerate(used)}
        animation["samplers"] = [animation["samplers"][i] for i in used]
        for channel in animation["channels"]:
            channel["sampler"] = remap[channel["sampler"]]
        for sampler in animation["samplers"]:
            accessors.update((sampler["input"], sampler["output"]))
    material_map = {old: new for new, old in enumerate(sorted(material_ids))}
    materials = copy.deepcopy([gltf["materials"][i] for i in sorted(material_ids)])
    texture_ids = set()

    def texture_fields(value):
        if isinstance(value, dict):
            for key, child in value.items():
                if key.endswith("Texture") and isinstance(child, dict) and "index" in child:
                    yield child
                else:
                    yield from texture_fields(child)
        elif isinstance(value, list):
            for child in value:
                yield from texture_fields(child)

    for field in texture_fields(materials):
        texture_ids.add(field["index"])
    texture_map = {old: new for new, old in enumerate(sorted(texture_ids))}
    for field in texture_fields(materials):
        field["index"] = texture_map[field["index"]]
    textures = copy.deepcopy([gltf["textures"][i] for i in sorted(texture_ids)])
    if any(t.get("extensions") for t in textures):
        raise ValueError("Texture resource extensions require an explicit packaging adapter")
    image_ids = sorted({t["source"] for t in textures})
    image_map = {old: new for new, old in enumerate(image_ids)}
    images = copy.deepcopy([gltf["images"][i] for i in image_ids])
    sampler_ids = sorted({t["sampler"] for t in textures if "sampler" in t})
    sampler_map = {old: new for new, old in enumerate(sampler_ids)}
    for texture in textures:
        texture["source"] = image_map[texture["source"]]
        if "sampler" in texture:
            texture["sampler"] = sampler_map[texture["sampler"]]
    accessor_map = {old: new for new, old in enumerate(sorted(accessors))}
    values = copy.deepcopy([gltf["accessors"][i] for i in sorted(accessors)])
    if any("sparse" in v or "bufferView" not in v for v in values):
        raise ValueError("Sparse accessors require expansion before resource packaging")
    if any("bufferView" not in image for image in images):
        raise ValueError("Production resource textures must be embedded")
    view_ids = sorted({v["bufferView"] for v in values} | {i["bufferView"] for i in images})
    view_map = {old: new for new, old in enumerate(view_ids)}
    views, payload = [], bytearray()
    for old in view_ids:
        view = copy.deepcopy(gltf["bufferViews"][old])
        if view.get("buffer", 0) != 0 or view.get("extensions"):
            raise ValueError("Unsupported resource buffer layout")
        offset = view.get("byteOffset", 0)
        chunk = binary[offset : offset + view["byteLength"]]
        if len(chunk) != view["byteLength"]:
            raise ValueError("Truncated resource buffer")
        payload += b"\0" * (-len(payload) % 4)
        view["byteOffset"] = len(payload)
        payload += chunk
        views.append(view)
    for value in values:
        value["bufferView"] = view_map[value["bufferView"]]
    for image in images:
        image["bufferView"] = view_map[image["bufferView"]]
    for mesh in meshes:
        for primitive in mesh["primitives"]:
            primitive["attributes"] = {
                k: accessor_map[v] for k, v in primitive["attributes"].items()
            }
            for target in primitive.get("targets", []):
                for key, value in target.items():
                    target[key] = accessor_map[value]
            if "indices" in primitive:
                primitive["indices"] = accessor_map[primitive["indices"]]
            if "material" in primitive:
                primitive["material"] = material_map[primitive["material"]]
    for binding in skins:
        binding["inverseBindMatrices"] = accessor_map[binding["inverseBindMatrices"]]
    for animation in animations:
        for sampler in animation["samplers"]:
            for key in ("input", "output"):
                sampler[key] = accessor_map[sampler[key]]
    result = {
        "asset": gltf["asset"],
        "scene": 0,
        "scenes": [{"nodes": [node_map[i] for i in sorted(keep) if i not in parents]}],
        "nodes": nodes,
        "accessors": values,
        "bufferViews": views,
    }
    for key, value in {
        "meshes": meshes,
        "skins": skins,
        "animations": animations,
        "materials": materials,
        "textures": textures,
        "images": images,
        "samplers": [gltf["samplers"][i] for i in sampler_ids],
    }.items():
        if value:
            result[key] = value
    used = set()

    def extensions(value):
        if isinstance(value, dict):
            used.update(value.get("extensions", {}))
            for child in value.values():
                extensions(child)
        elif isinstance(value, list):
            for child in value:
                extensions(child)

    extensions(result)
    if used:
        result["extensionsUsed"] = sorted(used)
    required = used & set(gltf.get("extensionsRequired", []))
    if required:
        result["extensionsRequired"] = sorted(required)
    return result, bytes(payload)


def package_actor(
    reference, graph, manifest, body, directory, evaluation, *, group_property="anima.render_group"
):
    from .graph import validate_evaluation

    validate_evaluation(evaluation)
    gltf, binary = read_glb(reference)
    animations = {a["name"]: a for a in gltf["animations"]}
    mesh_nodes = {i for i, n in enumerate(gltf["nodes"]) if "mesh" in n}
    gear_nodes = {}
    for item, gear in graph.data.get("equipment", {}).items():
        gear_nodes[item] = {
            i
            for i in mesh_nodes
            if gltf["nodes"][i].get("extras", {}).get(group_property) == gear["render_group"]
        }
        if not gear_nodes[item]:
            raise ValueError("Missing declared garment group: " + item)
    body_nodes = mesh_nodes - set().union(*gear_nodes.values()) if gear_nodes else mesh_nodes
    write_glb(directory / (body + ".glb"), *subset(gltf, binary, body_nodes, skin=True))
    outputs = [directory / (body + ".glb")]
    for item, nodes in gear_nodes.items():
        path = directory / (body + "." + item + ".glb")
        write_glb(path, *subset(gltf, binary, nodes, skin=True))
        outputs.append(path)
    sources, clips, layers = [], [], {}
    for name in graph.data["exports"]:
        sources.append(copy.deepcopy(animations[name]))
        clips.append({"name": name, **graph.data["playback"][name]})
    for owner in graph.data["layers"]:
        node = graph.nodes[owner]
        mask = node["mask"]
        roots = set(evaluation["masks"][mask])
        owned = set()
        for name in evaluation["parents"]:
            current = name
            while current:
                if current in roots:
                    owned.add(name)
                    break
                current = evaluation["parents"][current]
        context = {
            evaluation["parents"][name]
            for name in owned
            if evaluation["parents"][name] is not None and evaluation["parents"][name] not in owned
        }
        animation = copy.deepcopy(animations[owner])
        animation["name"] = owner
        animation["channels"] = [
            c
            for c in animation["channels"]
            if gltf["nodes"][c["target"]["node"]]["name"] in owned | context
        ]
        if not animation["channels"]:
            raise ValueError("Handling layer has no owned channels: " + owner)
        sources.append(animation)
        layers[owner] = {
            "mask": mask,
            "context_joints": sorted(context),
            "owned_joints": sorted(owned),
        }
    motion_path = directory / (body + ".motion.glb")
    write_glb(motion_path, *subset(gltf, binary, animations=sources))
    outputs.append(motion_path)
    contract = {
        "version": 3,
        "skeleton": manifest["skeleton"],
        "evaluation": evaluation,
        "recipe_id": graph.data["id"],
        "resource": motion_path.name,
        "clips": clips,
        "layers": layers,
    }
    contract_path = directory / (body + ".motion.json")
    contract_path.write_text(json.dumps(contract, indent=2) + "\n")
    outputs.append(contract_path)
    manifest["clips"] = []
    manifest["equipment"] = []
    manifest["motion_contract"] = contract_path.name
    return outputs, contract
