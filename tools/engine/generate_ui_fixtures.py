#!/usr/bin/env python3
"""Stage UI correctness fixtures and generate their deterministic RGBA test image."""

import argparse
from pathlib import Path
import shutil
import struct
import zlib


ASSETS = Path(__file__).resolve().parents[2] / "tests" / "ui" / "assets"


def checker_png():
    pixels = ((255, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 255), (255, 255, 255, 128))
    rows = b"".join(
        b"\0" + bytes(channel for pixel in pixels[row:row + 2] for channel in pixel)
        for row in (0, 2)
    )

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 2, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(rows, level=9))
        + chunk(b"IEND", b"")
    )


def generate(source, output):
    source = source.resolve(strict=True)
    output = output.resolve()
    if output == source or source in output.parents:
        raise ValueError("Generate UI fixtures outside the maintained source fixture directory")
    output.mkdir(parents=True, exist_ok=True)
    for name in ("controls.rml", "LatoLatin-Regular.ttf", "LICENSE.txt"):
        shutil.copyfile(source / name, output / name)
    (output / "checker.png").write_bytes(checker_png())
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", type=Path, default=ASSETS, help="source controls and licensed font fixtures")
    parser.add_argument("--output", type=Path, required=True, help="generated fixture directory, normally under build/")
    args = parser.parse_args()
    print(f"Generated UI fixtures in {generate(args.assets, args.output)}")


if __name__ == "__main__":
    main()
