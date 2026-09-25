"""Snapshot runtime resource trees, including nested fitted-item dependencies."""

from pathlib import Path
import shutil


def runtime_files(root):
    root = Path(root)
    result = {}

    def visit(directory, ancestors):
        resolved = directory.resolve(strict=True)
        if resolved in ancestors:
            raise ValueError("Cyclic runtime resource directory: " + str(directory))
        for path in sorted(directory.iterdir()):
            if path.is_dir():
                visit(path, ancestors | {resolved})
            elif (
                path.suffix in (".json", ".glb")
                and not path.name.endswith(".build.json")
                and ".reference." not in path.name
            ):
                result[path.relative_to(root)] = path

    visit(root, set())
    return result


def copy_runtime_tree(source, destination, exclude=()):
    for relative, path in runtime_files(source).items():
        if relative.as_posix() in exclude:
            continue
        target = Path(destination) / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
