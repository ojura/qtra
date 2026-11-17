"""The party stage follows atomic state updates and sends real widget requests.

Uses a private X display, a private qtra process, and a local HTTP fixture. No
Spotify account, desktop window, or party state file is changed by this test.
"""

from __future__ import annotations

import json
import copy
import ctypes
import os
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import threading
import time
import unittest
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from agentctl import AgentClient
from private_display import OwnDisplay, can_draw, child_environment

BUILD = Path(os.environ.get("RUNTIME_AGENT_TEST_BUILD_DIR", ROOT / "build" / "release"))


def cover_png(rgb, width=64, height=64):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    pixels = (b"\0" + bytes(rgb) * width) * height
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND", b""))


class Service(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, value):
        body = json.dumps(value).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        url = urlsplit(self.path)
        if url.path == "/api/state":
            self.server.state_reads += 1
            state = copy.deepcopy(self.server.state_snapshot)
            if self.server.state_reply_gate:
                self.server.state_reply_gate.wait(10)
            return self.reply(state)
        if url.path.startswith("/cover/"):
            self.server.cover_requests.append((url.path, self.headers.get("Authorization")))
            if url.path == "/cover/slow.png" and self.server.cover_reply_gate:
                self.server.cover_reply_gate.wait(10)
            body = self.server.covers.get(url.path, cover_png((80, 160, 200)))
            self.send_response(200); self.send_header("Content-Type", "image/png")
            self.send_header("Content-Length", str(len(body))); self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        if url.path == "/api/search/describe":
            identifier = parse_qs(url.query)["id"][0]
            self.server.description_polls.append(identifier)
            result = dict(self.server.descriptions[identifier])
            if self.server.description_reply_gate:
                self.server.description_reply_gate.wait(10)
            return self.reply({"search": result})
        if url.path == "/api/search":
            values = parse_qs(url.query); query = values["q"][0]
            identifier = values.get("searchId", [""])[0]
            self.server.search_attempts.append((query, identifier))
            self.server.searches.append(query)
            if query in self.server.search_gates:
                self.server.search_gates[query].wait(10)
            if identifier in self.server.search_cancelled:
                return self.reply({"tracks": [], "cancelled": True, "searchId": identifier})
            self.server.search_admitted.append((query, identifier))
            def search_reply(value):
                return self.reply({"searchId": self.server.search_reply_ids.get(query, identifier), "cancelled": False, **value})
            if query in self.server.search_tracks:
                return search_reply({"tracks": copy.deepcopy(self.server.search_tracks[query])})
            if query == "http failure":
                self.send_error(503, "Private catalogue outage")
                return
            if query == "offline":
                return search_reply({"error": "Search unavailable"})
            if query == "slow":
                time.sleep(.7)
            title = "Robot karaoke" if query == "robot" else f"Song {query}"
            if query == "long":
                title = "A very long song title " * 25
            return search_reply({"tracks": [{"id": "test", "uri": "spotify:track:test",
                                           "title": title, "artist": "LARICS house band",
                                           "album": "After hours", "durationMs": 180000}]})
        if url.path == "/api/chat":
            after = int(parse_qs(url.query).get("after", [0])[0])
            self.server.chat_requests += 1
            data = copy.deepcopy({"messages": [m for m in self.server.messages if m["id"] > after and m["id"] not in self.server.withdrawn_ids],
                               "withdrawnMessageIds": self.server.withdrawn_ids,
                               "dj": {"status": "listening", "lastSeen": int(time.time() * 1000),
                                      "llm": self.server.llm,
                                      "processingMessageIds": self.server.processing_ids, "messageWork": self.server.message_work}})
            if self.server.chat_reply_gate:
                self.server.chat_reply_gate.wait(10)
            self.reply(data); self.server.chat_last_llm_reply = data["dj"]["llm"]; self.server.chat_finished += 1
            return
        self.send_error(404)

    def do_POST(self):
        data = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if self.path == "/api/search/cancel":
            self.server.search_cancels.append(data)
            self.server.search_cancelled.add(data["searchId"])
            if self.server.search_cancel_reply_gate:
                self.server.search_cancel_reply_gate.wait(10)
            return self.reply({"searchId": data["searchId"], "cancelRequested": True})
        if self.path == "/api/admin/playback":
            self.server.playback_requests.append(data)
            self.server.playback_operators.append(self.headers.get("X-DJ-Operator"))
            if self.headers.get("Authorization") != "Bearer private-fixture-token":
                return self.reply({"error": "Private fixture authentication failed", "dispatched": False})
            state = copy.deepcopy(self.server.state_snapshot)
            now, actual = state["nowPlaying"], state["playbackState"]
            stamp = int(time.time() * 1000)
            state["spotify"]["observedAt"] = stamp; now["observedAt"] = stamp
            self.server.state_snapshot = state
            if data.get("expectedCurrentUid") != now.get("uid") or data.get("expectedDeviceId") != actual.get("device", {}).get("id"):
                return self.reply({"error": "Observed target changed", "dispatched": False})
            error = self.server.playback_error
            if not error or error.get("dispatched") is True:
                now.update(paused=data["paused"], playing=not data["paused"]); actual["paused"] = data["paused"]
                self.server.playback_commands.append(data["paused"])
                self.server.state_snapshot = state
            if self.server.playback_reply_gate:
                self.server.playback_reply_gate.wait(10)
            if error:
                return self.reply(error)
            observed = {"nowPlaying": now, "playbackState": actual, "observedAt": stamp, "queue": state.get("queue", [])}
            return self.reply({"ok": True, "action": "setPaused", "paused": data["paused"], "changed": True,
                               "device": actual["device"], "status": observed})
        if self.path == "/api/search/describe":
            self.server.description_posts.append(data)
            identifier = data["requestId"]
            result = self.server.descriptions.setdefault(identifier, {"id": identifier, "text": data["text"],
                "status": "pending", "tracks": [], "note": ""})
            return self.reply({"search": result})
        if self.path == "/api/message/recall":
            self.server.recall_attempts.append(data)
            if self.server.reject_recalls:
                self.server.reject_recalls -= 1
                return self.reply({"error": "Recall interrupted; try again"})
            identifier = data["messageId"]
            if identifier in self.server.recalled:
                return self.reply({"recalled": self.server.recalled[identifier]})
            own = [m for m in self.server.messages if m.get("guestId") == data["guestId"] and m["id"] not in self.server.withdrawn_ids]
            if not own or own[-1]["id"] != identifier:
                return self.reply({"error": "Only your latest message can be recalled."})
            message = own[-1]
            if message.get("acceptedAt") or str(identifier) in self.server.message_work or identifier in self.server.processing_ids:
                return self.reply({"error": "The DJ has already accepted this message."})
            result = {"id": identifier, "text": message["text"], "requestId": message["requestId"]}
            self.server.recalled[identifier] = result
            self.server.withdrawn_ids.append(identifier)
            if self.server.lose_recall_replies:
                self.server.lose_recall_replies -= 1
                return self.reply({"error": "Recall reply lost; try again"})
            if self.server.recall_reply_gate:
                self.server.recall_reply_gate.wait(10)
            return self.reply({"recalled": result})
        if self.path == "/api/request":
            self.server.request_attempts.append(data)
            if self.server.reject_requests:
                self.server.reject_requests -= 1
                return self.reply({"error": "Request interrupted; try again"})
            self.server.requests.append(data)
            return self.reply({"status": "queued"})
        if self.path == "/api/message":
            self.server.message_attempts.append(data)
            if self.server.reject_messages:
                self.server.reject_messages -= 1
                return self.reply({"error": "Message interrupted; try again"})
            self.server.posts.append(data)
            message = {"id": len(self.server.messages) + 1, "role": "user",
                       "name": data["name"], "text": data["text"], "createdAt": int(time.time() * 1000),
                       "guestId": data["guestId"], "requestId": data["requestId"], "status": "delivered", "acceptedAt": None}
            self.server.messages.append(message)
            if self.server.message_reply_gate:
                self.server.message_reply_gate.wait(10)
            return self.reply({"message": message})
        self.send_error(404)


@unittest.skipUnless(can_draw(), "a private display is required")
class PartyStage(unittest.TestCase):
    def setUp(self):
        for path in (BUILD / "qt_runtime_cube", BUILD / "agent_snippet_party_stage.so",
                     BUILD / "agent_snippet_orbit_sphere.so", BUILD / "agent_snippet_mobius_ring.so"):
            self.assertTrue(path.exists(), f"build the party stage first: {path}")
        self.directory = tempfile.TemporaryDirectory(prefix="qtra-party-")
        self.addCleanup(self.directory.cleanup)
        self.folder = Path(self.directory.name)
        self.display = OwnDisplay()
        self.assertIsNotNone(self.display.start())
        self.addCleanup(self.display.stop)
        self.assertIsNotNone(self.display.server)
        self.assertIsNone(self.display.server.poll())
        environment = child_environment(self.display)
        self.assertEqual(environment["DISPLAY"], self.display.number)
        self.assertEqual(environment["QT_QPA_PLATFORM"], "xcb")
        self.assertNotIn("WAYLAND_DISPLAY", environment)
        environment["XDG_CONFIG_HOME"] = str(self.folder / "config")
        self.environment = environment
        self.log = (self.folder / "qtra.log").open("w+")
        self.addCleanup(self.log.close)
        self.process = subprocess.Popen([str(BUILD / "qt_runtime_cube"), "--agent-socket", str(self.folder / "agent.sock")],
                                        env=environment, stdout=self.log, stderr=self.log)
        self.addCleanup(self.stop)
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            self.assertIsNone(self.process.poll(), "qtra exited during startup")
            self.client = AgentClient(str(self.folder / "agent.sock"), timeout=10)
            try:
                self.client.connect()
                break
            except OSError:
                self.client.close()
                time.sleep(.05)
        else:
            self.fail("qtra socket did not appear")
        self.addCleanup(self.client.close)
        for name in ("orbit_sphere", "mobius_ring"):
            module = self.client.request("snippet.load", {"path": str(BUILD / f"agent_snippet_{name}.so")})["moduleId"]
            operation = self.client.request("snippet.run", {"moduleId": module, "executor": "render", "request": {"restore": True}})
            finished = self.client.wait_for_operation(operation["operationId"], timeout=10)["data"]
            self.assertEqual(finished["outcome"], "completed", finished)
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Service)
        self.assertNotEqual(self.server.server_port, 8787, "the fixture must not use the party service port")
        self.assertEqual(self.client.request("hello", {})["pid"], self.process.pid)
        self.server.searches, self.server.requests, self.server.posts = [], [], []
        self.server.search_tracks = {}; self.server.llm = {}
        self.server.search_attempts, self.server.search_admitted, self.server.search_cancels = [], [], []
        self.server.search_gates, self.server.search_cancelled, self.server.search_reply_ids = {}, set(), {}
        self.server.search_cancel_reply_gate = None
        self.server.chat_requests = self.server.chat_finished = 0
        self.server.chat_reply_gate = None
        self.server.request_attempts, self.server.message_attempts = [], []
        self.server.reject_requests = self.server.reject_messages = 0
        self.server.message_reply_gate = None
        self.server.processing_ids = []
        self.server.message_work = {}
        self.server.state_reads = 0
        self.server.state_snapshot = {}
        self.server.cover_requests, self.server.playback_requests, self.server.playback_commands, self.server.playback_operators = [], [], [], []
        self.server.covers = {}
        self.server.cover_reply_gate = self.server.playback_reply_gate = None
        self.server.state_reply_gate = None
        self.server.playback_error = None
        self.server.withdrawn_ids, self.server.recall_attempts, self.server.description_posts, self.server.description_polls = [], [], [], []
        self.server.recalled, self.server.descriptions = {}, {}
        self.server.reject_recalls = self.server.lose_recall_replies = 0
        self.server.recall_reply_gate = self.server.description_reply_gate = None
        self.server.messages = [{"id": 1, "role": "assistant", "name": "DJ",
                                 "text": "Welcome to the lab. What should play next?", "createdAt": int(time.time() * 1000)}]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.addCleanup(self.stop_server)
        self.module = self.client.request("snippet.load", {"path": str(BUILD / "agent_snippet_party_stage.so")})["moduleId"]
        self.state = {"revision": 1, "autoJokes": False,
                      "requestLocalUrl": f"http://127.0.0.1:{self.server.server_port}",
                      "nowPlaying": {"title": "Atomic Song", "artist": "LARICS house band", "playing": True,
                                     "durationMs": 180000, "progressMs": 30000, "observedAt": int(time.time() * 1000)},
                      "scene": {"mode": "robots", "energy": .7}, "mood": {"label": "Dobra frekvencija"}}
        self.state_file = self.folder / "state.json"
        self.write_state()

    def stop(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(5)

    def stop_server(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(3)

    def write_state(self):
        self.server.state_snapshot = copy.deepcopy(self.state)
        staged = self.state_file.with_suffix(".new")
        staged.write_text(json.dumps(self.state))
        staged.replace(self.state_file)

    def run_stage(self, **request):
        # The explicit private URL must reach the constructor before any
        # visibility-triggered fetch, even when this test supplies invalid state.
        request.setdefault("requestBaseUrl", getattr(self, "request_base_url", f"http://127.0.0.1:{self.server.server_port}"))
        started = self.client.request("snippet.run", {"moduleId": self.module, "executor": "gui", "request": request})
        finished = self.client.wait_for_operation(started["operationId"], timeout=10)["data"]
        self.assertEqual(finished["outcome"], "completed", finished)
        return finished["result"]

    def eventually(self, check, message, seconds=8):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if check():
                return
            time.sleep(.08)
        self.fail(message)

    def set_text(self, object_name, value):
        prop = "plainText" if object_name == "partyMessage" else "text"
        self.client.request("object.set", {"objectName": object_name, "property": prop, "value": value})

    def composer_text(self):
        return self.client.request("object.get", {"objectName": "partyMessage", "property": "plainText"})

    def editor_draft(self):
        draft = self.run_stage(getComposerDraft=True)["composerDraft"]
        return {key: draft[key] for key in ("text", "cursor", "anchor", "query", "queryCursor", "queryAnchor", "selectedTrack")}

    def magic_bounds(self):
        # The generic property bridge does not serialize QRect; use the native
        # panel's explicit measurements of the actual widgets.
        return self.run_stage()["panelBounds"]

    def key(self, combination):
        windows = subprocess.check_output(["xdotool", "search", "--pid", str(self.process.pid), "--name", "LARICS"],
                                          env=self.environment, text=True).splitlines()
        self.assertTrue(windows)
        subprocess.run(["xdotool", "windowfocus", windows[-1]], env=self.environment, check=True)
        self.client.request("object.invoke", {"objectName": "partyMessage", "method": "setFocus"})
        subprocess.run(["xdotool", "key", "--clearmodifiers", combination], env=self.environment, check=True)

    def cursor_image(self):
        # Read the server's actual displayed ARGB cursor, not QWidget's cached
        # cursor property. This connection is only to the fixture-owned Xvfb.
        self.assertIsNotNone(self.display.server)
        self.assertIsNone(self.display.server.poll())
        self.assertEqual(self.environment["DISPLAY"], self.display.number)
        class CursorImage(ctypes.Structure):
            _fields_ = [("x", ctypes.c_short), ("y", ctypes.c_short),
                        ("width", ctypes.c_ushort), ("height", ctypes.c_ushort),
                        ("xhot", ctypes.c_ushort), ("yhot", ctypes.c_ushort),
                        ("serial", ctypes.c_ulong), ("pixels", ctypes.POINTER(ctypes.c_ulong)),
                        ("atom", ctypes.c_ulong), ("name", ctypes.c_char_p)]
        x11, fixes = ctypes.CDLL("libX11.so.6"), ctypes.CDLL("libXfixes.so.3")
        x11.XOpenDisplay.argtypes = [ctypes.c_char_p]; x11.XOpenDisplay.restype = ctypes.c_void_p
        x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
        x11.XFree.argtypes = [ctypes.c_void_p]
        fixes.XFixesGetCursorImage.argtypes = [ctypes.c_void_p]
        fixes.XFixesGetCursorImage.restype = ctypes.POINTER(CursorImage)
        display = x11.XOpenDisplay(self.display.number.encode())
        self.assertTrue(display)
        try:
            image = fixes.XFixesGetCursorImage(display)
            self.assertTrue(image)
            try:
                item = image.contents
                return {"width": item.width, "height": item.height,
                        "opaque": sum((item.pixels[i] >> 24) & 255 > 0 for i in range(item.width * item.height))}
            finally:
                x11.XFree(image)
        finally:
            x11.XCloseDisplay(display)

    def test_playback_feedback_cursor_and_shortcuts_preserve_editor_intent(self):
        (self.folder / "admin-token").write_text("private-fixture-token")
        stamp = int(time.time() * 1000)
        self.state["spotify"] = {"connected": True, "observedAt": stamp}
        self.state["playbackState"] = {"paused": False, "buffering": False, "device": {"id": "fixture-device"}}
        self.state["nowPlaying"].update(uid="feedback-fixture", paused=False, playing=True, clockValid=True)
        self.write_state(); self.run_stage(stateFile=str(self.state_file), fullscreen=False)
        windows = subprocess.check_output(["xdotool", "search", "--pid", str(self.process.pid), "--name", "LARICS"],
                                          env=self.environment, text=True).splitlines()
        self.assertTrue(windows)
        subprocess.run(["xdotool", "windowsize", windows[-1], "2560", "1600"], env=self.environment, check=True)
        control = lambda: self.run_stage()["playbackControl"]
        self.eventually(lambda: control()["enabled"], "private playback control never became ready")
        self.run_stage(composerDraft={"text": "Keep this exact draft and selection.", "cursor": 20, "anchor": 5})
        draft, bounds = self.editor_draft(), self.magic_bounds()
        button = control()["bounds"]
        subprocess.run(["xdotool", "mousemove", "--sync", "--window", windows[-1],
                        str(button[0] + button[2] // 2), str(button[1] + button[3] // 2)], env=self.environment, check=True)
        self.eventually(lambda: self.cursor_image()["width"] == 48 and self.cursor_image()["opaque"] > 100,
                        "the actual large DJ pointer was not visible over the button")
        label = control()["label"]
        self.server.playback_reply_gate = threading.Event()
        subprocess.run(["xdotool", "click", "1"], env=self.environment, check=True)
        try:
            self.eventually(lambda: control()["busy"] and len(self.server.playback_requests) == 1, "pointer click did not start one command")
            self.assertEqual(control()["label"], label, "verification replaced the observed play/pause label")
            self.assertIn("Provjeravam", control()["verification"])
            self.eventually(lambda: self.cursor_image()["width"] == 48 and self.cursor_image()["opaque"] > 100,
                            "the stationary cursor was not restored after the disabled transition", seconds=2)
            self.key("ctrl+p")
            self.assertEqual(len(self.server.playback_requests), 1, "Ctrl+P duplicated an in-flight click")
        finally:
            self.server.playback_reply_gate.set()
        self.eventually(lambda: control()["enabled"] and control()["paused"], "the confirmed pause did not arrive")
        self.assertEqual(control()["verification"], "")
        self.assertGreater(self.cursor_image()["opaque"], 100, "the stationary cursor stayed hidden after confirmation")
        self.assertEqual(self.editor_draft(), draft)
        self.client.request("object.invoke", {"objectName": "partyName", "method": "setFocus"})
        subprocess.run(["xdotool", "key", "--clearmodifiers", "ctrl+k"], env=self.environment, check=True)
        self.eventually(lambda: self.client.request("object.get", {"objectName": "partyMessage", "property": "focus"}), "Ctrl+K did not focus the Magic box")
        self.key("ctrl+p")
        self.eventually(lambda: len(self.server.playback_requests) == 2 and control()["enabled"] and not control()["paused"],
                        "Ctrl+P did not use the guarded explicit resume action")
        self.assertEqual(self.server.playback_commands, [True, False])
        self.assertEqual(self.editor_draft(), draft)
        self.assertEqual(self.magic_bounds(), bounds)
        self.state = copy.deepcopy(self.server.state_snapshot); self.state["revision"] += 1
        self.state["spotify"]["observedAt"] -= 11000; self.write_state()
        self.eventually(lambda: not control()["enabled"], "stale playback remained actionable")
        self.key("ctrl+p"); self.assertEqual(len(self.server.playback_requests), 2, "stale Ctrl+P reached transport")
        self.assertGreater(self.cursor_image()["opaque"], 100, "the cursor disappeared over unavailable playback")

    def test_llm_presence_expires_without_relying_on_watcher_or_new_json(self):
        self.run_stage(stateFile=str(self.state_file), fullscreen=False)
        presence = lambda: self.run_stage()["djPresence"]
        self.eventually(lambda: self.run_stage()["chatCount"] == 1, "fixture chat did not arrive")
        self.assertEqual(presence()["status"], "unavailable", "legacy watcher listening was treated as LLM availability")
        bounds = self.magic_bounds()
        stamp = int(time.time() * 1000)
        self.server.llm = {"status": "online", "lastSeen": stamp, "expiresAt": stamp + 30000, "remainingMs": 2600, "activeCount": 1}
        self.eventually(lambda: presence()["status"] == "online", "valid explicit LLM lease was not shown")
        self.assertIn("online", self.client.request("object.get", {"objectName": "partyDjOnlineBadge", "property": "text"}))
        self.eventually(lambda: presence()["status"] == "offline", "unchanged lease did not expire locally", seconds=4)
        time.sleep(1.6)
        self.assertEqual(presence()["status"], "offline", "repeated identical JSON renewed an expired lease")
        self.server.llm = {"status": "online", "lastSeen": stamp, "expiresAt": "later", "remainingMs": 10000, "activeCount": 1}
        self.eventually(lambda: presence()["status"] == "unavailable", "invalid lease remained online")
        stamp = int(time.time() * 1000)
        self.server.llm = {"status": "online", "lastSeen": stamp, "expiresAt": stamp + 120000, "remainingMs": 120000, "activeCount": 256}
        self.eventually(lambda: presence()["activeCount"] == 256, "the agreed maximum agent count was rejected")
        self.server.llm = {"status": "online", "lastSeen": stamp, "expiresAt": stamp + 120001, "remainingMs": 10000, "activeCount": 1}
        self.eventually(lambda: presence()["status"] == "unavailable", "an excessive lease horizon was accepted")
        self.server.llm = {"status": "online", "lastSeen": stamp, "expiresAt": stamp + 120000, "remainingMs": 120000, "activeCount": 256}
        self.eventually(lambda: presence()["activeCount"] == 256, "the agreed maximum agent count was rejected")
        self.server.llm = {**self.server.llm, "activeCount": 257}
        self.eventually(lambda: presence()["status"] == "unavailable", "an out-of-contract agent count was accepted")
        self.server.llm = {"status": "offline", "lastSeen": int(time.time() * 1000), "expiresAt": 0, "remainingMs": 0, "activeCount": 0}
        self.eventually(lambda: presence()["status"] == "offline", "explicit offline state was not rendered")
        self.assertEqual(self.magic_bounds(), bounds, "presence changed the composer geometry")
        self.assertFalse(self.server.playback_requests)

    def test_presence_same_expiry_metadata_and_offline_replay_cannot_renew_the_monotonic_lease(self):
        stamp = int(time.time() * 1000)
        lease = {"status": "online", "lastSeen": stamp, "expiresAt": stamp + 60000, "remainingMs": 2400, "activeCount": 2}
        self.server.llm = lease
        self.run_stage(stateFile=str(self.state_file), fullscreen=False)
        presence = lambda: self.run_stage()["djPresence"]
        self.eventually(lambda: presence()["status"] == "online", "short original lease was not accepted")
        original_deadline = time.monotonic() + presence()["remainingMs"] / 1000
        self.server.llm = {**lease, "activeCount": 1, "remainingMs": 50000}
        self.eventually(lambda: presence()["activeCount"] == 1, "same-expiry metadata update was not read")
        self.assertLessEqual(time.monotonic() + presence()["remainingMs"] / 1000, original_deadline + .15,
                             "same-expiry metadata extended the earlier monotonic deadline")
        offline = {"status": "offline", "lastSeen": stamp, "expiresAt": 0, "remainingMs": 0, "activeCount": 0}
        self.server.llm = offline
        self.eventually(lambda: getattr(self.server, "chat_last_llm_reply", None) == offline and presence()["status"] == "offline",
                        "intervening explicit offline state was not observed")
        replay = {**lease, "activeCount": 3, "remainingMs": 50000}
        self.server.llm = replay
        self.eventually(lambda: getattr(self.server, "chat_last_llm_reply", None) == replay, "same-expiry replay was not delivered")
        time.sleep(.15)
        self.assertEqual(presence()["status"], "offline", "offline reset the monotonic watermark and revived an expired lease")
        self.assertFalse(self.server.playback_requests)

    def test_search_artwork_is_bounded_cached_and_cancelled_with_stale_queries(self):
        base = f"http://127.0.0.1:{self.server.server_port}"
        def track(title, cover):
            return {"uri": "spotify:track:" + title, "title": title, "artist": "Original private fixture band", "artUrl": base + cover}
        self.server.search_tracks = {
            "cover slow": [track("Old result", "/cover/slow.png")],
            "cover current": [track("Current result", "/cover/current.png")],
            "cover large": [track("Oversized result", "/cover/large.png")],
            "cover preview": [track("Original first recording", "/cover/red.png"), track("Original second recording", "/cover/blue.png"),
                              {**track("Explained recording", "/cover/green.png"), "match": {"basis": "semantic", "rationale": "Original fixture explanation", "sources": []}}],
        }
        self.server.covers.update({"/cover/red.png": cover_png((185, 65, 80)), "/cover/blue.png": cover_png((50, 115, 180)),
                                  "/cover/green.png": cover_png((40, 155, 130)), "/cover/large.png": b"x" * (2 * 1024 * 1024 + 1)})
        self.run_stage(stateFile=str(self.state_file), fullscreen=False)
        windows = subprocess.check_output(["xdotool", "search", "--pid", str(self.process.pid), "--name", "LARICS"],
                                          env=self.environment, text=True).splitlines()
        subprocess.run(["xdotool", "windowsize", windows[-1], "2560", "1600"], env=self.environment, check=True)
        bounds = self.magic_bounds()
        self.server.cover_reply_gate = threading.Event(); self.set_text("partyMessage", "cover slow")
        self.eventually(lambda: any(path == "/cover/slow.png" for path, unused in self.server.cover_requests), "old artwork did not begin")
        try:
            self.set_text("partyMessage", "cover current")
            self.eventually(lambda: self.run_stage()["searchArtwork"]["loaded"] == 1, "current result artwork did not load")
        finally:
            self.server.cover_reply_gate.set()
        self.set_text("partyMessage", "Povećaj font u panelu.")
        self.eventually(lambda: self.run_stage()["searchArtwork"]["active"] == 0, "chat intent retained stale image work")
        self.set_text("partyMessage", "cover current")
        self.eventually(lambda: self.run_stage()["searchArtwork"]["loaded"] == 1, "cached result artwork did not return")
        self.assertEqual(sum(path == "/cover/current.png" for path, unused in self.server.cover_requests), 1)
        self.set_text("partyMessage", "cover large")
        self.eventually(lambda: any(path == "/cover/large.png" for path, unused in self.server.cover_requests), "oversized fixture was not requested")
        self.eventually(lambda: self.run_stage()["searchArtwork"]["active"] == 0, "oversized image did not finish rejection")
        self.assertEqual(self.run_stage()["searchArtwork"]["loaded"], 0)
        self.set_text("partyMessage", "cover preview")
        self.eventually(lambda: self.run_stage()["searchArtwork"]["loaded"] == 3, "the preview thumbnails did not load")
        self.assertTrue(all(auth is None for unused, auth in self.server.cover_requests))
        self.assertLessEqual(self.run_stage()["searchArtwork"]["active"], 2)
        self.assertLessEqual(self.run_stage()["searchArtwork"]["cached"], 24)
        self.assertEqual(self.magic_bounds(), bounds)
        self.assertFalse(self.server.requests); self.assertFalse(self.server.posts); self.assertFalse(self.server.playback_requests)
        capture = os.environ.get("PARTY_STAGE_CAPTURE")
        if capture:
            self.run_stage(capture=str(Path(capture).with_name("native269-search.png")))

    def test_initial_invalid_state_uses_only_the_explicit_private_service(self):
        self.state_file.write_text("")
        status = self.run_stage(stateFile=str(self.state_file), fullscreen=False)
        self.assertEqual(status["requestUrl"], f"http://127.0.0.1:{self.server.server_port}")
        self.assertTrue(status["fileError"], "an empty initial file was accepted as valid state")
        self.eventually(lambda: self.run_stage()["chatCount"] == 1, "initial visibility did not use the explicit fixture service")
        self.assertFalse(self.server.posts)
        self.assertFalse(self.server.requests)
        self.write_state()
        self.eventually(lambda: self.run_stage().get("revision") == 1 and not self.run_stage()["fileError"],
                        "the private state did not recover")

    def test_search_intent_is_local_exact_and_invalidated_by_editor_transitions(self):
        self.run_stage(stateFile=str(self.state_file), fullscreen=False)
        phrase = "Molim povećaj font ovog gumba u panelu."
        self.set_text("partyMessage", phrase)
        time.sleep(.45)
        self.assertEqual(self.run_stage()["searchIntent"], "chat")
        self.assertFalse(self.server.searches, "ordinary UI prose reached the catalogue")
        self.assertFalse(self.client.request("object.get", {"objectName": "partySuggestions", "property": "visible"}))
        def cue(intent="music"):
            draft = self.run_stage(getComposerDraft=True)["composerDraft"]
            return {**{key: draft[key] for key in ("guestId", "composerId", "draftRevision", "text", "editorMode")}, "intent": intent}
        current = cue()
        for key, value in (("guestId", "another-guest"), ("composerId", "another-editor"),
                           ("draftRevision", current["draftRevision"] + 1), ("text", phrase + " "), ("editorMode", "dedication")):
            result = self.run_stage(setSearchIntent={**current, key: value})
            self.assertFalse(result["searchIntentResult"]["applied"], f"mismatched {key} was accepted")
        self.assertFalse(self.server.searches)
        self.assertTrue(self.run_stage(setSearchIntent=current)["searchIntentResult"]["applied"])
        self.eventually(lambda: phrase in self.server.searches and self.run_stage()["searchCount"] == 1,
                        "exact music intent did not issue the current query")
        self.assertFalse(self.server.requests); self.assertEqual(self.run_stage()["composerMode"], "message")
        self.assertTrue(self.run_stage(setSearchIntent=cue("chat"))["searchIntentResult"]["applied"])
        self.assertEqual(self.run_stage()["searchCount"], 0)
        self.assertFalse(self.client.request("object.get", {"objectName": "partySuggestions", "property": "visible"}))
        self.set_text("partyMessage", "slow")
        self.eventually(lambda: "slow" in self.server.searches, "concise catalogue lookup was delayed by intent reasoning")
        self.set_text("partyMessage", phrase)
        time.sleep(.8)
        self.assertEqual(self.run_stage()["searchCount"], 0, "late music results replaced a chat decision")
        self.assertFalse(self.run_stage(setSearchIntent=current)["searchIntentResult"]["applied"], "A-B-A editing reused an old revision")
        restored = cue()
        self.run_stage(composerDraft={"text": phrase, "cursor": 8, "anchor": 2})
        self.assertFalse(self.run_stage(setSearchIntent=restored)["searchIntentResult"]["applied"], "restore retained an old intent binding")
        self.key("ctrl+k")
        self.eventually(lambda: self.run_stage()["searchCount"] == 1, "explicit Ctrl+K could not search conversational-looking text")
        selected_binding = cue()
        self.client.request("object.set", {"objectName": "partyResults", "property": "currentRow", "value": 0})
        self.assertEqual(self.run_stage()["composerMode"], "dedication")
        self.assertFalse(self.run_stage(setSearchIntent=selected_binding)["searchIntentResult"]["applied"], "song selection retained a message-mode binding")
        self.set_text("partyMessage", "For the fixture only.")
        before = len(self.server.searches)
        result = self.run_stage(setSearchIntent=cue())
        self.assertEqual(result["searchIntentResult"]["effectiveIntent"], "chat")
        time.sleep(.4); self.assertEqual(len(self.server.searches), before, "dedication text became a catalogue query")
        self.client.request("widget.click", {"objectName": "partyCancelSong"})
        self.assertTrue(self.run_stage(setSearchIntent=cue())["searchIntentResult"]["applied"])
        self.eventually(lambda: self.run_stage()["searchCount"] == 1, "send fixture did not have visible catalogue rows")
        send_binding = cue("chat")
        self.server.message_reply_gate = threading.Event()
        self.key("Return")
        try:
            self.eventually(lambda: bool(self.server.posts), "private chat send did not begin")
            self.assertEqual(self.run_stage()["searchCount"], 0, "sending left obsolete selectable rows above the draft")
            self.assertFalse(self.run_stage(setSearchIntent=send_binding)["searchIntentResult"]["applied"], "send retained an old intent binding")
        finally:
            self.server.message_reply_gate.set()
        self.eventually(lambda: not self.composer_text(), "private chat acknowledgement did not clear its sent text")
        self.set_text("partyMessage", phrase)
        service_binding = cue()
        changed = self.run_stage(requestBaseUrl=f"http://127.0.0.1:{self.server.server_port}/another-base", setSearchIntent=service_binding)
        self.assertFalse(changed["searchIntentResult"]["applied"], "service change retained a prior intent binding")

    def test_search_cancellation_is_exact_and_old_callbacks_cannot_replace_new_work(self):
        self.run_stage(stateFile=str(self.state_file), fullscreen=False)
        old_gate, new_gate, cancel_gate = threading.Event(), threading.Event(), threading.Event()
        self.server.search_gates.update({"old catalogue": old_gate, "new catalogue": new_gate})
        self.server.search_cancel_reply_gate = cancel_gate
        try:
            self.set_text("partyMessage", "old catalogue")
            self.eventually(lambda: bool(self.server.search_attempts), "old catalogue call did not dispatch")
            old_id = self.server.search_attempts[-1][1]
            self.assertRegex(old_id, r"^[a-f0-9-]{36}$")
            self.set_text("partyMessage", "new catalogue")
            self.eventually(lambda: old_id in self.server.search_cancelled and len(self.server.search_attempts) == 2,
                            "editing did not cancel just the dispatched old request")
            new_id = self.server.search_attempts[-1][1]
            self.assertNotEqual(old_id, new_id)
            self.assertEqual(self.server.search_cancels, [{"searchId": old_id}], "cancellation published more than an opaque ID")
            old_gate.set(); time.sleep(.15)
            self.assertEqual(self.run_stage()["suggestionState"], "loading")
            self.assertEqual(self.client.request("object.get", {"objectName": "partySearchActivity", "property": "maximum"}), 0)
            cancel_gate.set(); time.sleep(.15)
            self.assertEqual(self.run_stage()["suggestionState"], "loading", "old cancellation changed the newer spinner")
            new_gate.set()
            self.eventually(lambda: self.run_stage()["searchCount"] == 1, "current search results did not arrive")
            self.assertNotIn(("old catalogue", old_id), self.server.search_admitted, "cancel-before-admission still ran provider work")
            self.assertIn(("new catalogue", new_id), self.server.search_admitted)
            self.assertNotIn(new_id, self.server.search_cancelled)
            self.server.search_reply_ids["mismatched result"] = "another-request-id"
            self.set_text("partyMessage", "mismatched result")
            self.eventually(lambda: self.run_stage()["suggestionState"] == "error", "mismatched response ID was accepted")
            self.assertEqual(self.run_stage()["searchCount"], 0)
            cancelled_gate = threading.Event(); self.server.search_gates["cancelled result"] = cancelled_gate
            self.set_text("partyMessage", "cancelled result")
            self.eventually(lambda: self.server.search_attempts[-1][0] == "cancelled result", "cancelled fixture did not dispatch")
            self.server.search_cancelled.add(self.server.search_attempts[-1][1]); cancelled_gate.set()
            self.eventually(lambda: self.run_stage()["suggestionState"] == "empty", "cancelled response was not quietly discarded")
            release_gate = threading.Event(); self.server.search_gates["release catalogue"] = release_gate
            self.set_text("partyMessage", "release catalogue")
            self.eventually(lambda: self.server.search_attempts[-1][0] == "release catalogue", "release fixture did not dispatch")
            release_id = self.server.search_attempts[-1][1]
            self.run_stage(restore=True)
            self.eventually(lambda: release_id in self.server.search_cancelled, "stage release lost its dispatched cancellation")
            release_gate.set()
        finally:
            for gate in self.server.search_gates.values(): gate.set()
            cancel_gate.set()

    def test_released_generation_composer_rpcs_fail_without_recreating_the_stage(self):
        self.run_stage(stateFile=str(self.state_file), fullscreen=False)
        self.set_text("partyMessage", "Original private draft")
        draft = self.run_stage(getComposerDraft=True)["composerDraft"]
        cue = {**{key: draft[key] for key in ("guestId", "composerId", "draftRevision", "text", "editorMode")}, "intent": "chat"}
        self.run_stage(restore=True)
        for request in ({"getComposerDraft": True}, {"setSearchIntent": cue}):
            operation = self.client.request("snippet.run", {"moduleId": self.module, "executor": "gui", "request": request})
            finished = self.client.wait_for_operation(operation["operationId"], timeout=10)["data"]
            self.assertEqual(finished["outcome"], "failed", finished)
            self.assertIn("inactive", str(finished))
            names = {item.get("objectName") for item in self.client.request("object.list", {})}
            self.assertNotIn("partyStage", names, "a composer RPC recreated the resident released stage")
            self.assertNotIn("partyRequestPanel", names, "a composer RPC recreated its request panel")
        self.assertFalse(self.server.posts); self.assertFalse(self.server.requests); self.assertFalse(self.server.playback_requests)
        self.run_stage(stateFile=str(self.state_file), fullscreen=False)
        restored = self.run_stage(getComposerDraft=True)["composerDraft"]
        self.assertNotEqual(restored["composerId"], draft["composerId"], "explicit installation did not create a fresh editor identity")

    def test_http_search_failure_cancels_the_abandoned_id_without_retry(self):
        self.run_stage(stateFile=str(self.state_file), fullscreen=False)
        self.set_text("partyMessage", "http failure")
        self.eventually(lambda: self.run_stage()["suggestionState"] == "error" and bool(self.server.search_cancels),
                        "HTTP failure discarded the request without cancelling provider work")
        self.assertEqual(len(self.server.search_attempts), 1, "the failed GET was retried")
        identifier = self.server.search_attempts[0][1]
        self.assertEqual(self.server.search_cancels, [{"searchId": identifier}])
        self.assertEqual(self.composer_text(), "http failure")
        self.assertEqual(self.run_stage()["searchCount"], 0)
        self.assertTrue(self.run_stage()["panelNotice"], "the request error presentation was lost")
        self.assertFalse(self.server.posts); self.assertFalse(self.server.requests); self.assertFalse(self.server.playback_requests)

    def test_service_change_ignores_old_chat_presence_and_cancels_at_the_old_base(self):
        old_chat_gate, old_search_gate = threading.Event(), threading.Event()
        self.server.chat_reply_gate = old_chat_gate
        self.server.search_gates["old service catalogue"] = old_search_gate
        stamp = int(time.time() * 1000)
        self.server.llm = {"status": "online", "lastSeen": stamp, "expiresAt": stamp + 120000, "remainingMs": 120000, "activeCount": 1}
        self.run_stage(stateFile=str(self.state_file), fullscreen=False)
        self.eventually(lambda: self.server.chat_requests == 1, "old chat call did not start")
        self.set_text("partyMessage", "old service catalogue")
        self.eventually(lambda: bool(self.server.search_attempts), "old service search did not dispatch")
        old_id = self.server.search_attempts[-1][1]
        other = ThreadingHTTPServer(("127.0.0.1", 0), Service)
        self.assertNotEqual(other.server_port, 8787)
        other.messages = [{"id": 901, "role": "assistant", "name": "DJ", "text": "New private service conversation", "createdAt": stamp}]
        other.withdrawn_ids, other.processing_ids, other.message_work = [], [], {}
        other.llm = {"status": "offline", "lastSeen": stamp, "expiresAt": 0, "remainingMs": 0, "activeCount": 0}
        other.chat_requests = other.chat_finished = 0; other.chat_reply_gate = None
        other.search_cancels, other.search_cancelled, other.search_cancel_reply_gate = [], set(), None
        thread = threading.Thread(target=other.serve_forever, daemon=True); thread.start()
        new_chat_gate = threading.Event()
        try:
            self.request_base_url = f"http://127.0.0.1:{other.server_port}"
            self.run_stage()
            self.eventually(lambda: self.run_stage()["chatCount"] == 1 and self.run_stage()["djPresence"]["status"] == "offline",
                            "a busy old chat prevented the new service response")
            self.assertIn("New private service conversation", self.client.request("object.get", {"objectName": "partyChat", "property": "plainText"}))
            self.assertEqual(self.composer_text(), "old service catalogue")
            self.assertEqual(self.run_stage()["searchCount"], 0)
            self.eventually(lambda: old_id in self.server.search_cancelled, "service change did not cancel at the original base")
            self.assertFalse(other.search_cancels, "old request cancellation was sent to the new service")
            other.chat_reply_gate = new_chat_gate
            self.eventually(lambda: other.chat_requests == 2, "new service incremental poll did not begin")
            old_chat_gate.set(); old_search_gate.set()
            self.eventually(lambda: self.server.chat_finished == 1, "delayed old chat response did not finish")
            time.sleep(1.65)
            self.assertEqual(other.chat_requests, 2, "old callback cleared the newer service's busy flag")
            self.assertEqual(self.run_stage()["chatCount"], 1, "old service messages repopulated the new conversation")
            self.assertEqual(self.run_stage()["djPresence"]["status"], "offline", "old service presence overrode the new lease")
        finally:
            old_chat_gate.set(); old_search_gate.set(); new_chat_gate.set()
            other.shutdown(); other.server_close(); thread.join(3)

    def test_live_state_caption_dwell_requests_chat_and_release(self):
        first = self.run_stage(stateFile=str(self.state_file), fullscreen=True)
        self.assertEqual(first["revision"], 1)
        self.assertEqual(first["trackTitle"], "Atomic Song")
        self.assertTrue(first["panelVisible"], "the guest desk did not open with the stage")
        self.assertEqual(first["djCursorPixels"], 48)
        self.assertEqual(first["composerCursorPixels"], 48)
        self.key("Escape")
        self.assertTrue(self.run_stage()["panelVisible"], "Escape hid the permanent guest desk")
        self.state["caption"] = {"id": "one", "text": "Prvi refren.", "seconds": .1,
                                 "createdAt": int(time.time() * 1000)}
        self.state["revision"] = 2
        self.write_state()
        self.eventually(lambda: self.run_stage()["captionId"] == "one", "new caption was not shown")
        self.assertGreaterEqual(self.run_stage()["captionSeconds"], 4.5)
        self.state["caption"]["id"] = "two"
        self.state["caption"]["text"] = "Drugi refren."
        self.state["revision"] = 3
        self.write_state()
        self.eventually(lambda: self.run_stage()["pendingCaptions"] == 1, "second caption did not queue")
        self.assertEqual(self.run_stage()["captionId"], "one", "new state interrupted the readable first caption")
        self.state["revision"] = 4
        self.write_state()
        self.eventually(lambda: self.run_stage()["revision"] == 4, "atomic replacement was not read")
        self.assertEqual(self.run_stage()["pendingCaptions"], 1, "repeated caption ID was queued twice")
        self.state_file.write_text("{broken")
        self.eventually(lambda: bool(self.run_stage()["fileError"]), "malformed JSON was not reported")
        self.assertEqual(self.run_stage()["revision"], 4)
        self.state["revision"] = 5
        self.state["scene"]["mode"] = "laser"
        self.write_state()
        self.eventually(lambda: self.run_stage()["revision"] == 5, "valid state did not recover")
        self.assertEqual(self.run_stage()["scene"], "laser")

        def checked(name):
            return self.client.request("object.get", {"objectName": name, "property": "checked"})

        self.state["revision"] = 6
        self.state["scene"]["effect"] = 3
        self.write_state()
        self.eventually(lambda: checked("actionOrbitSphere") and checked("actionMobiusRing"), "scene effect did not enable both existing GL modules")
        self.assertFalse(checked("actionWireframe"))
        self.state["revision"] = 7
        self.state["scene"]["effect"] = 6
        self.write_state()
        self.eventually(lambda: not checked("actionOrbitSphere") and checked("actionMobiusRing") and checked("actionWireframe"), "scene handover did not change the real actions")
        self.assertEqual(self.run_stage()["effectError"], "")
        self.eventually(lambda: self.run_stage()["captionId"] == "two", "the second caption never got its full turn")

        self.run_stage(panel=True)
        self.eventually(lambda: self.run_stage()["chatCount"] == 1, "actual DJ chat was not fetched")
        bounds = self.magic_bounds()
        self.assertFalse(self.client.request("object.get", {"objectName": "partySuggestions", "property": "visible"}),
                         "empty suggestions covered the conversation")
        self.set_text("partyName", "Guest test")
        self.set_text("partyMessage", "slow")
        self.eventually(lambda: "slow" in self.server.searches, "slow search did not start")
        self.set_text("partyMessage", "long")
        self.eventually(lambda: self.run_stage()["suggestionState"] == "results", "replacement search did not complete")
        time.sleep(.75)
        self.assertEqual(self.magic_bounds(), bounds, "search results changed the Magic box bounds")
        self.assertTrue(self.client.request("object.get", {"objectName": "partySuggestions", "property": "visible"}),
                        "search results did not appear above the composer")
        popup, chat, composer = (bounds[name] for name in ("partySuggestions", "partyChat", "partyMessage"))
        self.assertGreaterEqual(popup["y"], chat["y"])
        self.assertLess(popup["y"], chat["y"] + chat["height"], "suggestions did not overlay the conversation")
        self.assertLessEqual(popup["y"] + popup["height"], composer["y"], "suggestions covered the composer")
        self.client.request("object.set", {"objectName": "partyResults", "property": "currentRow", "value": 0})
        self.assertTrue(self.run_stage()["selectedTrack"].startswith("A very long song title"), "a stale search replaced current results")
        self.assertEqual(self.run_stage()["composerMode"], "dedication")
        self.assertEqual(self.composer_text(), "")
        self.assertTrue(self.run_stage()["selectedCancelFits"], "the compact song popup clipped Cancel")
        self.assertFalse(self.server.requests, "selecting a song submitted it without consent")
        self.assertEqual(self.magic_bounds(), bounds, "song selection changed the Magic box bounds")
        self.client.request("widget.click", {"objectName": "partyCancelSong"})
        self.assertEqual(self.composer_text(), "long", "cancel did not restore the search draft")
        self.assertEqual(self.run_stage()["composerMode"], "message")

        pages_before = self.client.request("object.get", {"objectName": "partySuggestionPages", "property": "currentIndex"})
        self.set_text("partyMessage", "robot")
        self.assertEqual(self.client.request("object.get", {"objectName": "partySuggestionPages", "property": "currentIndex"}), pages_before,
                         "typing replaced the suggestions page with a loading view")
        self.assertFalse(self.client.request("object.get", {"objectName": "partyResults", "property": "enabled"}), "stale results remained selectable")
        self.eventually(lambda: self.run_stage()["suggestionState"] == "results", "search response was not displayed")
        self.client.request("object.set", {"objectName": "partyResults", "property": "currentRow", "value": 0})
        self.assertEqual(self.run_stage()["composerMode"], "dedication")
        dedication = "Za cijelu ekipu.\nI robote koji još plešu."
        self.set_text("partyMessage", dedication)
        self.server.reject_requests = 1
        self.client.request("widget.click", {"objectName": "partySendButton"})
        self.eventually(lambda: "interrupted" in self.run_stage()["panelNotice"], "failed song request was not reported")
        self.assertEqual(self.composer_text(), dedication, "failed request lost its dedication")
        self.assertEqual(self.run_stage()["composerMode"], "dedication")
        self.assertEqual(self.magic_bounds(), bounds, "request failure changed the Magic box bounds")
        self.client.request("widget.click", {"objectName": "partySendButton"})
        self.eventually(lambda: len(self.server.requests) == 1, "request button did not send a song")
        self.assertEqual(self.server.requests[0]["uri"], "spotify:track:test")
        self.assertEqual(self.server.requests[0]["requestedBy"], "Guest test")
        self.assertEqual(self.server.requests[0]["dedication"], dedication)
        self.assertEqual(self.server.request_attempts[0]["requestId"], self.server.request_attempts[1]["requestId"])
        self.eventually(lambda: self.composer_text() == "" and self.run_stage()["composerMode"] == "message", "successful song send did not reset the composer")

        self.set_text("partyMessage", "offline")
        self.eventually(lambda: self.run_stage()["suggestionState"] == "error", "search failure was not shown")
        self.assertFalse(self.client.request("object.get", {"objectName": "partySuggestions", "property": "visible"}),
                         "failed search left an empty popup covering the conversation")
        self.assertEqual(self.magic_bounds(), bounds, "search failure changed the Magic box bounds")
        prefix = "More groove, please <3"
        self.run_stage(composerDraft={"text": prefix, "cursor": len(prefix), "anchor": len(prefix)})
        self.key("shift+Return")
        self.eventually(lambda: self.composer_text() == prefix + "\n", "Shift+Enter did not insert a newline")
        self.assertFalse(self.server.posts, "Shift+Enter sent a message")
        message = prefix + "\nAnd something Croatian."
        self.run_stage(composerDraft={"text": message, "cursor": len(message), "anchor": len(message)})
        self.server.reject_messages = 1
        self.key("Return")
        self.eventually(lambda: "Message interrupted" in self.run_stage()["panelNotice"], "Enter did not attempt chat submission")
        self.assertEqual(self.composer_text(), message, "failed chat submission lost the multiline draft")
        self.key("Return")
        self.eventually(lambda: len(self.server.posts) == 1, "composer did not send the guest message")
        self.assertEqual(self.server.posts[0]["text"], message, "multiline text was changed during submission")
        self.assertEqual(self.server.posts[0]["source"], "native", "the native chat surface was not declared")
        self.assertEqual(self.server.message_attempts[0]["requestId"], self.server.message_attempts[1]["requestId"])
        self.eventually(lambda: self.composer_text() == "", "successful message did not clear the composer")
        self.assertEqual(self.run_stage()["panelNotice"], "", "successful send left a verbose status message")
        self.assertEqual(self.magic_bounds(), bounds, "chat submission changed the Magic box bounds")
        self.server.messages.append({"id": 3, "role": "assistant", "name": "DJ", "text": "Groove is on its way.", "createdAt": int(time.time() * 1000)})
        self.eventually(lambda: self.run_stage()["chatCount"] == 3, "the real reply did not appear or a message was duplicated")
        self.server.messages[1]["status"] = "read"
        self.eventually(lambda: "✓" in self.client.request("object.get", {"objectName": "partyChat", "property": "plainText"}),
                        "an existing message did not receive its seen check")
        self.assertEqual(self.run_stage()["chatCount"], 3, "receipt refresh duplicated a message")
        capture = os.environ.get("PARTY_STAGE_CAPTURE")
        if capture:
            self.run_stage(capture=capture)
        self.run_stage(panel=False)
        self.assertTrue(self.run_stage()["panelVisible"], "a legacy panel=false request hid the permanent desk")
        self.run_stage(restore=True)
        objects = self.client.request("object.list", {})
        names = {o.get("objectName") for o in objects}
        self.assertNotIn("partyStage", names)
        self.assertNotIn("partyRequestPanel", names)
        self.assertNotIn("partyRequestAction", names)
        self.assertNotIn("partySpotifyAction", names)
        self.assertNotIn("partyReturnAction", names)
        self.assertNotIn("partyPlaybackButton", names)
        self.eventually(lambda: not checked("actionOrbitSphere") and not checked("actionMobiusRing") and not checked("actionWireframe"), "release did not restore the prior scene actions")
        self.assertIn("pid", self.client.request("hello", {}))

    def test_recall_preserves_drafts_and_rejects_accepted_messages(self):
        self.run_stage(stateFile=str(self.state_file), fullscreen=False)
        self.eventually(lambda: self.run_stage()["chatCount"] == 1, "initial chat did not arrive")
        bounds = self.magic_bounds()
        fullscreen = self.client.request("object.get", {"objectName": "mainWindow", "property": "fullScreen"})

        def send(words, count):
            self.run_stage(composerDraft={"text": words, "cursor": len(words), "anchor": len(words)})
            self.key("Return")
            self.eventually(lambda: len(self.server.posts) == count and self.composer_text() == "", "chat send did not complete")
            return self.server.messages[-1]["id"]

        def html():
            return self.client.request("object.get", {"objectName": "partyChat", "property": "html"})

        original = "An original pending message."
        first_id = send(original, 1)
        self.assertIn(f"recall:{first_id}", html(), "pending own message has no clickable recall affordance")
        self.server.messages[-1]["status"] = "read"
        self.server.messages[-1]["readAt"] = int(time.time() * 1000)
        self.eventually(lambda: "✓" in self.client.request("object.get", {"objectName": "partyChat", "property": "plainText"}),
                        "the read-only receipt did not arrive")
        self.assertIn(f"recall:{first_id}", html(), "a read receipt alone removed recall before acceptance")
        saved = {"text": "Keep this dedication.\nAnd its selection.", "cursor": 12, "anchor": 5,
                 "query": "original song", "queryCursor": 9, "queryAnchor": 4,
                 "selectedTrack": {"uri": "spotify:track:saved", "title": "Saved candidate", "artist": "Fixture band"}}
        self.run_stage(composerDraft=saved)
        saved = self.editor_draft()
        self.key("Escape")
        self.eventually(lambda: self.composer_text() == original and first_id in self.server.withdrawn_ids, "Escape did not recall the pending message")
        self.assertEqual(self.run_stage()["composerMode"], "message")
        self.eventually(lambda: self.run_stage()["chatCount"] == 1, "the recalled transcript row remained")
        self.client.request("widget.click", {"objectName": "partyDraftButton"})
        self.assertEqual(self.editor_draft(), saved, "recall lost the prior song/dedication draft or cursor")
        self.assertEqual(self.run_stage()["composerMode"], "dedication")
        self.assertFalse(self.server.requests, "draft restoration enqueued a song")

        second = "The next pending original message."
        second_id = send(second, 2)
        self.run_stage(composerDraft={"text": "Retain the current draft.", "cursor": 10, "anchor": 3})
        before = self.editor_draft()
        self.server.reject_recalls = 1
        self.key("Escape")
        self.eventually(lambda: "interrupted" in self.run_stage()["panelNotice"], "recall failure was not shown")
        self.assertEqual(self.editor_draft(), before, "failed recall overwrote the draft")
        self.server.recall_reply_gate = threading.Event()
        self.key("Escape")
        try:
            self.eventually(lambda: second_id in self.server.withdrawn_ids, "recall was not accepted by the fixture")
            self.run_stage(composerDraft={"text": "Typing while the reply is in flight.", "cursor": 16, "anchor": 6})
            racing = self.editor_draft()
        finally:
            self.server.recall_reply_gate.set()
        self.eventually(lambda: self.client.request("object.get", {"objectName": "partyDraftButton", "property": "text"}) == "Uredi povučenu",
                        "racing recall did not offer the recovered text separately")
        self.assertEqual(self.editor_draft(), racing, "a late recall reply overwrote new typing")
        self.client.request("widget.click", {"objectName": "partyDraftButton"})
        self.assertEqual(self.composer_text(), second)
        self.client.request("widget.click", {"objectName": "partyDraftButton"})
        self.assertEqual(self.editor_draft(), racing)
        self.assertEqual(self.server.recall_attempts[-1], self.server.recall_attempts[-2], "recall retry changed its target")

        third = "The reply is lost after withdrawal."
        third_id = send(third, 3)
        self.run_stage(composerDraft={"text": "Keep this retry draft.", "cursor": 8, "anchor": 3})
        before = self.editor_draft()
        self.server.lose_recall_replies = 1
        self.key("Escape")
        self.eventually(lambda: "reply lost" in self.run_stage()["panelNotice"], "post-withdrawal reply failure was not shown")
        self.eventually(lambda: self.run_stage()["chatCount"] == 1, "withdrawal was not learned through the chat poll")
        self.assertEqual(self.editor_draft(), before)
        self.assertEqual(self.run_stage()["lastSentMessageId"], third_id, "a withdrawal poll discarded the idempotent retry target")
        self.key("Escape")
        self.eventually(lambda: self.composer_text() == third, "retry did not recover the already-withdrawn text")
        self.assertEqual(self.server.recall_attempts[-1], self.server.recall_attempts[-2])

        fourth_id = send("Already accepted by the DJ.", 4)
        self.server.messages[-1]["acceptedAt"] = int(time.time() * 1000)
        self.eventually(lambda: f"recall:{fourth_id}" not in html(), "permanent acceptance left the pending × visible")
        self.run_stage(composerDraft={"text": "This draft stays.", "cursor": 9, "anchor": 2})
        before = self.editor_draft()
        attempts = len(self.server.recall_attempts)
        self.key("Escape")
        self.assertEqual(self.editor_draft(), before)
        self.assertEqual(len(self.server.recall_attempts), attempts, "known accepted message was offered for recall")
        self.assertTrue(self.run_stage()["panelVisible"])
        self.assertEqual(self.client.request("object.get", {"objectName": "mainWindow", "property": "fullScreen"}), fullscreen)
        self.assertEqual(self.magic_bounds(), bounds, "recall state changed panel/composer geometry")
        self.assertLessEqual(self.run_stage()["panelBottomGap"], 12, "an empty status footer still separates Send from the panel bottom")

    def test_description_search_is_explicit_and_stale_results_are_ignored(self):
        self.run_stage(stateFile=str(self.state_file), fullscreen=False)
        windows = subprocess.check_output(["xdotool", "search", "--pid", str(self.process.pid), "--name", "LARICS"],
                                          env=self.environment, text=True).splitlines()
        subprocess.run(["xdotool", "windowsize", windows[-1], "2560", "1600"], env=self.environment, check=True)
        self.eventually(lambda: self.magic_bounds()["partySuggestions"]["height"] == 340, "enlarged suggestions did not fit the native pane")
        bounds = self.magic_bounds()
        description = "ona originalna pjesma o robotu i kiši"
        self.set_text("partyMessage", "ab")
        self.eventually(lambda: "ab" in self.server.searches, "a short exact query did not use direct search")
        self.assertFalse(self.client.request("object.get", {"objectName": "partyDescribeButton", "property": "enabled"}))
        self.set_text("partyMessage", description)
        self.eventually(lambda: description in self.server.searches, "ordinary catalogue search did not run immediately")
        self.assertFalse(self.server.description_posts, "typing started reasoning without the explicit action")
        self.client.request("widget.click", {"objectName": "partyDescribeButton"})
        self.eventually(lambda: len(self.server.description_posts) == 1 and self.run_stage()["descriptionStatus"] == "pending",
                        "explicit descriptive search was not submitted")
        identifier = self.server.description_posts[-1]["requestId"]
        self.assertTrue(self.server.description_posts[-1]["guestId"])
        self.assertIn("DJ", self.client.request("object.get", {"objectName": "partySuggestionHint", "property": "text"}))
        self.assertEqual(self.magic_bounds(), bounds)
        self.server.descriptions[identifier].update(status="resolved", tracks=[
            {"uri": "spotify:track:stale", "title": "Stale resolved candidate", "artist": "Fixture band"}])
        self.server.description_reply_gate = threading.Event()
        try:
            self.eventually(lambda: identifier in self.server.description_polls, "description result was not polled")
            self.set_text("partyMessage", "exact replacement")
            self.eventually(lambda: "exact replacement" in self.server.searches and self.run_stage()["suggestionState"] == "results",
                            "an active reasoning request delayed direct search")
        finally:
            self.server.description_reply_gate.set()
        time.sleep(.25)
        self.client.request("object.set", {"objectName": "partyResults", "property": "currentRow", "value": 0})
        self.assertEqual(self.run_stage()["selectedTrack"], "Song exact replacement", "a stale semantic reply replaced the current query")
        self.assertNotIn("Procjena podudaranja", self.client.request("object.get", {"objectName": "partySelectedTitle", "property": "toolTip"}),
                         "a plain catalogue result acquired an invented confidence")
        self.client.request("widget.click", {"objectName": "partyCancelSong"})
        self.set_text("partyMessage", description)
        self.client.request("widget.click", {"objectName": "partyDescribeButton"})
        self.eventually(lambda: len(self.server.description_posts) == 2, "second explicit query was not submitted")
        identifier = self.server.description_posts[-1]["requestId"]
        match = {"basis": "semantic", "rationale": "A remembered metaphor <not markup> & original wording.", "confidence": .73,
                 "sources": [{"url": "https://example.invalid/reference?x=1&y=2", "label": "Fixture & source"},
                             {"url": "javascript:untrusted()", "label": "Do not show this URL"}]}
        resolved = {"uri": "spotify:track:resolved", "title": "Roboti pod kišobranom", "artist": "Fixture band",
                    "match": match}
        self.server.descriptions[identifier].update(status="resolved", note="Original fixture match.", tracks=[
            resolved])
        self.eventually(lambda: self.run_stage()["descriptionStatus"] == "resolved" and self.run_stage()["searchCount"] == 1,
                        "resolved candidates were not displayed")
        capture = os.environ.get("PARTY_STAGE_CAPTURE")
        if capture:
            self.run_stage(capture=str(Path(capture).with_name("native-ux-search.png")))
        self.assertFalse(self.server.requests)
        self.assertFalse(self.server.posts)
        self.client.request("object.set", {"objectName": "partyResults", "property": "currentRow", "value": 0})
        self.assertEqual(self.run_stage()["selectedTrack"], "Roboti pod kišobranom")
        self.assertEqual(self.run_stage()["composerMode"], "dedication")
        self.assertEqual(self.editor_draft()["selectedTrack"], resolved, "selection dropped the resolver explanation")
        tooltip = self.client.request("object.get", {"objectName": "partySelectedTitle", "property": "toolTip"})
        for expected in ("Po značenju", "&lt;not markup&gt;", "73%", "Fixture &amp; source", "https://example.invalid/reference?x=1&amp;y=2"):
            self.assertIn(expected, tooltip)
        self.assertNotIn("javascript:", tooltip)
        self.assertNotIn("<not markup>", tooltip)
        flat = {key: resolved[key] for key in ("uri", "title", "artist")}
        flat.update(matchBasis=match["basis"], **{key: match[key] for key in ("rationale", "confidence", "sources")})
        selected = self.editor_draft()
        self.run_stage(composerDraft={**selected, "selectedTrack": flat})
        self.assertEqual(self.client.request("object.get", {"objectName": "partySelectedTitle", "property": "toolTip"}), tooltip,
                         "explicit flat match details did not retain the same explanation")
        self.assertFalse(self.server.requests, "a resolved candidate was enqueued without selection and send")
        self.client.request("widget.click", {"objectName": "partyCancelSong"})
        self.assertEqual(self.composer_text(), description)
        self.assertEqual(self.magic_bounds(), bounds)
        if capture:
            self.set_text("partyMessage", "")
            self.eventually(lambda: not self.run_stage()["panelNoticeVisible"], "the transient notice did not dismiss", seconds=7)
            self.assertEqual(self.magic_bounds(), bounds, "dismissing the notice changed panel geometry")
            self.run_stage(capture=str(Path(capture).with_name("native-ux-empty.png")))

    def test_chat_processing_and_smooth_idle_return(self):
        stamp = int(time.time() * 1000)
        self.server.messages = [{"id": i, "role": "user" if i == 30 else "assistant", "name": "Laptop", "status": "read",
                                 "text": f"Original test message {i}.\nThe conversation keeps its place.", "createdAt": stamp + i}
                                for i in range(1, 31)]
        self.server.processing_ids = [30]
        self.run_stage(stateFile=str(self.state_file), fullscreen=False)
        windows = subprocess.check_output(["xdotool", "search", "--pid", str(self.process.pid), "--name", "LARICS"],
                                          env=self.environment, text=True).splitlines()
        subprocess.run(["xdotool", "windowsize", windows[-1], "2560", "1600"], env=self.environment, check=True)

        def scroll(prop):
            return self.client.request("object.get", {"objectName": "partyChatScroll", "property": prop})

        def chat_key(combination):
            subprocess.run(["xdotool", "windowfocus", windows[-1]], env=self.environment, check=True)
            self.client.request("object.invoke", {"objectName": "partyChat", "method": "setFocus"})
            subprocess.run(["xdotool", "key", "--clearmodifiers", combination], env=self.environment, check=True)

        self.eventually(lambda: self.run_stage()["chatCount"] == 30 and scroll("maximum") > 0 and scroll("value") == scroll("maximum"),
                        "initial history did not smoothly reach the bottom")
        self.assertEqual(self.run_stage()["processingCount"], 1)
        self.assertIn("●", self.client.request("object.get", {"objectName": "partyChat", "property": "plainText"}))
        self.assertIn("radi", self.client.request("object.get", {"objectName": "partyDjPresence", "property": "text"}))
        bounds = self.magic_bounds()
        height = scroll("maximum")
        capture = os.environ.get("PARTY_CHAT_CAPTURE")
        if capture:
            self.run_stage(capture=capture)
        self.server.processing_ids = []
        self.eventually(lambda: self.run_stage()["processingCount"] == 0, "explicit completion did not clear the processing dot")
        self.assertNotIn("●", self.client.request("object.get", {"objectName": "partyChat", "property": "plainText"}))
        self.assertEqual(scroll("maximum"), height, "the processing badge changed transcript height")
        self.assertEqual(self.magic_bounds(), bounds)

        old_bottom = scroll("value")
        self.server.messages.append({"id": 31, "role": "assistant", "text": "A new reply.\n" * 5, "createdAt": stamp + 31})
        self.eventually(lambda: old_bottom < scroll("value") < scroll("maximum"), "incoming chat jumped instead of animating")
        chat_key("Home")
        self.eventually(lambda: scroll("value") == 0, "user scrolling did not interrupt following")
        time.sleep(.65)
        self.assertEqual(scroll("value"), 0, "the canceled animation pulled the reader back down")
        self.server.messages.append({"id": 32, "role": "assistant", "text": "This reply waits while you read.", "createdAt": stamp + 32})
        self.eventually(lambda: self.run_stage()["chatCount"] == 32, "the new reply did not arrive")
        idle_started = time.monotonic()
        self.assertEqual(self.run_stage()["chatIdleReturnMs"], 120000)
        self.assertLess(self.run_stage()["chatQuietMs"], 3000, "the agent reply did not restart the quiet timer")
        self.assertEqual(scroll("value"), 0, "an incoming reply moved the reader away from older messages")
        self.eventually(lambda: scroll("value") > 0, "idle chat did not return to the bottom", seconds=123)
        self.assertGreaterEqual(time.monotonic() - idle_started, 119.3, "idle return started before two quiet minutes")
        self.assertLess(scroll("value"), scroll("maximum"), "idle return jumped instead of animating")
        self.eventually(lambda: scroll("value") == scroll("maximum"), "idle animation did not finish", seconds=3)
        self.assertEqual(self.magic_bounds(), bounds)

        old_bottom = scroll("value")
        message = "The guest sends a message.\nKeep this reply at the bottom."
        self.run_stage(composerDraft={"text": message, "cursor": len(message), "anchor": len(message)})
        self.key("Return")
        self.eventually(lambda: len(self.server.posts) == 1, "the guest message was not sent")
        deadline = time.monotonic() + .8
        while time.monotonic() < deadline:
            self.assertGreaterEqual(scroll("value"), old_bottom, "sending a message jumped the transcript upward")
            time.sleep(.04)
        self.eventually(lambda: scroll("value") == scroll("maximum"), "the sent message was not followed to the bottom")
        self.assertEqual(self.composer_text(), "")
        chat_key("Home")
        self.eventually(lambda: scroll("value") == 0, "the reader could not revisit the history")
        self.server.message_reply_gate = threading.Event()
        self.run_stage(composerDraft={"text": "Reply from earlier in the history.", "cursor": 33, "anchor": 33})
        self.key("Return")
        try:
            self.eventually(lambda: self.run_stage()["chatCount"] == 34, "chat polling did not deliver the held POST message")
            self.assertEqual(scroll("value"), 0, "polling moved the reader before their held send completed")
        finally:
            self.server.message_reply_gate.set()
        self.eventually(lambda: len(self.server.posts) == 2 and scroll("value") == scroll("maximum"),
                        "an already-polled successful send did not follow the guest message")
        self.assertEqual(self.magic_bounds(), bounds)

    def test_durable_work_updates_old_rows_without_completing_read_receipts(self):
        self.run_stage(stateFile=str(self.state_file), fullscreen=False)
        self.run_stage(composerDraft={"text": "Private work-state fixture.", "cursor": 27, "anchor": 27})
        self.key("Return")
        self.eventually(lambda: len(self.server.posts) == 1 and self.composer_text() == "", "the private work fixture was not sent")
        identifier = self.server.messages[-1]["id"]
        self.server.messages[-1].update(status="read", readAt=int(time.time() * 1000))
        self.server.processing_ids = [identifier]
        plain = lambda: self.client.request("object.get", {"objectName": "partyChat", "property": "plainText"})
        html = lambda: self.client.request("object.get", {"objectName": "partyChat", "property": "html"})
        self.eventually(lambda: "●" in plain() and "✓" in plain(), "read and legacy processing were not separate marks")
        bounds = self.magic_bounds()
        updated = int(time.time() * 1000)
        def work(state, **fields):
            nonlocal updated
            updated += 1
            record = {"state": state, "effectiveState": state, "summary": "Original private fixture work.",
                      "outcome": "", "evidence": [], "updatedAt": updated, "activeUntil": None}
            record.update(fields)
            self.server.message_work[str(identifier)] = record
            self.eventually(lambda: self.run_stage()["workStates"].get(str(identifier)) == state, f"work did not become {state}")
        work("waiting")
        self.eventually(lambda: "◷" in plain() and "●" not in plain(), "waiting retained the legacy orange dot")
        self.assertIn("✓", plain(), "waiting removed the independent read receipt")
        self.assertNotIn(f"recall:{identifier}", html(), "migrated work with no acceptedAt remained recallable")
        before_attempts = len(self.server.recall_attempts)
        self.run_stage(composerDraft={"text": "Keep this work draft.", "cursor": 8, "anchor": 3})
        draft = self.editor_draft(); self.key("Escape")
        self.assertEqual(self.editor_draft(), draft)
        self.assertEqual(len(self.server.recall_attempts), before_attempts)
        work("blocked", outcome="The private dependency is unavailable.")
        self.eventually(lambda: "◇" in plain() and "◆" not in plain(), "blocked work looked complete")
        work("completed", outcome="The private fixture was checked.", evidence=["original fixture assertion passed"])
        self.eventually(lambda: "◆" in plain() and "✓" in plain(), "completion and read receipt were not distinct")
        work("cancelled", outcome="The private request was withdrawn.")
        self.eventually(lambda: "–" in plain() and "◆" not in plain(), "cancelled work retained completion")
        work("active", activeUntil=int(time.time() * 1000) + 2400)
        self.eventually(lambda: "●" in plain(), "active work did not show its actual processing mark")
        self.eventually(lambda: self.run_stage()["workStates"].get(str(identifier)) == "unconfirmed" and "?" in plain()
                        and "●" not in plain(), "expired work was not shown as unconfirmed", seconds=5)
        self.assertEqual(self.run_stage()["processingCount"], 0, "an expired durable record fell back to the legacy dot")
        self.assertEqual(self.run_stage()["chatCount"], 2, "work updates duplicated an old message")
        self.assertEqual(self.magic_bounds(), bounds)

    def test_artwork_cache_stale_reply_and_four_upcoming_cards(self):
        base = f"http://127.0.0.1:{self.server.server_port}"
        stamp = int(time.time() * 1000)
        (self.folder / "admin-token").write_text("private-fixture-token")
        self.state["spotify"] = {"connected": True, "observedAt": stamp}
        self.state["playbackState"] = {"paused": True, "buffering": False, "device": {"id": "fixture-device"}}
        self.state["nowPlaying"].update(uid="fixture-current", paused=True, playing=False, clockValid=True, artUrl=base + "/cover/slow.png")
        self.state["queue"] = [{"uid": f"next-{i}", "title": f"Original upcoming song {i}", "artist": "Private fixture band", "status": "queued"}
                               for i in range(1, 6)]
        self.server.cover_reply_gate = threading.Event()
        self.write_state(); self.run_stage(stateFile=str(self.state_file), fullscreen=False)
        windows = subprocess.check_output(["xdotool", "search", "--pid", str(self.process.pid), "--name", "LARICS"],
                                          env=self.environment, text=True).splitlines()
        subprocess.run(["xdotool", "windowsize", windows[-1], "2560", "1600"], env=self.environment, check=True)
        self.eventually(lambda: any(path == "/cover/slow.png" for path, unused in self.server.cover_requests), "slow cover was not requested")
        def cover(path):
            self.state["revision"] += 1; self.state["nowPlaying"]["artUrl"] = base + path
            self.write_state()
            self.eventually(lambda: self.run_stage()["artwork"]["url"] == base + path, "new cover identity did not arrive")
        try:
            cover("/cover/current.png")
            self.eventually(lambda: self.run_stage()["artwork"]["loaded"], "current cover did not decode")
        finally:
            self.server.cover_reply_gate.set()
        time.sleep(.2)
        self.assertEqual(self.run_stage()["artwork"]["url"], base + "/cover/current.png", "a late old cover replaced the new one")
        self.eventually(lambda: len(self.run_stage()["queuePreview"]) == 4, "the larger upcoming preview was not painted")
        self.assertEqual([track["uid"] for track in self.run_stage()["queuePreview"]], [f"next-{i}" for i in range(1, 5)])
        rendered = self.run_stage()
        self.assertTrue(all(track["detailFontPixels"] >= 24 and track["bounds"][3] >= 80 for track in rendered["queuePreview"]),
                        "upcoming metadata fell below the shared readable native scale")
        self.assertGreaterEqual(rendered["qrBounds"][2], 220, "the QR remained undersized at the laptop viewport")
        self.assertEqual(rendered["requestCardBounds"][1], rendered["playingCardBounds"][1], "the two request/current headers are vertically misaligned")
        self.assertGreater(rendered["requestCardBounds"][0], rendered["playingCardBounds"][0] + rendered["playingCardBounds"][2],
                           "the enlarged QR card overlaps current-track metadata")
        bounds = self.magic_bounds()
        cover("/cover/other.png"); self.eventually(lambda: self.run_stage()["artwork"]["loaded"], "second cover did not arrive")
        cover("/cover/current.png"); self.eventually(lambda: self.run_stage()["artwork"]["loaded"], "cached cover did not return")
        self.assertEqual(sum(path == "/cover/current.png" for path, unused in self.server.cover_requests), 1, "cache return fetched again")
        for i in range(9):
            cover(f"/cover/cache-{i}.png")
            self.eventually(lambda: self.run_stage()["artwork"]["loaded"], "bounded-cache fixture cover did not load")
        self.assertEqual(self.run_stage()["artwork"]["cacheSize"], 8)
        self.server.covers["/cover/large.png"] = b"x" * (2 * 1024 * 1024 + 1)
        cover("/cover/large.png")
        self.eventually(lambda: bool(self.run_stage()["artwork"]["error"]), "oversized cover was not rejected")
        self.assertFalse(self.run_stage()["artwork"]["loaded"])
        cover("/cover/current.png"); self.eventually(lambda: self.run_stage()["artwork"]["loaded"], "valid cover did not recover")
        self.assertTrue(all(auth is None for unused, auth in self.server.cover_requests), "private admin authentication reached a cover request")
        self.assertFalse(self.server.playback_requests, "loading the UI issued a playback request")
        self.assertEqual(self.magic_bounds(), bounds)
        capture = os.environ.get("PARTY_STAGE_CAPTURE")
        if capture:
            self.run_stage(capture=str(Path(capture).with_name("native-artwork-queue.png")))

    def test_playback_is_explicit_attributed_and_uses_monotonic_canonical_state(self):
        (self.folder / "admin-token").write_text("private-fixture-token")
        stamp = int(time.time() * 1000)
        self.state["spotify"] = {"connected": True, "observedAt": stamp}
        self.state["playbackState"] = {"paused": True, "buffering": False, "device": {"id": "fixture-device"}}
        self.state["nowPlaying"].update(uid="fixture-current", paused=True, playing=False, clockValid=True,
                                        requestedBy="Private guest", dedication="Keep canonical metadata.")
        self.write_state(); self.run_stage(stateFile=str(self.state_file), fullscreen=False)
        control = lambda: self.run_stage()["playbackControl"]
        click = lambda: self.client.request("widget.click", {"objectName": "partyPlaybackButton"})
        self.eventually(lambda: control()["enabled"] and control()["paused"], "fresh paused playback was not available")
        self.assertFalse(self.server.playback_requests)
        self.run_stage(composerDraft={"text": "Keep the exact live intent private.", "cursor": 16, "anchor": 5})
        draft, bounds = self.editor_draft(), self.magic_bounds()
        self.server.playback_reply_gate = threading.Event(); click()
        try:
            self.eventually(lambda: len(self.server.playback_requests) == 1 and control()["busy"], "explicit click did not issue the guarded request")
            self.assertTrue(control()["paused"], "the native view optimistically changed before confirmation")
            self.assertEqual(self.server.playback_requests[0], {"paused": False, "expectedCurrentUid": "fixture-current",
                             "expectedDeviceId": "fixture-device", "source": "native"})
            self.assertEqual(self.server.playback_operators, ["qtra-native"])
            click(); self.assertEqual(len(self.server.playback_requests), 1, "an in-flight click dispatched twice")
        finally:
            self.server.playback_reply_gate.set()
        self.eventually(lambda: control()["enabled"] and not control()["paused"] and self.server.state_reads == 1,
                        "confirmed playback did not refresh canonical state")
        self.assertEqual(self.editor_draft(), draft)
        self.assertNotIn("private-fixture-token", json.dumps(self.run_stage()), "private authentication leaked in native status")

        self.server.playback_error = {"error": "Reply lost after dispatch", "dispatched": True}
        click()
        self.eventually(lambda: control()["enabled"] and control()["paused"] and self.server.state_reads == 2,
                        "uncertain playback did not refresh its actual result")
        self.assertEqual(len(self.server.playback_requests), 2, "uncertain transport was retried automatically")
        self.assertEqual(self.server.playback_commands, [False, True])

        self.server.playback_error = None
        self.server.state_reply_gate = threading.Event(); click()
        try:
            self.eventually(lambda: self.server.state_reads == 3, "canonical refresh did not start")
            self.state = copy.deepcopy(self.server.state_snapshot)
            self.state["revision"] += 1
            self.state["spotify"]["observedAt"] = int(time.time() * 1000) + 1
            self.state["nowPlaying"].update(uid="newer-occurrence", title="Newer canonical observation", requestedBy="New guest")
            self.state["queue"] = [{"uid": "canonical-next", "title": "Canonical queue metadata", "artist": "Fixture band"}]
            self.write_state()
            self.eventually(lambda: self.run_stage()["trackTitle"] == "Newer canonical observation", "newer file observation did not arrive")
        finally:
            self.server.state_reply_gate.set()
        self.eventually(lambda: not control()["busy"], "canonical HTTP refresh did not finish")
        self.assertEqual(self.run_stage()["trackTitle"], "Newer canonical observation", "older HTTP data overwrote a newer file observation")
        self.eventually(lambda: self.run_stage()["queuePreview"] and self.run_stage()["queuePreview"][0]["uid"] == "canonical-next",
                        "raw command queue replaced canonical request metadata")
        self.assertEqual(self.editor_draft(), draft)
        self.assertEqual(self.magic_bounds(), bounds)

        attempts = len(self.server.playback_requests)
        valid = copy.deepcopy(self.state)
        for index, mode in enumerate(("stale", "disconnected", "buffering", "invalid", "uid", "device"), 1):
            self.state = copy.deepcopy(valid); self.state["revision"] += index
            self.state["spotify"]["observedAt"] = int(time.time() * 1000)
            if mode == "stale": self.state["spotify"]["observedAt"] -= 11000
            elif mode == "disconnected": self.state["spotify"]["connected"] = False
            elif mode == "buffering": self.state["nowPlaying"]["buffering"] = True
            elif mode == "invalid": self.state["nowPlaying"]["clockValid"] = False
            elif mode == "uid": self.state["nowPlaying"]["uid"] = ""
            else: self.state["playbackState"]["device"] = {}
            self.write_state()
            self.eventually(lambda: self.run_stage()["revision"] == self.state["revision"] and not control()["enabled"], f"{mode} control stayed enabled")
            click(); self.assertEqual(len(self.server.playback_requests), attempts, f"{mode} click reached transport")

    def test_track_progress_holds_invalid_future_and_stale_actual_clocks(self):
        stamp = int(time.time() * 1000)
        self.state["spotify"] = {"connected": True, "observedAt": stamp}
        self.state["nowPlaying"].update(uid="progress-fixture", clockValid=True, playing=True, paused=False,
                                        buffering=False, progressMs=1000, observedAt=stamp)
        self.write_state(); self.run_stage(stateFile=str(self.state_file), fullscreen=False)
        self.eventually(lambda: self.run_stage()["trackProgressMs"] > 1050, "the valid progress clock did not advance")
        before = self.run_stage()["trackProgressMs"]
        def observe(**fields):
            self.state["nowPlaying"].update(fields); self.state["revision"] += 1
            self.write_state()
            self.eventually(lambda: self.run_stage()["revision"] == self.state["revision"], "the progress observation did not arrive")
        observe(clockValid=False, progressMs=0, observedAt=int(time.time() * 1000) + 60000)
        held = self.run_stage()["trackProgressMs"]
        self.assertGreaterEqual(held, before, "an invalid numeric compatibility field replaced the last accepted position")
        self.assertFalse(self.run_stage()["trackClockValid"])
        time.sleep(.2); self.assertEqual(self.run_stage()["trackProgressMs"], held)
        observe(clockValid=True, progressMs=2000, observedAt=int(time.time() * 1000) - 9800)
        self.eventually(lambda: self.run_stage()["trackProgressMs"] == 12000, "the bar did not stop at the 10-second observation deadline")
        time.sleep(.2); self.assertEqual(self.run_stage()["trackProgressMs"], 12000)
        observe(progressMs=0, observedAt=int(time.time() * 1000) + 60000)
        self.assertEqual(self.run_stage()["trackProgressMs"], 12000, "a future source timestamp moved the held bar")
        observe(playing=False, paused=True, progressMs=0, observedAt=None)
        self.assertEqual(self.run_stage()["trackProgressMs"], 0, "a valid paused zero position was discarded")
        time.sleep(.2); self.assertEqual(self.run_stage()["trackProgressMs"], 0)

    def test_karaoke_clock_note_and_stable_lyric_area(self):
        self.run_stage(stateFile=str(self.state_file), fullscreen=False)
        windows = subprocess.check_output(["xdotool", "search", "--pid", str(self.process.pid), "--name", "LARICS"],
                                          env=self.environment, text=True).splitlines()
        subprocess.run(["xdotool", "windowsize", windows[-1], "2560", "1600"], env=self.environment, check=True)
        first = [{"text": word, "startMs": start, "endMs": end} for word, start, end in (
            ("Ro", 1000, 1400), ("bo", 1400, 1800), ("ti ", 1800, 2200),
            ("pje", 2400, 2800), ("va", 2800, 3200), ("ju.", 3200, 3800))]
        second = [{"text": "Sad je red na vas.", "startMs": 5000, "endMs": 8000}]
        self.state["karaoke"] = {"enabled": True, "title": "Roboti pjevaju", "source": "manual", "status": "paused",
                                 "durationMs": 9000, "positionMs": 1500, "sampledAt": int(time.time() * 1000), "offsetMs": 1234,
                                 "lines": [{"text": "Roboti pjevaju.", "startMs": 1000, "endMs": 3800, "syllables": first},
                                           {"text": "Sad je red na vas.", "startMs": 5000, "endMs": 8000, "syllables": second}]}

        def change(**fields):
            self.state["karaoke"].update(fields)
            self.state["revision"] += 1
            self.write_state()
            self.eventually(lambda: self.run_stage()["revision"] == self.state["revision"], "karaoke state did not arrive")
            self.eventually(lambda: bool(self.run_stage()["karaoke"].get("bounds")), "karaoke was not painted")
            return self.run_stage()["karaoke"]

        frame = change()
        self.assertEqual(frame["text"], "Roboti pjevaju.")
        self.assertEqual(frame["nextText"], "Sad je red na vas.")
        self.assertEqual(frame["syllableIndex"], 1)
        self.assertEqual(frame["positionMs"], 1500, "the native renderer applied offset twice")
        bounds, line_bounds = frame["bounds"], frame["lineBounds"]
        self.assertGreater(bounds[2], 1500, "the karaoke panel did not use the full native stage width")
        first_note = frame["note"]
        time.sleep(.2)
        self.assertEqual(self.run_stage()["karaoke"]["positionMs"], 1500, "paused lyrics continued advancing")
        frame = change(positionMs=2000)
        self.eventually(lambda: self.run_stage()["karaoke"]["note"][0] > first_note[0], "the note did not follow the next syllable")
        self.assertEqual(frame["bounds"], bounds)
        self.assertEqual(frame["lineBounds"], line_bounds, "highlighting moved the lyric area")
        capture = os.environ.get("PARTY_KARAOKE_CAPTURE")
        if capture:
            self.run_stage(capture=capture)
        photo = self.folder / "original-test-photo.ppm"
        photo.write_bytes(b"P6\n16 9\n255\n" + bytes(component for y in range(9) for x in range(16)
                                                    for component in (30 + x * 8, 55 + y * 14, 100)))
        self.state["bookMoment"] = {"imagePath": str(photo), "title": "Originalna probna fotografija", "page": 1}
        self.state["caption"] = {"id": "karaoke-photo", "text": "Fotografija i titl zadržavaju svoja mjesta.",
                                 "seconds": 20, "createdAt": int(time.time() * 1000)}
        frame = change()
        self.assertEqual(frame["bounds"], bounds, "a photo moved the lyric area")
        self.eventually(lambda: self.run_stage()["captionId"] == "karaoke-photo", "the photo caption was not shown")
        self.assertTrue(self.run_stage()["bookLoaded"])
        if capture:
            self.run_stage(capture=str(Path(capture).with_name("karaoke-photo-test.png")))
        frame = change(positionMs=-4000, status="paused")
        self.assertEqual(frame["countInMs"], 5000, "count-in ignored the negative pre-roll")
        frame = change(positionMs=-4000, status="playing", sampledAt=int(time.time() * 1000) - 1000)
        self.assertEqual(frame["positionMs"], 0)
        self.assertGreater(frame["countInMs"], 3500)
        self.assertLessEqual(frame["countInMs"], 4000, "count-in did not advance through pre-roll")
        time.sleep(.25)
        self.assertEqual(self.run_stage()["karaoke"]["positionMs"], 0, "a negative anchor was clamped before interpolation")
        frame = change(positionMs=1400, status="playing", sampledAt=int(time.time() * 1000))
        self.eventually(lambda: self.run_stage()["karaoke"]["syllableIndex"] >= 2, "playing lyrics did not interpolate between state polls")
        frame = change(positionMs=2300, status="paused")
        self.assertEqual(frame["phase"], "gap")
        self.assertEqual(frame["syllableIndex"], 2, "the note did not hold through the syllable gap")
        frame = change(positionMs=5200)
        self.assertEqual(frame["lineIndex"], 1, "seeking did not choose the right lyric line")
        self.assertEqual(frame["bounds"], bounds, "the next lyric changed the panel bounds")
        stamp = int(time.time() * 1000)
        self.state["nowPlaying"].update(clockValid=True, paused=False, buffering=False, playing=True)
        frame = change(source="spotify", positionMs=1000, status="playing", sampledAt=stamp,
                       clockValidUntil=stamp + 350, clockState="running")
        self.eventually(lambda: self.run_stage()["karaoke"]["status"] == "held", "lyrics did not hold at the source deadline")
        held = self.run_stage()["karaoke"]
        self.assertEqual(held["positionMs"], 1350, "deadline capping changed the signed song clock")
        self.assertEqual(held["clockState"], "stale")
        time.sleep(.2)
        self.assertEqual(self.run_stage()["karaoke"]["positionMs"], 1350, "stale lyrics kept advancing")
        self.state["nowPlaying"]["clockValid"] = False
        frame = change(positionMs=1800, status="playing", sampledAt=int(time.time() * 1000) - 1000,
                       clockValidUntil=int(time.time() * 1000) + 9000, clockState="running")
        self.assertEqual(frame["positionMs"], 1800, "invalid raw clock was interpolated")
        self.assertEqual(frame["status"], "held")
        self.state["nowPlaying"].update(clockValid=True, paused=True)
        frame = change()
        self.assertEqual(frame["positionMs"], 1800, "explicit player pause was ignored")
        self.state["nowPlaying"].update(paused=False, buffering=True)
        frame = change()
        self.assertEqual(frame["positionMs"], 1800, "buffering advanced lyrics")
        self.state["nowPlaying"]["buffering"] = False
        for anchor in (True, None, int(time.time() * 1000) + 20000):
            frame = change(sampledAt=anchor)
            self.assertEqual(frame["positionMs"], 1800, "invalid/future anchor was used")
            self.assertEqual(frame["status"], "held")
        frame = change(positionMs=0, status="paused", sampledAt=int(time.time() * 1000))
        self.assertEqual(frame["positionMs"], 0, "valid zero was replaced by an old position")
        frame = change(source="manual", positionMs=0, status="finished")
        self.assertEqual(frame["positionMs"], 9000)
        self.assertEqual(frame["phase"], "finished")
        frame = change(positionMs=0, status="waiting")
        self.assertEqual(frame["phase"], "waiting")
        self.state["karaoke"] = None
        self.state["revision"] += 1
        self.write_state()
        self.eventually(lambda: not self.run_stage()["karaoke"]["active"], "stopped karaoke remained active")
        self.assertTrue(self.run_stage()["panelVisible"], "karaoke changed the guest panel")


if __name__ == "__main__":
    unittest.main()
