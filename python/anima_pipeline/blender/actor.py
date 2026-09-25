"""Compile a caller's actor recipe into separately qualified runtime resources."""

import json
from pathlib import Path
import bpy
from ..graph import load_graph
from ..properties import AuthoringProperties
from ..resources import package_actor
from ..validation import qualify
from .bake import export_profile
from .composition import prepare_export


def compile_actor(
    source,
    recipe,
    body,
    output,
    *,
    context,
    material_converter,
    properties=AuthoringProperties(),
    root_joint="root",
    initial_clip=None,
    prepare_mesh=None,
    mesh_name=None,
):
    source, recipe, output = map(lambda path: Path(path).resolve(), (source, recipe, output))
    graph = load_graph(recipe)
    adapter = graph.data["bodies"][body]
    before = context.build_inputs(source, recipe)
    output.mkdir(parents=True, exist_ok=False)
    motion_report = {}

    def compile_motion():
        actions, report = prepare_export(body, recipe, properties=properties)
        motion_report.update(report)
        return actions

    result = export_profile(
        source,
        output,
        adapter["label"],
        mesh_collection=adapter["mesh_collection"],
        rig_name=adapter["rig"],
        stem=body,
        asset_id=adapter["asset_id"],
        skeleton_id=adapter["skeleton_id"],
        stage=adapter["stage"],
        material_converter=material_converter,
        prepare_actions=compile_motion,
        policies={
            **graph.data["playback"],
            **{name: {"loop": False, "events": []} for name in graph.data["layers"]},
        },
        properties=properties,
        root_joint=root_joint,
        initial_clip=initial_clip,
        prepare_mesh=prepare_mesh,
        mesh_name=mesh_name,
        preserve_hierarchy=adapter.get("preserve_hierarchy", False),
        preserve_object_transform=adapter.get("preserve_object_transform", False),
    )
    manifest = result["manifest"]
    reference = output / (body + ".reference.glb")
    (output / (body + ".glb")).rename(reference)
    (output / (body + ".reference.asset.json")).write_text(
        json.dumps({**manifest, "model": reference.name}, indent=2) + "\n"
    )
    evaluation = json.loads((context.root / graph.data["evaluation"]).read_text())
    outputs, contract = package_actor(
        reference, graph, manifest, body, output, evaluation, group_property=properties.render_group
    )
    manifest_path = output / (body + ".asset.json")
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    outputs.append(manifest_path)
    fits = {
        "version": 1,
        "items": [
            {
                "id": name,
                "slot": definition["slot"],
                "fits": {
                    body: {
                        "model": body + "." + name + ".glb",
                        "skeleton": manifest["skeleton"]["id"],
                        "bind_signature": manifest["skeleton"]["bind_signature"],
                    }
                },
            }
            for name, definition in graph.data.get("equipment", {}).items()
        ],
    }
    fit_path = output / "compiled-fits.json"
    fit_path.write_text(json.dumps(fits, indent=2) + "\n")
    result.update(
        {
            "source_sha256": before["source"],
            "motion_build": motion_report,
            "qualification": qualify(manifest_path, reference, garment_catalog=fit_path),
        }
    )
    if before != context.build_inputs(source, recipe):
        raise ValueError("Source, recipe or compiler changed during export; rebuild required")
    context.write_receipt(
        output / (body + ".build.json"),
        source,
        recipe,
        body,
        motion_report,
        outputs,
        {"version": bpy.app.version_string, "build_hash": bpy.app.build_hash.decode()},
    )
    (output / "export-report.json").write_text(json.dumps(result, indent=2) + "\n")
    return result
