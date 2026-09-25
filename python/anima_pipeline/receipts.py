"""Exact compiler/source receipts with caller-owned project roots and dependencies."""

import json
from pathlib import Path
from .graph import digest, file_hash, fields, load_graph, validate_evaluation


def compiler_sources():
    """Stable names, actual engine paths. Include the complete supported package."""
    package = Path(__file__).resolve().parent
    return {
        "anima_pipeline/" + path.relative_to(package).as_posix(): path
        for path in sorted(package.rglob("*.py"))
    }


class BuildContext:
    def __init__(self, root, extra_compiler_sources=None):
        self.root = Path(root).resolve()
        self.compiler_sources = compiler_sources()
        for name, path in (extra_compiler_sources or {}).items():
            if (
                not name
                or name in self.compiler_sources
                or Path(name).is_absolute()
                or ".." in Path(name).parts
            ):
                raise ValueError("Compiler dependencies need unique stable relative names")
            self.compiler_sources[name] = Path(path).resolve()

    def build_inputs(self, source, recipe):
        graph = load_graph(recipe)
        evaluation = self.root / graph.data["evaluation"]
        validate_evaluation(json.loads(evaluation.read_text()))
        return {
            "source": file_hash(source),
            "recipe": file_hash(recipe),
            "compiler": {
                name: file_hash(path) for name, path in sorted(self.compiler_sources.items())
            },
            "dependencies": {graph.data["evaluation"]: file_hash(evaluation)},
        }

    def write_receipt(self, path, source, recipe, body, motion_report, outputs, blender):
        result = {
            "version": 2,
            "body": body,
            "inputs": self.build_inputs(source, recipe),
            "blender": blender,
            "motion": motion_report,
            "outputs": {Path(p).name: file_hash(Path(p)) for p in outputs},
        }
        result["build_key"] = digest(
            {k: result[k] for k in ("inputs", "body", "blender", "motion")}
        )
        Path(path).write_text(json.dumps(result, indent=2) + "\n")
        self.verify_receipt(path, source, recipe, body=body)
        return result

    def verify_receipt(self, path, source, recipe, *, body=None):
        path = Path(path)
        value = json.loads(path.read_text())
        fields(value, ("version", "body", "inputs", "blender", "motion", "outputs", "build_key"))
        if value["version"] != 2 or (body is not None and value["body"] != body):
            raise ValueError("Wrong build receipt version or body")
        graph = load_graph(recipe)
        if value["body"] not in graph.data["bodies"]:
            raise ValueError("Receipt body is not registered by its recipe")
        if value["inputs"] != self.build_inputs(source, recipe):
            raise ValueError("Stale actor export: source, recipe or compiler changed")
        expected = digest({k: value[k] for k in ("inputs", "body", "blender", "motion")})
        if expected != value["build_key"]:
            raise ValueError("Invalid build receipt key")
        required = {
            value["body"] + suffix
            for suffix in (".glb", ".asset.json", ".motion.json", ".motion.glb")
        }
        required.update(
            value["body"] + "." + name + ".glb" for name in graph.data.get("equipment", {})
        )
        if set(value["outputs"]) != required:
            raise ValueError("Build receipt must cover the model and runtime manifest/resources")
        for name, checksum in value["outputs"].items():
            if Path(name).name != name or (path.parent / name).is_symlink():
                raise ValueError("Invalid build output path")
            if file_hash(path.parent / name) != checksum:
                raise ValueError("Changed actor build output: " + name)
        return value
