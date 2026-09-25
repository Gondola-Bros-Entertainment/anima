#!/usr/bin/env python3
"""Clean repository-local CMake outputs while retaining sources and evidence."""

import argparse
from dataclasses import dataclass, field
import math
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
GIB = 1024 ** 3
SOURCE_OVERRIDE = re.compile(r"^FETCHCONTENT_SOURCE_DIR_[^:]+:[^=]*=(.*)$")
CACHE_LOCATION = re.compile(r"^CMAKE_CACHEFILE_DIR:[^=]*=(.*)$")


@dataclass
class Build:
    path: Path
    caches: list = field(default_factory=list)
    references: set = field(default_factory=set)
    reason: str = ""
    bytes: int = 0


def linked(path):
    info = path.lstat()
    return stat.S_ISLNK(info.st_mode) or bool(
        getattr(info, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
    )


def walk_error(error):
    raise error


def scan(path):
    build = Build(path)
    for directory, names, files in os.walk(path, followlinks=False, onerror=walk_error):
        current = Path(directory)
        if ".git" in names or ".git" in files:
            build.reason = "contains a Git checkout/worktree"
        for name in list(names):
            child = current / name
            if linked(child):
                build.reason = build.reason or "contains a symlink/junction"
                build.references.add(child.resolve())
                names.remove(name)
            elif name == ".git":
                names.remove(name)
        for name in files:
            child = current / name
            if linked(child):
                build.reason = build.reason or "contains a symlink/junction"
                build.references.add(child.resolve())
                continue
            if name == "CMakeCache.txt":
                build.caches.append(child)
            build.bytes += child.stat().st_size
    for cache in build.caches:
        locations = []
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            location = CACHE_LOCATION.match(line)
            if location:
                locations.append(location.group(1))
            match = SOURCE_OVERRIDE.match(line)
            if not match or not match.group(1):
                continue
            source = Path(match.group(1))
            if source.is_absolute():
                build.references.add(source.resolve())
            else:
                # CMake normally stores absolute overrides. Retain both plausible
                # roots for a manually supplied relative cache value.
                build.references.add((cache.parent / source).resolve())
                build.references.add((path.parent.parent / source).resolve())
        # Generated clean rules can retain absolute paths after a build is moved.
        # Unknown or inconsistent cache origins are not safe cleanup candidates.
        if len(locations) != 1 or not Path(locations[0]).is_absolute():
            build.reason = build.reason or f"unknown CMake cache location: {cache.relative_to(path)}"
        elif os.path.normcase(str(Path(locations[0]).resolve())) != os.path.normcase(str(cache.parent.resolve())):
            build.reason = build.reason or f"moved CMake cache: {cache.relative_to(path)}"
    return build


def contains(parent, child):
    try:
        child.relative_to(parent)
        return True
    except ValueError:
        return False


def inventory(root, keep):
    directory = root / "build"
    if not directory.exists():
        if keep:
            raise ValueError("Cannot keep configurations: build/ does not exist")
        return []
    if linked(directory) or (directory / ".git").exists() or (directory / ".git").is_symlink():
        raise ValueError("Refusing a linked build/ directory or one containing a Git root")
    for name in keep:
        if not name or name in {".", ".."} or "/" in name or "\\" in name:
            raise ValueError("--keep requires one immediate build directory name")
        if not (directory / name).is_dir():
            raise ValueError(f"Unknown --keep directory: {name}")
    kept_paths = {os.path.normcase(str((directory / name).resolve())) for name in keep}
    builds, retained_entries, aliases = [], [], []
    for path in sorted(directory.iterdir()):
        if linked(path):
            aliases.append(path.resolve())
            continue
        if not path.is_dir():
            continue
        cache = path / "CMakeCache.txt"
        if not cache.is_file() or linked(cache):
            # Never select non-CMake entries. Their nested configurations may
            # still borrow sources hosted by a selectable sibling directory.
            retained = scan(path)
            retained.reason = "non-CMake directory"
            retained_entries.append(retained)
            continue
        build = scan(path)
        if os.path.normcase(str(path.resolve())) in kept_paths:
            build.reason = "explicit --keep"
        builds.append(build)
    for build in builds:
        if any(contains(build.path.resolve(), target) or contains(target, build.path.resolve()) for target in aliases):
            build.reason = build.reason or "referenced by a symlink/junction"
    # Protect source hosts transitively, including references in nested consumer
    # caches and automatically retained worktrees/linked configurations.
    changed = True
    while changed:
        changed = False
        for retained in builds + retained_entries:
            if not retained.reason:
                continue
            for candidate in builds:
                if candidate.reason:
                    continue
                if any(contains(candidate.path.resolve(), source) for source in retained.references):
                    candidate.reason = f"dependency sources used by {retained.path.name}"
                    changed = True
    return builds


def clean_build(build):
    # Recheck the whole candidate immediately before changing it. Do not run this
    # utility concurrently with builds, checkout creation or directory changes.
    if linked(build.path):
        raise ValueError(f"Safety conditions changed for {build.path.name}; nothing in it was removed")
    current = scan(build.path)
    if current.reason:
        raise ValueError(f"Safety conditions changed for {build.path.name}: {current.reason}; nothing in it was removed")
    # CMake alone decides which files are generated outputs. A build directory
    # can contain authored assets, sources and evidence that must stay untouched.
    for cache in sorted(current.caches, key=lambda item: len(item.parts), reverse=True):
        subprocess.run(["cmake", "--build", str(cache.parent), "--target", "clean"], check=True)


def nonnegative_gib(value):
    number = float(value)
    if not math.isfinite(number) or number < 0:
        raise argparse.ArgumentTypeError("Expected a finite nonnegative GiB value")
    return number


def main(argv=None, root=ROOT):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apply", action="store_true", help="run CMake clean for selected builds (default: dry-run)")
    parser.add_argument("--keep", action="append", default=[], metavar="NAME", help="retain build/NAME; repeatable")
    parser.add_argument("--min-free-gib", type=nonnegative_gib, help="require this free space; with --apply check afterward")
    args = parser.parse_args(argv)
    try:
        builds = inventory(root, set(args.keep))
        free = shutil.disk_usage(root).free
        selected = [build for build in builds if not build.reason]
        print(f"Free space: {free / GIB:.2f} GiB")
        for build in builds:
            if build.reason:
                print(f"KEEP build/{build.path.name}: {build.reason}")
            else:
                print(f"SELECT build/{build.path.name}: approximately {build.bytes / GIB:.2f} GiB total directory size")
        total = sum(build.bytes for build in selected)
        print(f"Selected {len(selected)} configuration(s), approximately {total / GIB:.2f} GiB total; not all reclaimable")
        if args.apply:
            for build in selected:
                clean_build(build)
                print(f"CLEANED build/{build.path.name} and nested configurations using CMake targets")
            free = shutil.disk_usage(root).free
            print(f"Free space after cleanup: {free / GIB:.2f} GiB")
        else:
            print("Dry-run: nothing removed. Non-CMake entries and logs/evidence remain untouched.")
        if args.min_free_gib is not None and free / GIB < args.min_free_gib:
            print(f"Free space is below the required {args.min_free_gib:g} GiB", file=sys.stderr)
            return 1
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Cleanup refused or stopped: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
