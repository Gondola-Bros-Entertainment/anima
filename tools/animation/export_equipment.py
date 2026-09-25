"""Blender entry point for Anima's independent equipment compiler."""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from anima_pipeline.blender.equipment import export

p = argparse.ArgumentParser(description=__doc__)
for name in ("source", "recipe", "out"):
    p.add_argument("--" + name, required=True)
a = p.parse_args(sys.argv[sys.argv.index("--") + 1 :])
export(a.source, a.recipe, a.out)
