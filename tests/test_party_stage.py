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
            self.server.searches.append(parse_qs(url.query)["q"][0])
            return self.reply({"tracks": [{"id": "test", "uri": "spotify:track:test",
                                           "title": "Robot karaoke", "artist": "LARICS house band",
                                           "album": "After hours", "durationMs": 180000}]})
        if url.path == "/api/chat":
            after = int(parse_qs(url.query).get("after", [0])[0])
            return self.reply({"messages": [m for m in self.server.messages if m["id"] > after],
                               "dj": {"status": "listening", "lastSeen": int(time.time() * 1000)}})
        self.send_error(404)

    def do_POST(self):
        data = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if self.path == "/api/request":
            self.server.requests.append(data)
            return self.reply({"status": "queued"})
        if self.path == "/api/message":
            self.server.posts.append(data)
            message = {"id": len(self.server.messages) + 1, "role": "user",
                       "name": data["name"], "text": data["text"], "createdAt": int(time.time() * 1000)}
            self.server.messages.append(message)
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
        self.client.request("object.set", {"objectName": object_name, "property": "text", "value": value})

    def test_live_state_caption_dwell_requests_chat_and_release(self):
        first = self.run_stage(stateFile=str(self.state_file), fullscreen=True)
        self.assertEqual(first["revision"], 1)
        self.assertEqual(first["trackTitle"], "Atomic Song")
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

        self.run_stage(panel=True)
        self.eventually(lambda: self.run_stage()["chatCount"] == 1, "actual DJ chat was not fetched")
        self.set_text("partyName", "Guest test")
        self.set_text("partySearch", "robot")
        self.eventually(lambda: self.run_stage()["searchCount"] == 1, "search response was not displayed")
        self.assertEqual(self.server.searches, ["robot"])
        self.client.request("widget.click", {"objectName": "partyRequestButton"})
        self.eventually(lambda: len(self.server.requests) == 1, "request button did not send a song")
        self.assertEqual(self.server.requests[0]["uri"], "spotify:track:test")
        self.assertEqual(self.server.requests[0]["requestedBy"], "Guest test")
        self.assertTrue(self.server.requests[0]["requestId"])
        self.set_text("partyMessage", "More groove, please <3")
        self.client.request("widget.click", {"objectName": "partySendButton"})
        self.eventually(lambda: len(self.server.posts) == 1, "composer did not send the guest message")
        self.server.messages.append({"id": 3, "role": "assistant", "name": "DJ", "text": "Groove is on its way.", "createdAt": int(time.time() * 1000)})
        self.eventually(lambda: self.run_stage()["chatCount"] == 3, "the real reply did not appear or a message was duplicated")
        self.eventually(lambda: self.run_stage()["captionId"] == "two", "the second caption never got its full turn")
        capture = os.environ.get("PARTY_STAGE_CAPTURE")
        if capture:
            self.run_stage(capture=capture)
        self.run_stage(panel=False)
        self.assertFalse(self.run_stage()["panelVisible"])
        self.run_stage(restore=True)
        objects = self.client.request("object.list", {})
        names = {o.get("objectName") for o in objects}
        self.assertNotIn("partyStage", names)
        self.assertNotIn("partyRequestPanel", names)
        self.assertNotIn("partyRequestAction", names)
        self.eventually(lambda: not checked("actionOrbitSphere") and not checked("actionMobiusRing") and not checked("actionWireframe"), "release did not restore the prior scene actions")
        self.assertIn("pid", self.client.request("hello", {}))


if __name__ == "__main__":
    unittest.main()
