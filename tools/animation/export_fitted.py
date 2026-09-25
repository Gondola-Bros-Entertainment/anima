"""Blender entry point for a fitted mesh using standard glTF materials."""

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from anima_pipeline.blender.fitted import export_fitted
from anima_pipeline.receipts import BuildContext
from anima_pipeline.properties import AuthoringProperties

p = argparse.ArgumentParser(description=__doc__)
for name in ("source", "recipe", "body", "profile", "group", "item", "slot", "root", "out"):
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
export_fitted(
    a.source,
    a.recipe,
    a.body,
    a.profile,
    a.group,
    a.item,
    a.slot,
    a.out,
    context=BuildContext(a.root, extra),
    material_converter=lambda material, mesh: material,
    properties=properties,
    root_joint=a.root_joint,
)
print("FITTED_RUNTIME_EXPORTED", a.item, flush=True)
