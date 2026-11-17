"""The party stage follows atomic state updates and sends real widget requests.

Uses a private X display, a private qtra process, and a local HTTP fixture. No
Spotify account, desktop window, or party state file is changed by this test.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from agentctl import AgentClient
from private_display import OwnDisplay, can_draw, child_environment

BUILD = Path(os.environ.get("RUNTIME_AGENT_TEST_BUILD_DIR", ROOT / "build" / "release"))


class Service(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, value):
        body = json.dumps(value).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urlsplit(self.path)
        if url.path == "/api/search":
            query = parse_qs(url.query)["q"][0]
            self.server.searches.append(query)
            if query == "offline":
                return self.reply({"error": "Search unavailable"})
            if query == "slow":
                time.sleep(.7)
            title = "Robot karaoke" if query == "robot" else f"Song {query}"
            if query == "long":
                title = "A very long song title " * 25
            return self.reply({"tracks": [{"id": "test", "uri": "spotify:track:test",
                                           "title": title, "artist": "LARICS house band",
                                           "album": "After hours", "durationMs": 180000}]})
        if url.path == "/api/chat":
            after = int(parse_qs(url.query).get("after", [0])[0])
            return self.reply({"messages": [m for m in self.server.messages if m["id"] > after],
                               "dj": {"status": "listening", "lastSeen": int(time.time() * 1000),
                                      "processingMessageIds": self.server.processing_ids}})
        self.send_error(404)

    def do_POST(self):
        data = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
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
                       "name": data["name"], "text": data["text"], "createdAt": int(time.time() * 1000)}
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
        environment = child_environment(self.display)
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
        self.server.searches, self.server.requests, self.server.posts = [], [], []
        self.server.request_attempts, self.server.message_attempts = [], []
        self.server.reject_requests = self.server.reject_messages = 0
        self.server.message_reply_gate = None
        self.server.processing_ids = []
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
        staged = self.state_file.with_suffix(".new")
        staged.write_text(json.dumps(self.state))
        staged.replace(self.state_file)

    def run_stage(self, **request):
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

    def test_live_state_caption_dwell_requests_chat_and_release(self):
        first = self.run_stage(stateFile=str(self.state_file), fullscreen=True)
        self.assertEqual(first["revision"], 1)
        self.assertEqual(first["trackTitle"], "Atomic Song")
        self.assertTrue(first["panelVisible"], "the guest desk did not open with the stage")
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
        self.eventually(lambda: not checked("actionOrbitSphere") and not checked("actionMobiusRing") and not checked("actionWireframe"), "release did not restore the prior scene actions")
        self.assertIn("pid", self.client.request("hello", {}))

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
        idle_started = time.monotonic()
        self.eventually(lambda: scroll("value") == 0, "user scrolling did not interrupt following")
        time.sleep(.65)
        self.assertEqual(scroll("value"), 0, "the canceled animation pulled the reader back down")
        self.server.messages.append({"id": 32, "role": "assistant", "text": "This reply waits while you read.", "createdAt": stamp + 32})
        self.eventually(lambda: self.run_stage()["chatCount"] == 32, "the new reply did not arrive")
        self.assertEqual(scroll("value"), 0, "an incoming reply moved the reader away from older messages")
        self.eventually(lambda: scroll("value") > 0, "idle chat did not return to the bottom", seconds=23)
        self.assertGreaterEqual(time.monotonic() - idle_started, 19.3, "idle return started before 20 seconds without interaction")
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
        frame = change(positionMs=0, status="finished")
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
