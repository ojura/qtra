"""Private fixtures for the optional renderer CLI; never launch a child."""
from __future__ import annotations

import contextlib
import io
import json
import os
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch

from rehearsal import RehearsalViolation
import render_voice


class FixtureGuard(render_voice.RenderGuard):
    def install(self):
        pass  # The standalone logic runner already installed its process-wide guard.

    def self_check(self):
        self.checks.append("fixture guard")


class FixtureRenderer:
    directories = []

    def __init__(self, directory):
        self.directory = Path(directory)
        self.directories.append(self.directory)

    def active_launch_on_thread(self):
        return False

    def allow_spawn(self, *args):
        return False

    def render(self, text="original fixture"):
        files = {}
        for name in ("voice", "mix"):
            path = self.directory / f"{name}.wav"
            path.write_bytes(b"private file fixture")
            files[name] = str(path)
        return {"text": text, "voice": {"durationSeconds": 1}, "files": files}


class RenderCliTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="render-cli-test-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name).resolve()
        FixtureRenderer.directories = []

    def render(self, **kwargs):
        return render_voice.run_render(renderer_factory=FixtureRenderer,
                                       guard_factory=FixtureGuard, **kwargs)

    def test_default_cleans_private_files_and_reports_unheard_voice(self):
        report = self.render()
        self.assertEqual(report["status"], "passed")
        self.assertEqual(report["privateOutputs"], "cleaned")
        self.assertEqual(report["exportedFiles"], {})
        self.assertNotIn("files", report["render"])
        self.assertEqual(report["audibleVerification"], "unperformed")
        self.assertEqual(report["pronunciationVerification"], "unperformed")
        self.assertTrue(FixtureRenderer.directories)
        self.assertTrue(all(not path.exists() for path in FixtureRenderer.directories))

    def test_explicit_export_retains_only_the_two_requested_files(self):
        destination = self.directory / "export"
        report = self.render(export=destination)
        self.assertEqual(report["status"], "passed")
        self.assertEqual(set(report["exportedFiles"]), {"voice", "mix"})
        self.assertEqual({path.name for path in destination.iterdir()}, {"voice.wav", "mix.wav"})
        self.assertEqual(destination.stat().st_mode & 0o777, 0o700)
        self.assertEqual(report["privateOutputs"], "cleaned")

    def test_existing_export_is_preserved_and_renderer_is_not_constructed(self):
        kept = self.directory / "keep.txt"
        kept.write_text("existing content")
        report = self.render(export=self.directory)
        self.assertEqual(report["status"], "failed")
        self.assertIn("already exist", report["errors"][0]["message"])
        self.assertEqual(kept.read_text(), "existing content")
        self.assertEqual(FixtureRenderer.directories, [])

    def test_export_copy_failure_removes_only_the_new_partial_directory(self):
        destination = self.directory / "export"
        kept = self.directory / "keep.txt"
        kept.write_text("keep")
        copyfile = render_voice.shutil.copyfile

        def fail_after_first(source, target):
            if Path(target).name == "mix.wav":
                raise OSError("fixture copy failed")
            return copyfile(source, target)

        with patch.object(render_voice.shutil, "copyfile", side_effect=fail_after_first):
            report = self.render(export=destination)
        self.assertEqual(report["status"], "failed")
        self.assertFalse(destination.exists())
        self.assertEqual(kept.read_text(), "keep")
        self.assertEqual(report["privateOutputs"], "cleaned")

    def test_constructor_and_render_failures_report_cleanup(self):
        def constructor_failure(directory):
            raise RuntimeError("fixture constructor failed")

        class FailingRenderer(FixtureRenderer):
            def render(self, text="fixture"):
                (self.directory / "partial.wav").write_bytes(b"partial")
                raise RuntimeError("fixture synthesis failed")

        for factory in (constructor_failure, FailingRenderer):
            with self.subTest(factory=factory):
                report = render_voice.run_render(renderer_factory=factory, guard_factory=FixtureGuard)
                self.assertEqual(report["status"], "failed")
                self.assertEqual(report["privateOutputs"], "cleaned")
                self.assertEqual(report["errors"][0]["type"], "RuntimeError")
        self.assertTrue(all(not path.exists() for path in FixtureRenderer.directories))

    def test_caught_forbidden_attempt_still_fails_and_prevents_export(self):
        guards = []

        class CapturedGuard(FixtureGuard):
            def __init__(self, directory):
                super().__init__(directory)
                guards.append(self)

        class CatchingRenderer(FixtureRenderer):
            def render(self, text="fixture"):
                try:
                    guards[-1].audit("socket.connect", (None, ("fixture.invalid", 1)))
                except RehearsalViolation:
                    pass
                return super().render(text)

        destination = self.directory / "export"
        report = render_voice.run_render(export=destination, renderer_factory=CatchingRenderer,
                                        guard_factory=CapturedGuard)
        self.assertEqual(report["status"], "failed")
        self.assertEqual(report["sideEffectAttempts"][0]["event"], "socket.connect")
        self.assertFalse(destination.exists())
        self.assertEqual(report["privateOutputs"], "cleaned")

    def test_cleanup_failure_is_reported_with_its_directory(self):
        temporary = tempfile.TemporaryDirectory(dir=self.directory)
        self.addCleanup(temporary.cleanup)
        with patch.object(render_voice.tempfile, "TemporaryDirectory", return_value=temporary):
            with patch.object(temporary, "cleanup", side_effect=OSError("fixture cleanup failed")):
                report = self.render()
        self.assertEqual(report["status"], "failed")
        self.assertEqual(report["privateOutputs"], "cleanup_failed")
        self.assertEqual(report["privateDirectory"], temporary.name)
        self.assertIn("fixture cleanup failed", report["errors"][0]["message"])

    def test_export_rejects_outside_and_symlinked_sources(self):
        guard = FixtureGuard(self.directory / "private")
        guard.directory.mkdir()
        outside = self.directory / "outside.wav"
        outside.write_bytes(b"outside")
        with self.assertRaisesRegex(ValueError, "regular private file"):
            render_voice.export_files({"voice": str(outside), "mix": str(outside)},
                                      self.directory / "export", guard)
        inside = guard.directory / "inside.wav"
        inside.write_bytes(b"inside")
        with patch.object(Path, "is_symlink", return_value=True):
            with self.assertRaisesRegex(ValueError, "regular private file"):
                render_voice.export_files({"voice": str(inside), "mix": str(inside)},
                                          self.directory / "export", guard)
        self.assertFalse((self.directory / "export").exists())

    def test_cli_json_failure_exit_and_stdin_text(self):
        failed = {"status": "failed", "errors": [{"type": "RuntimeError", "message": "fixture"}]}
        stdout = io.StringIO()
        with patch.object(render_voice, "run_render", return_value=failed) as run:
            with patch.object(render_voice.sys, "stdin", io.StringIO("Antonio, pozdrav ekipi.")):
                with contextlib.redirect_stdout(stdout):
                    result = render_voice.main(["--json", "--text-stdin"])
        run.assert_called_once_with("Antonio, pozdrav ekipi.", None)
        self.assertEqual(result, 1)
        self.assertEqual(json.loads(stdout.getvalue()), failed)

    def test_cli_default_does_not_read_stdin(self):
        class Unreadable:
            def read(self, *args):
                raise AssertionError("stdin should be untouched")

        stdout = io.StringIO()
        with patch.object(render_voice, "run_render", return_value={"status": "passed"}) as run:
            with patch.object(render_voice.sys, "stdin", Unreadable()):
                with contextlib.redirect_stdout(stdout):
                    self.assertEqual(render_voice.main(["--json"]), 0)
        run.assert_called_once_with(None, None)


class RenderGuardTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="render-guard-test-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name).resolve()
        self.guard = render_voice.RenderGuard(self.directory)

    def test_process_permission_is_delegated_to_exact_renderer_approval(self):
        event_args = ("/private/espeak", ["/private/espeak", "-w", "file.wav"],
                      str(self.directory), {"LANG": "C.UTF-8"})
        with self.assertRaises(RehearsalViolation):
            self.guard.audit("subprocess.Popen", event_args)
        renderer = FixtureRenderer(self.directory)
        self.guard.renderer = renderer
        with patch.object(renderer, "allow_spawn", return_value=True) as allowed:
            self.guard.audit("subprocess.Popen", event_args)
        allowed.assert_called_once_with(*event_args)
        with self.assertRaises(RehearsalViolation):
            self.guard.audit("subprocess.Popen", event_args)
        for event in ("os.system", "os.posix_spawn", "socket.connect", "urllib.Request"):
            with self.subTest(event=event), self.assertRaises(RehearsalViolation):
                self.guard.audit(event, ())

    def test_only_pipe_open_on_the_active_launch_thread_is_permitted(self):
        renderer = FixtureRenderer(self.directory)
        self.guard.renderer = renderer
        with patch.object(os, "readlink", return_value="pipe:[1234]"):
            with self.assertRaises(RehearsalViolation):
                self.guard.audit("open", (77, "w", os.O_WRONLY))
            with patch.object(renderer, "active_launch_on_thread", return_value=True):
                self.guard.audit("open", (77, "w", os.O_WRONLY))
                for descriptor in (0, 1, 2):
                    with self.subTest(descriptor=descriptor), self.assertRaises(RehearsalViolation):
                        self.guard.audit("open", (descriptor, "w", os.O_WRONLY))
                with self.assertRaises(RehearsalViolation):
                    self.guard.audit("os.truncate", (77, 0))
        with patch.object(os, "readlink", return_value="/etc/fixture-outside"):
            with patch.object(renderer, "active_launch_on_thread", return_value=True):
                with self.assertRaises(RehearsalViolation):
                    self.guard.audit("open", (77, "w", os.O_WRONLY))
        with self.assertRaises(RehearsalViolation):
            self.guard.audit("open", ("/dev/snd/fixture", "r", 0))

    def test_export_permission_expires_and_relative_paths_stay_forbidden(self):
        destination = self.directory.parent / (self.directory.name + "-export")
        with self.guard.exporting_to(destination):
            self.guard.audit("open", (str(destination / "voice.wav"), "w", os.O_WRONLY))
            with self.assertRaises(RehearsalViolation):
                self.guard.audit("open", ("voice.wav", "w", os.O_WRONLY))
        with self.assertRaises(RehearsalViolation):
            self.guard.audit("open", (str(destination / "voice.wav"), "w", os.O_WRONLY))
        self.assertFalse(destination.exists())

    def test_another_thread_cannot_use_the_export_permission(self):
        destination = self.directory.parent / (self.directory.name + "-export")
        denied = []

        def other_thread():
            try:
                self.guard.audit("open", (str(destination / "voice.wav"), "w", os.O_WRONLY))
            except RehearsalViolation:
                denied.append(True)

        with self.guard.exporting_to(destination):
            thread = threading.Thread(target=other_thread, daemon=True)
            thread.start()
            thread.join(1)
        self.assertFalse(thread.is_alive())
        self.assertEqual(denied, [True])


if __name__ == "__main__":
    unittest.main()
