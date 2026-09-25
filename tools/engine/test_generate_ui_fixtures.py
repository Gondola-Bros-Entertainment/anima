#!/usr/bin/env python3
"""Verify generated UI PNG format, exact texels and fixture staging without a GPU."""

from pathlib import Path
import struct
import subprocess
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch
import zlib

import gpu_ui_smoke
from generate_ui_fixtures import ASSETS, checker_png, generate, run_consumer


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

    def test_test_wrapper_removes_images_on_success_and_failure(self):
        with TemporaryDirectory() as temporary:
            consumer = Path(temporary) / "consumer"
            consumer.touch()
            for code in (0, 7):
                with self.subTest(returncode=code):
                    fixtures = []

                    def execute(command, check):
                        self.assertFalse(check)
                        self.assertEqual(command[0], str(consumer.resolve()))
                        directory = Path(command[1]).parent
                        fixtures.append(directory)
                        self.assertEqual((directory / "checker.png").read_bytes(), checker_png())
                        self.assertTrue((directory / "controls.rml").is_file())
                        return subprocess.CompletedProcess(command, code)

                    with patch("generate_ui_fixtures.subprocess.run", side_effect=execute):
                        self.assertEqual(run_consumer(ASSETS, consumer), code)
                    self.assertEqual(len(fixtures), 1)
                    self.assertFalse(fixtures[0].exists())

    def test_test_wrapper_removes_images_when_process_cannot_start(self):
        with TemporaryDirectory() as temporary:
            consumer = Path(temporary) / "consumer"
            consumer.touch()
            fixtures = []

            def fail(command, check):
                self.assertFalse(check)
                fixtures.append(Path(command[1]).parent)
                raise OSError("Cannot start synthetic test process")

            with patch("generate_ui_fixtures.subprocess.run", side_effect=fail):
                with self.assertRaises(OSError):
                    run_consumer(ASSETS, consumer)
            self.assertEqual(len(fixtures), 1)
            self.assertFalse(fixtures[0].exists())

    def test_gpu_failure_removes_fixture_and_partial_readback(self):
        with TemporaryDirectory() as temporary:
            root = Path(temporary)
            consumer = root / "consumer"
            consumer.touch()
            output = root / "evidence"
            fixtures = []

            def fail(binary, arguments, directory, name):
                self.assertEqual(binary, consumer.resolve())
                self.assertEqual(name, "default")
                fixtures.append(Path(arguments[2]))
                self.assertTrue((fixtures[0] / "checker.png").is_file())
                (Path(arguments[1]) / "partial.ppm").write_bytes(b"synthetic readback")
                (directory / "failure.log").write_text("synthetic failure", encoding="utf-8")
                raise RuntimeError("Synthetic GPU failure")

            arguments = ["gpu_ui_smoke.py", str(consumer), "--assets", str(ASSETS), "--output", str(output)]
            with patch("sys.argv", arguments), patch.object(gpu_ui_smoke, "run", side_effect=fail):
                with self.assertRaises(RuntimeError):
                    gpu_ui_smoke.main()
            self.assertEqual(len(fixtures), 1)
            self.assertFalse(fixtures[0].exists())
            self.assertEqual(list(output.rglob("*.ppm")), [])
            self.assertTrue((output / "failure.log").is_file())


if __name__ == "__main__":
    unittest.main()
