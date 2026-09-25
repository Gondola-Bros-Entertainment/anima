"""Blender entry point for a caller's actor recipe and standard glTF materials."""

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from anima_pipeline.blender.actor import compile_actor
from anima_pipeline.receipts import BuildContext
from anima_pipeline.properties import AuthoringProperties

p = argparse.ArgumentParser(description=__doc__)
for name in ("source", "recipe", "body", "root", "out"):
    p.add_argument("--" + name, required=True)
p.add_argument("--root-joint", default="root")
p.add_argument("--properties", type=Path)
a = p.parse_args(sys.argv[sys.argv.index("--") + 1 :])
properties = (
    AuthoringProperties(**json.loads(a.properties.read_text()))
    if a.properties
    else AuthoringProperties()
)
extra = {"consumer/properties.json": a.properties} if a.properties else {}
result = compile_actor(
    a.source,
    a.recipe,
    a.body,
    a.out,
    context=BuildContext(a.root, extra),
    material_converter=lambda material, mesh: material,
    properties=properties,
    root_joint=a.root_joint,
)
print("ACTOR_RUNTIME_EXPORTED", result["vertices"], result["mesh_objects"], flush=True)
