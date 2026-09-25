#!/usr/bin/env python3
"""Verify the built Python source and wheel distributions carry Anima's licenses."""

from email.parser import BytesParser
from pathlib import Path
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
LICENSE_FILES = ("LICENSE", "NOTICE")


def check_metadata(data):
    metadata = BytesParser().parsebytes(data)
    if metadata["License-Expression"] != "Apache-2.0":
        raise SystemExit("Distribution is missing the Apache-2.0 license expression")
    if set(metadata.get_all("License-File", [])) != set(LICENSE_FILES):
        raise SystemExit("Distribution is missing LICENSE or NOTICE metadata")


def check(root=ROOT):
    expected = {}
    for name in LICENSE_FILES:
        expected[name] = (root / name).read_bytes()
        if (root / "python" / name).read_bytes() != expected[name]:
            raise SystemExit(f"Python package {name} differs from repository {name}")

    distributions = root / "build/dist"
    wheels = list(distributions.glob("*.whl"))
    sources = list(distributions.glob("*.tar.gz"))
    if len(wheels) != 1 or len(sources) != 1:
        raise SystemExit("Expected one wheel and one source distribution in build/dist")
    with zipfile.ZipFile(wheels[0]) as wheel:
        metadata_path, = (name for name in wheel.namelist()
                          if name.endswith(".dist-info/METADATA"))
        check_metadata(wheel.read(metadata_path))
        prefix = metadata_path.rsplit("/", 1)[0]
        for name, contents in expected.items():
            if wheel.read(f"{prefix}/licenses/{name}") != contents:
                raise SystemExit(f"Wheel {name} does not match the repository")
    with tarfile.open(sources[0], "r:gz") as source:
        metadata_path, = (name for name in source.getnames()
                          if len(Path(name).parts) == 2 and name.endswith("/PKG-INFO"))
        check_metadata(source.extractfile(metadata_path).read())
        prefix = metadata_path.rsplit("/", 1)[0]
        for name, contents in expected.items():
            if source.extractfile(f"{prefix}/{name}").read() != contents:
                raise SystemExit(f"Source distribution {name} does not match the repository")


if __name__ == "__main__":
    check()
    print("PASS source and wheel licensing")
