"""Where the demo is shown when something automated runs it.

On a display of its own by default, so a test or a validation run does not put
a window on somebody's screen while they are working. Set
RUNTIME_AGENT_TEST_DISPLAY=host to use the display this shell already has,
which is what to do when watching the cube is the point.

The server is started here and stopped here, and the application is launched
directly against it. Wrapping each launch in xvfb-run instead puts a wrapper
between the caller and the process it has to stop, and terminating the wrapper
leaves the application and the server behind. That is not hypothetical: it left
five cubes and five Xvfb servers running.

Both the activation tests and tools/smoke_test.py use this, so there is one
answer to which display the application gets and one implementation that stops
what it started.
"""

from __future__ import annotations

import os
import select
import shutil
import subprocess
import time

def on_host() -> bool:
    """Whether the caller asked to use the display this shell already has."""
    return os.environ.get("RUNTIME_AGENT_TEST_DISPLAY") == "host"


def can_draw() -> bool:
    """Whether a display can be had at all.

    Ask this before starting anything: it reports whether one is obtainable,
    not whether one is running.
    """
    if on_host():
        return bool(os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY"))
    return shutil.which("Xvfb") is not None


class OwnDisplay:
    """An Xvfb server this process starts, owns, and stops."""

    def __init__(self) -> None:
        self.number: str | None = None
        self.server: subprocess.Popen | None = None

    def start(self) -> str | None:
        """Returns the display to use, or None if there is none to be had.

        None is also what asking for the host display gets, because then there
        is nothing for this to start.
        """
        if on_host():
            return None
        if shutil.which("Xvfb") is None:
            return None

        # The server chooses the display and says which one it took.
        #
        # Choosing here means looking for a free number and then starting a
        # server on it, and two runs that look at the same moment pick the same
        # number. The loser then sees a socket, reports success, and both use one
        # server; when the winner stops it, the loser's application loses its
        # display mid-run. Waiting for the socket to appear cannot tell them
        # apart either, because the socket it sees may be the winner's.
        #
        # -displayfd hands that to Xvfb: it takes a free number itself and writes
        # it to this pipe once it is ready to accept connections, so what comes
        # back names a server this call started and one that is already up.
        readable, writable = os.pipe()
        try:
            self.server = subprocess.Popen(
                ["Xvfb", "-displayfd", str(writable), "-screen", "0", "1280x800x24",
                 "+extension", "GLX", "+render", "-noreset"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                pass_fds=(writable,),
            )
        except OSError:
            # Only the read end here. The finally below owns the write end on
            # every path, and closing it twice raises EBADF out of the finally,
            # which would replace this None with an exception on the one path
            # that exists to fail quietly.
            os.close(readable)
            return None
        finally:
            # This end belongs to the child. Holding it open here would leave the
            # read below waiting for a writer that is this process.
            os.close(writable)

        try:
            reported = b""
            deadline = time.monotonic() + 10
            while b"\n" not in reported:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                ready, _, _ = select.select([readable], [], [], remaining)
                if not ready:
                    break
                chunk = os.read(readable, 64)
                if not chunk:
                    break       # The server closed it without saying a number.
                reported += chunk
        finally:
            os.close(readable)

        number = reported.decode(errors="replace").strip()
        if number.isdigit() and self.server.poll() is None:
            self.number = f":{number}"
            return self.number

        self.stop()
        return None

    def stop(self) -> None:
        """Stops the server, and is safe to call more than once."""
        if self.server is not None:
            self.server.terminate()
            try:
                self.server.wait(10)
            except subprocess.TimeoutExpired:
                self.server.kill()
                self.server.wait(5)
            self.server = None
        self.number = None

    def __enter__(self) -> "OwnDisplay":
        self.start()
        return self

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        self.stop()


def child_environment(display: OwnDisplay | None = None) -> dict[str, str]:
    """The environment to launch the application with.

    Raises RuntimeError when a private display was wanted and there is none.
    The alternative is using whichever display is around, which puts a window on
    somebody's screen at the moment something has already gone wrong.
    """
    environment = dict(os.environ)
    # A virtual display has no GPU behind it, and the host one may be remote.
    environment.setdefault("LIBGL_ALWAYS_SOFTWARE", "1")
    if on_host():
        if not can_draw():
            raise RuntimeError("asked for the host display and there is none")
        return environment

    # Whatever this shell was given goes, whether or not a private display was
    # started, so nothing can inherit its way onto somebody's screen.
    environment.pop("WAYLAND_DISPLAY", None)
    environment.pop("DISPLAY", None)
    if display is None or display.number is None:
        raise RuntimeError(
            "there is no display of our own to draw on, and using the one this shell "
            "has must be asked for with RUNTIME_AGENT_TEST_DISPLAY=host"
        )
    environment["DISPLAY"] = display.number
    # Qt prefers Wayland where the environment offers one, which would be the
    # session on somebody's screen.
    environment["QT_QPA_PLATFORM"] = "xcb"
    return environment
