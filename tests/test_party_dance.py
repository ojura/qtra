"""Exercise the separate visual layer in an owned display, with synthetic beats."""
from __future__ import annotations

import json
import os
from pathlib import Path
import socket
import subprocess
import threading
import time
import unittest

import test_party_stage as fixture
from agentctl import ProtocolError


class BeatStream:
    def __init__(self, path):
        self.path = str(path)
        self.server = socket.socket(socket.AF_UNIX)
        self.server.bind(self.path)
        self.server.listen(1)
        self.server.settimeout(.1)
        self.stop_event = threading.Event()
        self.subscribed = threading.Event()
        self.client = None
        self.subscriptions = []
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        while not self.stop_event.is_set():
            try:
                client, _ = self.server.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            self.client = client
            client.settimeout(.1)
            buffer = b""
            while not self.stop_event.is_set():
                try:
                    chunk = client.recv(4096)
                    if not chunk:
                        break
                    buffer += chunk
                    if b"\n" in buffer:
                        self.subscriptions.append(json.loads(buffer.split(b"\n", 1)[0]))
                        self.subscribed.set()
                        buffer = b""
                except socket.timeout:
                    continue
                except OSError:
                    break
            client.close()

    def beat(self, count):
        self.client.sendall((json.dumps({"event": "audio.beat", "data": {
            "count": count, "strength": 1.1, "bpm": 128, "bass": .8, "centroid": .3}}) + "\n").encode())

    def disconnect(self):
        self.subscribed.clear()
        self.client.shutdown(socket.SHUT_RDWR)

    def stop(self):
        self.stop_event.set()
        if self.client:
            try:
                self.client.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self.server.close()
        self.thread.join(2)


@unittest.skipUnless(fixture.can_draw(), "a private display is required")
class PartyDance(unittest.TestCase):
    def setUp(self):
        self.stage = fixture.PartyStage()
        self.addCleanup(self.stage.doCleanups)
        self.stage.setUp()
        self.client = self.stage.client
        self.stage.state["scene"]["audioActStartedAt"] = int(time.time() * 1000)
        self.stage.write_state()
        self.stage.run_stage(stateFile=str(self.stage.state_file))
        windows = subprocess.check_output(["xdotool", "search", "--pid", str(self.stage.process.pid),
                                           "--name", "LARICS"], env=self.stage.environment, text=True).splitlines()
        subprocess.run(["xdotool", "windowsize", "--sync", windows[-1], "2560", "1600"],
                       env=self.stage.environment, check=True)
        subprocess.run(["xdotool", "windowfocus", windows[-1]], env=self.stage.environment, check=True)
        self.beats = BeatStream(self.stage.folder / "beats.sock")
        self.addCleanup(self.beats.stop)
        self.module = self.client.request("snippet.load", {
            "path": str(fixture.BUILD / "agent_snippet_party_dance.so")})["moduleId"]

    def run_dance(self, **request):
        operation = self.client.request("snippet.run", {"moduleId": self.module, "executor": "gui", "request": request})
        result = self.client.wait_for_operation(operation["operationId"], timeout=10)["data"]
        self.assertEqual(result["outcome"], "completed", result)
        return result["result"]

    def test_beat_animation_act_change_and_release_preserve_desk(self):
        self.stage.set_text("partyMessage", "Dino, pa još Dino.\nI ples!")
        self.client.request("object.invoke", {"objectName": "partyMessage", "method": "setFocus"})
        self.assertTrue(self.client.request("object.get", {"objectName": "partyMessage", "property": "focus"}))
        original = self.stage.run_stage(getComposerDraft=True)
        original_bounds = self.stage.magic_bounds()
        first = self.run_dance(stateFile=str(self.stage.state_file), agentSocket=self.beats.path)
        self.assertEqual(first["act"], "disco")
        self.assertTrue(self.beats.subscribed.wait(3), "beat stream did not subscribe")
        self.assertEqual(self.beats.subscriptions[0]["params"]["prefixes"], ["audio.beat"])
        self.beats.beat(7)
        self.stage.eventually(lambda: self.run_dance()["beats"] == 7, "beat did not reach the layer")
        active = self.run_dance()
        self.assertGreater(active["pulse"], 0)
        self.assertEqual(active["bpm"], 128)
        self.assertEqual(active["width"], active["parentWidth"])
        self.assertLess(active["width"], 2560)
        time.sleep(.18)  # No status call may supply the animation's repaint.
        self.assertGreater(self.run_dance()["frames"], active["frames"] + 2, "animation timer stopped")
        capture = os.environ.get("PARTY_DANCE_CAPTURE")
        if capture:
            self.stage.run_stage(capture=str(Path(capture).with_suffix(".disco.png")))
        self.stage.state["scene"]["audioActStartedAt"] += 600000
        self.stage.write_state()
        self.stage.eventually(lambda: self.run_dance()["act"] == "streamers", "scheduled act did not change")
        self.beats.disconnect()
        self.assertTrue(self.beats.subscribed.wait(3), "beat stream did not reconnect")
        self.beats.beat(8)
        self.stage.eventually(lambda: self.run_dance()["beats"] == 8, "beat stream did not recover")
        if capture:
            self.stage.run_stage(capture=str(Path(capture).with_suffix(".streamers.png")))
        after = self.stage.run_stage(getComposerDraft=True)
        self.assertEqual(after["composerDraft"], original["composerDraft"])
        self.assertEqual(self.stage.magic_bounds(), original_bounds)
        self.assertTrue(self.client.request("object.get", {"objectName": "partyMessage", "property": "focus"}))
        self.assertTrue(self.run_dance(restore=True)["removed"])
        with self.assertRaises(ProtocolError):
            self.client.request("object.get", {"objectName": "partyDance", "property": "width"})
        self.assertEqual(self.stage.run_stage(getComposerDraft=True)["composerDraft"], original["composerDraft"])
        reloaded = self.run_dance(stateFile=str(self.stage.state_file), agentSocket=self.beats.path)
        self.assertEqual(reloaded["act"], "streamers")
        self.run_dance(restore=True)


if __name__ == "__main__":
    unittest.main()
