#!/usr/bin/env python3
"""Run the karaoke and dedication rehearsals in a standalone, silent process."""
from __future__ import annotations

import argparse
import contextlib
import io
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import unittest
from unittest.mock import patch
import urllib.request

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
SUITES = {"karaoke": "test_karaoke", "speech": "test_speech"}


class RehearsalViolation(RuntimeError):
    def __init__(self, event, reason):
        self.event = event
        super().__init__(f"Silent rehearsal blocked {event}: {reason}")


class SilentGuard:
    """Deny external effects for this process, including all test worker threads."""

    PROCESS_EVENTS = {
        "subprocess.Popen", "os.system", "os.posix_spawn", "os.exec",
        "os.fork", "os.forkpty", "pty.spawn",
    }
    MUTATIONS = {
        "os.remove": (1, False), "os.rmdir": (1, False),
        "os.mkdir": (2, False), "os.chmod": (2, True),
        "os.chown": (3, True), "os.utime": (3, True),
        "os.truncate": (None, True), "shutil.rmtree": (1, False),
    }

    def __init__(self, directory):
        self.directory = Path(directory).resolve()
        self.unexpected = []
        self.expected = []
        self.checks = []
        self._probe = None

    def install(self):
        # This hook stays installed through cleanup and process exit. Do not
        # invoke main() inside a long-lived production Python process.
        sys.addaudithook(self.audit)

    def reject(self, event, reason):
        item = {"event": event, "reason": reason}
        if self._probe == (threading.get_ident(), event):
            self.expected.append(item)
        else:
            self.unexpected.append(item)
        raise RehearsalViolation(event, reason)

    def private_path(self, event, value, dir_fd=None, *, follow=True):
        if isinstance(value, int):
            try:
                value = os.readlink(f"/proc/self/fd/{value}")
            except OSError:
                self.reject(event, "unknown file descriptor")
            if not value.startswith("/"):
                self.reject(event, "descriptor is not a private file")
        try:
            candidate = Path(os.fsdecode(value))
            if not candidate.is_absolute() and dir_fd is not None and dir_fd >= 0:
                directory = os.readlink(f"/proc/self/fd/{dir_fd}")
                if not directory.startswith("/"):
                    self.reject(event, "unknown directory descriptor")
                candidate = Path(directory) / candidate
            resolved = candidate.resolve() if follow else candidate.parent.resolve() / candidate.name
        except (OSError, TypeError, ValueError):
            self.reject(event, "unresolvable output path")
        if resolved != self.directory and not resolved.is_relative_to(self.directory):
            self.reject(event, "write outside the private rehearsal directory")

    def audit(self, event, args):
        if event in self.PROCESS_EVENTS:
            self.reject(event, "external process execution is disabled")
        if event.startswith("socket.") or event == "urllib.Request":
            self.reject(event, "network and local service connections are disabled")
        if event == "os.chdir":
            self.reject(event, "working-directory changes are disabled")
        if event == "open":
            name, mode, flags = args
            if isinstance(name, (str, bytes, os.PathLike)):
                device = os.fsdecode(name)
                if device.startswith(("/dev/snd", "/dev/dsp", "/dev/audio", "/dev/video")):
                    self.reject(event, "media devices are disabled")
            write_mode = isinstance(mode, str) and any(flag in mode for flag in "wax+")
            write_flags = isinstance(flags, int) and flags & (
                os.O_WRONLY | os.O_RDWR | os.O_CREAT | os.O_TRUNC | os.O_APPEND)
            if write_mode or write_flags:
                # The open audit event omits os.open's dir_fd. Only absolute
                # names have the same destination for every directory handle.
                if not isinstance(name, int) and not Path(os.fsdecode(name)).is_absolute():
                    self.reject(event, "relative write paths are disabled")
                self.private_path(event, name)
        elif event == "sqlite3.connect":
            database = os.fsdecode(args[0])
            if database != ":memory:":
                if database.startswith("file:"):
                    self.reject(event, "SQLite URI targets are disabled")
                self.private_path(event, database)
        elif event == "os.rename":
            source, destination, source_fd, destination_fd = args
            self.private_path(event, source, source_fd, follow=False)
            self.private_path(event, destination, destination_fd, follow=False)
        elif event in ("os.symlink", "os.link"):
            self.reject(event, "filesystem links are not used by the rehearsal")
        elif event in self.MUTATIONS:
            fd_index, follow = self.MUTATIONS[event]
            dir_fd = args[fd_index] if fd_index is not None and len(args) > fd_index else None
            self.private_path(event, args[0], dir_fd, follow=follow)

    def probe(self, name, event, operation):
        """Accept exactly one deliberate denial on this thread and this event."""
        before = len(self.expected)
        self._probe = (threading.get_ident(), event)
        try:
            try:
                operation()
            except RehearsalViolation as error:
                if error.event != event:
                    raise AssertionError(f"{name}: blocked at {error.event}, expected {event}") from error
            else:
                raise AssertionError(f"{name}: the guard did not reject the operation")
            if len(self.expected) != before + 1:
                raise AssertionError(f"{name}: expected one recorded denial")
        finally:
            self._probe = None
        self.checks.append(name)

    def self_check(self):
        # The sentinels prevent an actual child or HTTP request even if a guard
        # regression makes either of these two real entry-path probes fail.
        with patch.object(subprocess, "_fork_exec",
                          side_effect=AssertionError("process sentinel reached")) as child:
            self.probe("process entry", "subprocess.Popen",
                       lambda: subprocess.Popen([str(self.directory / "never-executed")],
                                                close_fds=True))
            child.assert_not_called()
        with patch.object(urllib.request.OpenerDirector, "_open",
                          side_effect=AssertionError("HTTP sentinel reached")) as network:
            self.probe("HTTP entry", "urllib.Request",
                       lambda: urllib.request.urlopen("http://rehearsal.invalid/guard", timeout=1))
            network.assert_not_called()
        # Alternate dispatch paths are checked without performing their
        # underlying operation, including exec, which would replace this CLI.
        for event, args in [
            ("os.system", ("unused rehearsal command",)),
            ("os.posix_spawn", (str(self.directory / "unused"), [], {})),
            ("os.exec", (str(self.directory / "unused"), [], {})),
            ("os.fork", ()),
            ("os.chdir", (str(self.directory),)),
            ("socket.__new__", (None, 2, 1, 0)),
            ("socket.connect", (None, ("rehearsal.invalid", 1))),
            ("socket.bind", (None, ("127.0.0.1", 0))),
            ("socket.getaddrinfo", ("rehearsal.invalid", 1, 0, 0, 0)),
        ]:
            self.probe(event, event, lambda event=event, args=args: sys.audit(event, *args))
        outside = self.directory.parent / (self.directory.name + "-outside")
        for name, event, args in [
            ("external file", "open", (str(outside), "w", os.O_WRONLY | os.O_CREAT)),
            ("external database", "sqlite3.connect", (str(outside),)),
            ("external rename", "os.rename", (str(outside), str(outside), -1, -1)),
            ("external remove", "os.remove", (str(outside), -1)),
            ("relative file", "open", ("guard-check.txt", "w", os.O_WRONLY | os.O_CREAT)),
            ("stdout mutation", "os.truncate", (1, 0)),
        ]:
            self.probe(name, event, lambda event=event, args=args: sys.audit(event, *args))
        scratch = self.directory / "guard-check.txt"
        scratch.write_text("Original silent rehearsal fixture.\n", encoding="utf-8")
        renamed = scratch.with_suffix(".moved")
        scratch.replace(renamed)
        renamed.unlink()
        with contextlib.closing(sqlite3.connect(self.directory / "guard-check.sqlite3")) as database:
            database.execute("CREATE TABLE rehearsal(value INTEGER)")
        (self.directory / "guard-check.sqlite3").unlink()
        self.checks.append("private files and database")


def run_rehearsal(selected, timeout):
    """Return a report and whether a stuck test requires immediate process exit."""
    started = time.monotonic()
    baseline_threads = {thread.ident for thread in threading.enumerate()}
    report = {"mode": "silent", "status": "failed",
              "audibleVerification": "unperformed", "suites": [],
              "guardChecks": [], "sideEffectAttempts": [], "threadsRemaining": [],
              "errors": [], "privateOutputs": "pending_cleanup"}
    temporary = tempfile.TemporaryDirectory(prefix="virtual-dj-rehearsal-")
    directory = Path(temporary.name).resolve()
    tempfile.tempdir = str(directory)
    os.environ["TMPDIR"] = str(directory)
    os.environ["SQLITE_TMPDIR"] = str(directory)
    guard = SilentGuard(directory)
    guard.install()
    output = io.StringIO()
    suite_thread = None
    forced_exit = False
    cancelled = threading.Event()
    result_lock = threading.Lock()
    suite_results = []
    suite_errors = []
    try:
        guard.self_check()
        sys.path.insert(0, str(ROOT / "tools" / "party_dj"))

        def exercise():
            try:
                for name in selected:
                    if cancelled.is_set():
                        break
                    loader = unittest.TestLoader()
                    suite = loader.loadTestsFromName(SUITES[name])
                    result = unittest.TextTestRunner(stream=output, verbosity=2, buffer=True).run(suite)
                    item = {
                        "name": name, "tests": result.testsRun,
                        "passed": result.testsRun > 0 and result.wasSuccessful() and not result.skipped,
                        "failures": [{"test": test.id(), "traceback": trace}
                                     for test, trace in result.failures + result.errors],
                        "skipped": [{"test": test.id(), "reason": reason}
                                    for test, reason in result.skipped],
                    }
                    with result_lock:
                        suite_results.append(item)
                        if not result.testsRun:
                            suite_errors.append(f"Selected suite {name} contained no tests.")
            except BaseException:
                with result_lock:
                    suite_errors.append(traceback.format_exc())

        # A stuck test cannot keep the command alive through a non-daemon
        # worker's interpreter shutdown. Denials remain active until exit.
        suite_thread = threading.Thread(target=exercise, name="silent-rehearsal-tests", daemon=True)
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            suite_thread.start()
            suite_thread.join(timeout)
        if suite_thread.is_alive():
            cancelled.set()
            report["errors"].append(f"Selected rehearsal suites exceeded {timeout:g} seconds.")
            forced_exit = True
    except BaseException:
        report["errors"].append(traceback.format_exc())
    finally:
        cancelled.set()
        with result_lock:
            report["suites"] = list(suite_results)
            report["errors"].extend(suite_errors)
        remaining = [thread for thread in threading.enumerate()
                     if thread.ident not in baseline_threads and thread.is_alive()]
        report["threadsRemaining"] = [thread.name for thread in remaining]
        forced_exit = forced_exit or bool(remaining)
        try:
            temporary.cleanup()
            report["privateOutputs"] = "cleanup_unresolved" if remaining else "cleaned"
            if remaining:
                report["privateDirectory"] = str(directory)
        except BaseException:
            report["errors"].append(traceback.format_exc())
            report["privateOutputs"] = "cleanup_failed"
        report["guardChecks"] = guard.checks
        report["sideEffectAttempts"] = list(guard.unexpected)
        report["elapsedSeconds"] = round(time.monotonic() - started, 3)
        report["testLog"] = output.getvalue()
    passed = (len(report["suites"]) == len(selected)
              and all(item["passed"] for item in report["suites"])
              and not report["errors"] and not report["sideEffectAttempts"]
              and not report["threadsRemaining"] and report["privateOutputs"] == "cleaned")
    report["status"] = "passed" if passed else "failed"
    return report, forced_exit


def main(argv=None):
    # A timed-out unittest worker can still restore global sys.stdout/stderr.
    # Keep the report attached to the streams supplied to this standalone CLI.
    stdout, stderr = sys.stdout, sys.stderr
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=["all", *SUITES], default="all")
    parser.add_argument("--json", action="store_true", help="Print a machine-readable result to stdout.")
    parser.add_argument("--verbose", action="store_true", help="Include the individual silent test results.")
    parser.add_argument("--timeout", type=float, default=60,
                        help="Maximum seconds for the selected test suites (default: 60).")
    args = parser.parse_args(argv)
    if not 0 < args.timeout <= 600:
        parser.error("--timeout must be greater than zero and no more than 600 seconds")
    selected = list(SUITES) if args.suite == "all" else [args.suite]
    report, forced_exit = run_rehearsal(selected, args.timeout)
    if args.json:
        if not args.verbose and report["status"] == "passed":
            report.pop("testLog", None)
        print(json.dumps(report, ensure_ascii=False), file=stdout)
    else:
        count = sum(item["tests"] for item in report["suites"])
        print(f"SILENT REHEARSAL {report['status'].upper()}: {count} tests", file=stdout)
        for item in report["suites"]:
            label = "PASS" if item["passed"] else "FAIL"
            print(f"  {label} {item['name']}: {item['tests']} tests", file=stdout)
        print(f"  Guard checks: {len(report['guardChecks'])}; "
              f"unexpected side-effect attempts: {len(report['sideEffectAttempts'])}", file=stdout)
        print(f"  Private outputs: {report['privateOutputs']}", file=stdout)
        print("  Audience audibility: unperformed (silent fixtures and fake playback only)", file=stdout)
        if args.verbose or report["status"] != "passed":
            if report["testLog"]:
                print(report["testLog"], end="", file=stdout)
            for error in report["errors"]:
                print(error, file=stdout)
            for attempt in report["sideEffectAttempts"]:
                print(f"  BLOCKED {attempt['event']}: {attempt['reason']}", file=stdout)
            if report["threadsRemaining"]:
                print("  Unclosed threads: " + ", ".join(report["threadsRemaining"]), file=stdout)
    exit_code = 0 if report["status"] == "passed" else 1
    if forced_exit:
        stdout.flush()
        stderr.flush()
        os._exit(exit_code or 1)
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
