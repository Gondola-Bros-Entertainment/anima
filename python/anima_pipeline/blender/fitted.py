"""Compile one fitted mesh without rebuilding its owner's motion."""

import json
from pathlib import Path
from ..graph import load_graph, file_hash
from ..properties import AuthoringProperties
from .bake import export_profile
import bpy


def export_fitted(
    source,
    recipe,
    body,
    profile,
    group,
    item,
    slot,
    output,
    *,
    context,
    material_converter,
    properties=AuthoringProperties(),
    root_joint="root",
    prepare_mesh=None,
    mesh_name=None,
):
    if not item or any(c not in "abcdefghijklmnopqrstuvwxyz0123456789._-" for c in item):
        raise ValueError("Fitted item ID must be a safe filename")
    if not profile or not slot or not group:
        raise ValueError("Fitted exports require a profile, slot and source group")
    source, recipe, output = map(lambda path: Path(path).resolve(), (source, recipe, output))
    before = context.build_inputs(source, recipe)
    adapter = load_graph(recipe).data["bodies"][body]
    output.mkdir(parents=True, exist_ok=False)
    stem = body + "." + item
    result = export_profile(
        source,
        output,
        adapter["label"],
        mesh_collection=adapter["mesh_collection"],
        rig_name=adapter["rig"],
        material_converter=material_converter,
        stem=stem,
        asset_id=item + "." + body,
        skeleton_id=adapter["skeleton_id"],
        stage="fitted_equipment_candidate",
        rest_only=True,
        render_group=group,
        properties=properties,
        root_joint=root_joint,
        prepare_mesh=prepare_mesh,
        mesh_name=mesh_name,
        preserve_hierarchy=adapter.get("preserve_hierarchy", False),
        preserve_object_transform=adapter.get("preserve_object_transform", False),
    )
    manifest = result["manifest"]
    fit = {
        "id": item,
        "slot": slot,
        "fits": {
            profile: {
                "model": stem + ".glb",
                "skeleton": manifest["skeleton"]["id"],
                "bind_signature": manifest["skeleton"]["bind_signature"],
            }
        },
    }
    (output / (stem + ".fit.json")).write_text(json.dumps(fit, indent=2) + "\n")
    if before != context.build_inputs(source, recipe):
        raise ValueError("Fitted export source/recipe/compiler changed during export")
    receipt = {
        "version": 2,
        "id": item,
        "body": profile,
        "status": "exported",
        "inputs": before,
        "outputs": {
            stem + ext: file_hash(output / (stem + ext))
            for ext in (".glb", ".asset.json", ".fit.json")
        },
        "blender": bpy.app.version_string,
        "animation_clips": 0,
        "bind_matrix_error": result["source_bind_matrix_max_error"],
        "review": "pending fit validation and visual review",
    }
    (output / (stem + ".build.json")).write_text(json.dumps(receipt, indent=2) + "\n")
    return receipt
