#!/usr/bin/env python3
"""Safety regressions for repository-local build cleanup; uses temporary files only."""

from contextlib import redirect_stderr, redirect_stdout
import io
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import clean_builds


class CleanupTests(unittest.TestCase):
    def setUp(self):
        self.temporary = TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / "build").mkdir()
        self.commands = []

    def write(self, path, contents="fixture"):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents, encoding="utf-8")
        return path

    def build(self, name, cache=""):
        path = self.root / "build" / name
        self.cache(path, cache)
        self.write(path / "objects" / "engine.o")
        return path

    def cache(self, path, extra=""):
        return self.write(path / "CMakeCache.txt", f"CMAKE_CACHEFILE_DIR:INTERNAL={path.resolve()}\n{extra}")

    def run_tool(self, *arguments):
        output, errors = io.StringIO(), io.StringIO()
        with redirect_stdout(output), redirect_stderr(errors), patch.object(clean_builds.subprocess, "run") as execute:
            result = clean_builds.main(list(arguments), root=self.root)
        self.commands.extend(execute.call_args_list)
        return result, output.getvalue(), errors.getvalue()

    def assert_cleaned(self, *directories):
        self.assertEqual(len(self.commands), len(directories))
        for command, directory in zip(self.commands, directories):
            self.assertEqual(command.args, (["cmake", "--build", str(directory), "--target", "clean"],))
            self.assertEqual(command.kwargs, {"check": True})

    def test_dry_run_and_apply_preserve_evidence_and_non_cmake_entries(self):
        active, retired = self.build("active"), self.build("retired")
        log = self.write(retired / "Testing" / "Temporary" / "LastTest.log")
        report = self.write(retired / "summary.json")
        text = self.write(retired / "qualification.txt")
        evidence = self.write(retired / "evidence" / "capture.bin")
        asset = self.write(retired / "authored" / "model.glb")
        source = self.write(retired / "authored" / "source.cpp")
        nested = retired / "consumer-suite" / "build"
        self.cache(nested)
        notes = self.write(self.root / "build" / "notes" / "keep.bin")
        result, output, _ = self.run_tool("--keep", "active")
        self.assertEqual(result, 0)
        self.assertIn("Dry-run", output)
        self.assert_cleaned()
        self.assertTrue((retired / "objects" / "engine.o").exists())
        result, _, _ = self.run_tool("--apply", "--keep", "active")
        self.assertEqual(result, 0)
        self.assertTrue((active / "CMakeCache.txt").exists())
        self.assert_cleaned(nested, retired)
        # The utility never deletes files itself; only the mocked generator
        # clean targets are permitted to decide which outputs are removable.
        self.assertTrue((retired / "CMakeCache.txt").exists())
        self.assertTrue((retired / "objects" / "engine.o").exists())
        for retained in (log, report, text, evidence, notes, asset, source):
            self.assertEqual(retained.read_text(encoding="utf-8"), "fixture")

    def test_git_roots_and_nested_worktrees_are_retained(self):
        checkout, worktree = self.build("checkout"), self.build("nested-worktree")
        (checkout / ".git").mkdir()
        self.write(worktree / "sources" / "project" / ".git", "gitdir: /elsewhere")
        disposable = self.build("disposable")
        result, output, _ = self.run_tool("--apply")
        self.assertEqual(result, 0)
        self.assertIn("Git checkout/worktree", output)
        self.assertTrue((checkout / "objects" / "engine.o").exists())
        self.assertTrue((worktree / "objects" / "engine.o").exists())
        self.assert_cleaned(disposable)

    def test_build_directory_itself_cannot_be_a_git_root(self):
        candidate = self.build("candidate")
        self.write(self.root / "build" / ".git", "gitdir: /elsewhere")
        result, _, _ = self.run_tool("--apply")
        self.assertEqual(result, 2)
        self.assertTrue((candidate / "CMakeCache.txt").exists())
        self.assert_cleaned()

    def test_nested_and_root_symlinks_leave_external_files_untouched(self):
        candidate = self.build("linked-source")
        external = self.root / "external"
        payload = self.write(external / "payload.bin")
        self.cache(external)
        try:
            (candidate / "linked").symlink_to(external, target_is_directory=True)
            (self.root / "build" / "linked-root").symlink_to(external, target_is_directory=True)
        except OSError as error:
            self.skipTest(f"Symlink creation unavailable: {error}")
        result, _, _ = self.run_tool("--apply")
        self.assertEqual(result, 0)
        self.assertTrue((candidate / "objects" / "engine.o").exists())
        self.assertTrue((self.root / "build" / "linked-root").is_symlink())
        self.assertEqual(payload.read_text(encoding="utf-8"), "fixture")
        self.assert_cleaned()

    def test_source_hosts_are_retained_transitively_from_nested_caches(self):
        first_source = self.root / "build" / "source-host" / "_deps" / "one-src"
        second_source = self.root / "build" / "transitive-host" / "_deps" / "two-src"
        kept = self.build("active")
        self.cache(
            kept / "consumer-suite" / "build",
            f"FETCHCONTENT_SOURCE_DIR_ONE:PATH={first_source}\n",
        )
        first = self.build("source-host", f"FETCHCONTENT_SOURCE_DIR_TWO:PATH={second_source}\n")
        second = self.build("transitive-host")
        self.write(first_source / "source.cpp")
        self.write(second_source / "source.cpp")
        obsolete = self.build("obsolete")
        result, output, _ = self.run_tool("--apply", "--keep", "active")
        self.assertEqual(result, 0)
        self.assertIn("dependency sources used by active", output)
        self.assertIn("dependency sources used by source-host", output)
        for retained in (kept, first, second):
            self.assertTrue((retained / "CMakeCache.txt").exists())
        self.assert_cleaned(obsolete)

    def test_moved_root_or_nested_cache_never_runs_a_clean_target(self):
        original = self.build("original")
        moved = original.rename(self.root / "build" / "moved")
        containing = self.build("nested-move")
        nested = containing / "consumer-suite" / "old-build"
        self.cache(nested)
        nested.rename(nested.with_name("moved-build"))
        result, output, _ = self.run_tool("--apply")
        self.assertEqual(result, 0)
        self.assertIn("moved CMake cache", output)
        self.assertTrue((moved / "objects" / "engine.o").exists())
        self.assertTrue((containing / "objects" / "engine.o").exists())
        self.assert_cleaned()

    def test_unknown_or_ambiguous_cache_location_is_retained(self):
        for name, contents in (
            ("missing", ""),
            ("relative", "CMAKE_CACHEFILE_DIR:INTERNAL=build/relative\n"),
            ("empty", "CMAKE_CACHEFILE_DIR:INTERNAL=\n"),
        ):
            path = self.build(name)
            self.write(path / "CMakeCache.txt", contents)
        duplicate = self.build("duplicate")
        self.cache(duplicate, f"CMAKE_CACHEFILE_DIR:INTERNAL={duplicate.resolve()}\n")
        result, output, _ = self.run_tool("--apply")
        self.assertEqual(result, 0)
        self.assertEqual(output.count("unknown CMake cache location"), 4)
        self.assert_cleaned()

    def test_changed_cache_location_is_rechecked_before_any_clean_target(self):
        candidate = self.build("candidate")
        self.cache(candidate / "nested")
        selected = clean_builds.inventory(self.root, set())[0]
        self.write(candidate / "CMakeCache.txt", f"CMAKE_CACHEFILE_DIR:INTERNAL={self.root / 'old-location'}\n")
        with patch.object(clean_builds.subprocess, "run") as execute:
            with self.assertRaisesRegex(ValueError, "moved CMake cache"):
                clean_builds.clean_build(selected)
        execute.assert_not_called()

    def test_invalid_keep_name_cannot_expand_cleanup_scope(self):
        candidate = self.build("candidate")
        for name in ("../outside", "..\\outside", "missing"):
            result, _, _ = self.run_tool("--apply", "--keep", name)
            self.assertEqual(result, 2)
            self.assertTrue((candidate / "CMakeCache.txt").exists())
        self.assert_cleaned()

    def test_free_space_preflight_never_cleans_and_apply_checks_afterward(self):
        candidate = self.build("candidate")
        low = SimpleNamespace(free=clean_builds.GIB)
        enough = SimpleNamespace(free=3 * clean_builds.GIB)
        with patch.object(clean_builds.shutil, "disk_usage", return_value=low):
            result, _, errors = self.run_tool("--min-free-gib", "2")
        self.assertEqual(result, 1)
        self.assertIn("below", errors)
        self.assertTrue((candidate / "CMakeCache.txt").exists())
        self.assert_cleaned()
        with patch.object(clean_builds.shutil, "disk_usage", side_effect=[low, enough]):
            result, _, _ = self.run_tool("--apply", "--min-free-gib", "2")
        self.assertEqual(result, 0)
        self.assert_cleaned(candidate)
        self.commands.clear()
        with patch.object(clean_builds.shutil, "disk_usage", return_value=low):
            result, _, _ = self.run_tool("--apply", "--min-free-gib", "2")
        self.assertEqual(result, 1)
        self.assert_cleaned(candidate)

    def test_clean_failure_stops_without_direct_file_deletion(self):
        first = self.build("first")
        second = self.build("second")
        output = io.StringIO()
        with redirect_stdout(output), redirect_stderr(output), patch.object(
            clean_builds.subprocess, "run", side_effect=subprocess.CalledProcessError(1, "cmake")
        ) as execute:
            result = clean_builds.main(["--apply"], root=self.root)
        self.assertEqual(result, 2)
        self.assertEqual(execute.call_count, 1)
        self.assertTrue((first / "objects" / "engine.o").exists())
        self.assertTrue((second / "objects" / "engine.o").exists())


if __name__ == "__main__":
    unittest.main()
