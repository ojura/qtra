#!/usr/bin/env python3
"""Synthesize Croatian speech and measure private WAV files without playback."""
from __future__ import annotations

import argparse
import contextlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import threading
import time

sys.dont_write_bytecode = True
from rehearsal import SilentGuard

ROOT = Path(__file__).resolve().parents[1]


class RenderGuard(SilentGuard):
    """Permit the renderer's validated file commands and an explicit export."""

    def __init__(self, directory):
        super().__init__(directory)
        self.renderer = None
        self.export_directory = None
        self.exporting_thread = None

    def private_path(self, event, value, dir_fd=None, *, follow=True):
        if self.exporting_thread == threading.get_ident() and self.export_directory is not None:
            try:
                candidate = Path(os.fsdecode(value))
                resolved = candidate.resolve() if follow else candidate.parent.resolve() / candidate.name
                if candidate.is_absolute() and (resolved == self.export_directory
                        or resolved.is_relative_to(self.export_directory)):
                    return
            except (OSError, TypeError, ValueError):
                pass
        super().private_path(event, value, dir_fd, follow=follow)

    def audit(self, event, args):
        renderer = self.renderer
        if event == "subprocess.Popen" and renderer is not None:
            if renderer.allow_spawn(*args):
                return
        if (event == "open" and renderer is not None
                and isinstance(args[0], int) and args[0] > 2):
            # subprocess creates its input pipe before the Popen audit event.
            # A normal writable descriptor still goes through private_path.
            if renderer.active_launch_on_thread():
                try:
                    target = os.readlink(f"/proc/self/fd/{args[0]}")
                except OSError:
                    target = ""
                if target.startswith("pipe:[") and target.endswith("]"):
                    return
        super().audit(event, args)

    @contextlib.contextmanager
    def exporting_to(self, destination):
        self.export_directory = destination
        self.exporting_thread = threading.get_ident()
        try:
            yield
        finally:
            self.exporting_thread = None
            self.export_directory = None


def export_destination(value):
    """Require a new directory under an existing parent; never overwrite it."""
    candidate = Path(value).expanduser().absolute()
    parent = candidate.parent.resolve(strict=True)
    if not parent.is_dir() or not candidate.name:
        raise ValueError("The export parent must be an existing directory.")
    destination = parent / candidate.name
    if destination.exists() or destination.is_symlink():
        raise ValueError("The export directory must not already exist.")
    return destination


def export_files(files, destination, guard):
    if set(files) != {"voice", "mix"}:
        raise ValueError("The renderer must return exactly the voice and mix WAV files.")
    sources = {}
    for name, value in files.items():
        path = Path(value)
        if (not path.is_absolute() or path.is_symlink() or not path.is_file()
                or path.resolve() != path or not path.is_relative_to(guard.directory)):
            raise ValueError(f"The {name} WAV is not a regular private file.")
        sources[name] = path
    with guard.exporting_to(destination):
        destination.mkdir(mode=0o700)
        try:
            exported = {}
            for name, source in sources.items():
                target = destination / f"{name}.wav"
                shutil.copyfile(source, target)
                exported[name] = str(target)
            return exported
        except BaseException:
            # Use absolute owned paths: rmtree uses relative dir_fd operations.
            for name in ("voice", "mix"):
                (destination / f"{name}.wav").unlink(missing_ok=True)
            destination.rmdir()
            raise


def run_render(text=None, export=None, *, renderer_factory=None, guard_factory=None):
    """Run once in a standalone process and return measurements and failures."""
    started = time.monotonic()
    report = {"mode": "file_only", "status": "failed", "render": None,
              "audibleVerification": "unperformed", "pronunciationVerification": "unperformed",
              "guardChecks": [], "sideEffectAttempts": [], "errors": [],
              "privateOutputs": "not_created", "exportedFiles": {}}
    temporary = guard = None
    try:
        destination = export_destination(export) if export is not None else None
        temporary = tempfile.TemporaryDirectory(prefix="virtual-dj-voice-")
        directory = Path(temporary.name).resolve()
        report["privateOutputs"] = "pending_cleanup"
        guard = (guard_factory or RenderGuard)(directory)
        guard.install()
        guard.self_check()
        if renderer_factory is None:
            sys.path.insert(0, str(ROOT / "tools" / "party_dj"))
            from voice_render import VoiceRenderer
            renderer_factory = VoiceRenderer
        renderer = renderer_factory(directory)
        guard.renderer = renderer
        result = renderer.render() if text is None else renderer.render(text)
        result = dict(result)
        files = result.pop("files")
        report["render"] = result
        if guard.unexpected:
            raise RuntimeError("Rendering attempted a forbidden operation; inspect sideEffectAttempts.")
        if destination is not None:
            report["exportedFiles"] = export_files(files, destination, guard)
    except BaseException as error:
        item = {"type": type(error).__name__, "message": str(error)}
        if getattr(error, "stage", None):
            item["stage"] = error.stage
        if getattr(error, "return_code", None) is not None:
            item["returnCode"] = error.return_code
        stderr = getattr(error, "stderr", None)
        if stderr:
            item["stderr"] = (stderr.decode("utf-8", "replace") if isinstance(stderr, bytes)
                              else str(stderr))[:2000]
        report["errors"].append(item)
    finally:
        if temporary is not None:
            try:
                temporary.cleanup()
                report["privateOutputs"] = "cleaned"
            except BaseException as error:
                report["privateOutputs"] = "cleanup_failed"
                report["privateDirectory"] = temporary.name
                report["errors"].append({"type": type(error).__name__, "message": str(error)})
        if guard is not None:
            report["guardChecks"] = list(guard.checks)
            report["sideEffectAttempts"] = list(guard.unexpected)
        report["elapsedSeconds"] = round(time.monotonic() - started, 3)
    if (report["render"] is not None and not report["errors"]
            and not report["sideEffectAttempts"] and report["privateOutputs"] == "cleaned"):
        report["status"] = "passed"
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--text-stdin", action="store_true",
                        help="Read original dedication text from stdin instead of the built-in phrase.")
    parser.add_argument("--export", metavar="NEW_DIRECTORY",
                        help="Explicitly retain voice.wav and mix.wav in a new directory.")
    parser.add_argument("--json", action="store_true", help="Print one machine-readable result.")
    args = parser.parse_args(argv)
    text = None
    if args.text_stdin:
        text = sys.stdin.read(4097)
        if len(text) > 4096:
            parser.error("stdin text must contain no more than 4096 characters")
    report = run_render(text, args.export)
    if args.json:
        print(json.dumps(report, ensure_ascii=False, allow_nan=False))
    else:
        print(f"FILE-ONLY VOICE REHEARSAL {report['status'].upper()}")
        if report["render"] is not None:
            print(json.dumps(report["render"], ensure_ascii=False, indent=2, allow_nan=False))
        print(f"Guard checks: {len(report['guardChecks'])}; "
              f"unexpected side-effect attempts: {len(report['sideEffectAttempts'])}")
        print(f"Private outputs: {report['privateOutputs']}")
        for name, path in report["exportedFiles"].items():
            print(f"Exported {name}: {path}")
        for error in report["errors"]:
            print(f"{error['type']}: {error['message']}")
            if error.get("stderr"):
                print(error["stderr"])
        for attempt in report["sideEffectAttempts"]:
            print(f"BLOCKED {attempt['event']}: {attempt['reason']}")
        print("Pronunciation and audience hearing: unperformed")
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
