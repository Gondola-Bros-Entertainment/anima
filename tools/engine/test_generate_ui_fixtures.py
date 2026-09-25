#!/usr/bin/env python3
"""Verify generated UI PNG format, exact texels and fixture staging without a GPU."""

from pathlib import Path
import struct
from tempfile import TemporaryDirectory
import unittest
import zlib

from generate_ui_fixtures import checker_png, generate


class FixtureTests(unittest.TestCase):
    def test_png_dimensions_channels_texels_and_checksums(self):
        data = checker_png()
        self.assertEqual(data, checker_png())
        self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
        offset = 8
        chunks = []
        while offset < len(data):
            size = struct.unpack_from(">I", data, offset)[0]
            kind = data[offset + 4:offset + 8]
            payload = data[offset + 8:offset + 8 + size]
            checksum = struct.unpack_from(">I", data, offset + 8 + size)[0]
            self.assertEqual(checksum, zlib.crc32(kind + payload))
            chunks.append((kind, payload))
            offset += 12 + size
        self.assertEqual(offset, len(data))
        self.assertEqual([kind for kind, _ in chunks], [b"IHDR", b"IDAT", b"IEND"])
        self.assertEqual(struct.unpack(">IIBBBBB", chunks[0][1]), (2, 2, 8, 6, 0, 0, 0))
        rows = zlib.decompress(chunks[1][1])
        self.assertEqual(rows, bytes((0, 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 0, 255, 255, 255, 255, 255, 128)))

    def test_staging_preserves_inputs_and_rejects_source_output(self):
        with TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            source.mkdir()
            contents = {
                "controls.rml": b"<rml><head></head><body></body></rml>",
                "LatoLatin-Regular.ttf": b"synthetic font fixture",
                "LICENSE.txt": b"synthetic license fixture",
            }
            for name, data in contents.items():
                (source / name).write_bytes(data)
            output = generate(source, root / "generated")
            for name, data in contents.items():
                self.assertEqual((output / name).read_bytes(), data)
                self.assertEqual((source / name).read_bytes(), data)
            self.assertEqual((output / "checker.png").read_bytes(), checker_png())
            self.assertFalse((source / "checker.png").exists())
            with self.assertRaises(ValueError):
                generate(source, source)
            with self.assertRaises(ValueError):
                generate(source, source / "generated")


if __name__ == "__main__":
    unittest.main()
