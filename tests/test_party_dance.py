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
        for index, (title, theme) in enumerate([
                ("Volim piti", "bottles"), ("Koktel ljubavi", "cocktail"),
                ("Gdje Sam Bio", "compass"), ("Podigla Me Iz Pepela", "embers"),
                ("Maslačak", "dandelion")]):
            self.stage.state["nowPlaying"].update(uid=f"song-{index}", uri=f"spotify:track:fixture{index}", title=title)
            self.stage.write_state()
            self.stage.eventually(lambda: self.run_dance()["theme"] == theme, f"theme did not follow {title}")
            if capture and theme in ("bottles", "dandelion"):
                self.stage.run_stage(capture=str(Path(capture).with_suffix(f".{theme}.png")))
        song = self.run_dance()
        self.stage.state["nowPlaying"]["progressMs"] += 1000
        self.stage.state["queue"] = [{"title": "Volim piti", "uid": "queued-only"}]
        self.stage.write_state()
        time.sleep(.6)
        self.assertEqual(self.run_dance()["songRevision"], song["songRevision"], "progress/queue changed the current song artwork")
        for occurrence in ("unmatched-one", "unmatched-two"):
            revision = self.run_dance()["songRevision"]
            self.stage.state["nowPlaying"].update(uid=occurrence, title="An original fixture tune")
            self.stage.write_state()
            self.stage.eventually(lambda: self.run_dance()["songRevision"] > revision, "new occurrence did not update the artwork")
            current = self.run_dance()
            if occurrence == "unmatched-one": first_default = current["theme"]
            else: self.assertNotEqual(current["theme"], first_default, "default musical motif did not change")
        self.stage.state["scene"]["audioActStartedAt"] += 600000
        self.stage.write_state()
        self.stage.eventually(lambda: self.run_dance()["act"] == "streamers", "scheduled act did not change")
        self.beats.disconnect()
        self.assertTrue(self.beats.subscribed.wait(3), "beat stream did not reconnect")
        self.beats.beat(8)
        self.stage.eventually(lambda: self.run_dance()["beats"] == 8, "beat stream did not recover")
        if capture:
            self.stage.run_stage(capture=str(Path(capture).with_suffix(".streamers.png")))
        self.stage.state["scene"]["audioActStartedAt"] += 600000
        self.stage.write_state()
        self.stage.eventually(lambda: self.run_dance()["act"] == "conga", "third act did not start")
        if capture:
            self.stage.run_stage(capture=str(Path(capture).with_suffix(".conga.png")))
        self.stage.state["scene"]["audioActStartedAt"] += 600000
        self.stage.write_state()
        self.stage.eventually(lambda: self.run_dance()["act"] == "stadium", "fourth act did not start")
        if capture:
            self.stage.run_stage(capture=str(Path(capture).with_suffix(".stadium.png")))
        after = self.stage.run_stage(getComposerDraft=True)
        self.assertEqual(after["composerDraft"], original["composerDraft"])
        self.assertEqual(self.stage.magic_bounds(), original_bounds)
        self.assertTrue(self.client.request("object.get", {"objectName": "partyMessage", "property": "focus"}))
        self.assertTrue(self.run_dance(restore=True)["removed"])
        with self.assertRaises(ProtocolError):
            self.client.request("object.get", {"objectName": "partyDance", "property": "width"})
        self.assertEqual(self.stage.run_stage(getComposerDraft=True)["composerDraft"], original["composerDraft"])
        reloaded = self.run_dance(stateFile=str(self.stage.state_file), agentSocket=self.beats.path)
        self.assertEqual(reloaded["act"], "stadium")
        self.assertEqual(reloaded["songRevision"], current["songRevision"])
        self.assertEqual(reloaded["theme"], current["theme"])
        self.stage.state["scene"]["audioActStartedAt"] += 600000
        self.stage.write_state()
        self.stage.eventually(lambda: self.run_dance()["act"] == "disco", "four-act cycle did not wrap")
        self.run_dance(restore=True)

    def test_song_visual_cue_tracks_occurrence_and_recovers_without_reset(self):
        self.stage.state["nowPlaying"].update(uid="current", uri="spotify:track:original", title="An original fixture tune")
        self.stage.write_state()
        original = self.run_dance(stateFile=str(self.stage.state_file), agentSocket=self.beats.path)
        self.assertEqual(original["themeSource"], "automatic")
        self.assertEqual(original["theme"], original["automaticTheme"])
        identity = {key: original[key] for key in ("songKey", "songRevision", "act")}

        def expect(theme, source, identity_expected=identity):
            self.stage.eventually(lambda: self.run_dance()["theme"] == theme and self.run_dance()["themeSource"] == source,
                                  f"expected {source} artwork {theme}")
            current = self.run_dance()
            self.assertEqual({key: current[key] for key in identity_expected}, identity_expected)
            return current

        self.stage.state["scene"]["songVisual"] = {"trackUid": "current", "theme": "cocktail"}
        self.stage.write_state()
        cue = expect("cocktail", "cue")
        self.assertEqual(cue["automaticTheme"], original["automaticTheme"])
        self.stage.state["nowPlaying"]["progressMs"] += 1000
        self.stage.state["queue"] = [{"uid": "next", "title": "Another original tune"}]
        self.stage.write_state()
        time.sleep(.6)
        expect("cocktail", "cue")

        self.stage.state["scene"]["songVisual"]["theme"] = "compass"
        self.stage.write_state()
        expect("compass", "cue")
        self.stage.state_file.unlink()
        self.stage.eventually(lambda: bool(self.run_dance()["stateError"]), "missing state was not observed")
        expect("compass", "cue")
        self.stage.write_state()
        self.stage.eventually(lambda: not self.run_dance()["stateError"], "identical restored state did not clear its error")
        expect("compass", "cue")
        self.stage.state_file.write_text("{broken")
        self.stage.eventually(lambda: bool(self.run_dance()["stateError"]), "invalid state was not observed")
        self.run_dance(restore=True)
        reloaded = self.run_dance(stateFile=str(self.stage.state_file), agentSocket=self.beats.path)
        self.assertTrue(reloaded["stateError"])
        expect("compass", "cue")
        self.stage.write_state()
        self.stage.eventually(lambda: not self.run_dance()["stateError"], "state did not recover")

        current_track = self.stage.state["nowPlaying"]
        self.stage.state["nowPlaying"] = None
        self.stage.write_state()
        expect(original["automaticTheme"], "automatic")
        self.run_dance(restore=True)
        self.run_dance(stateFile=str(self.stage.state_file), agentSocket=self.beats.path)
        expect(original["automaticTheme"], "automatic")
        self.stage.state["nowPlaying"] = current_track
        self.stage.write_state()
        expect("compass", "cue")

        for cue in (None, {"trackUid": "current", "theme": "unknown"}, {"trackUid": "next", "theme": "dandelion"}):
            self.stage.state["scene"]["songVisual"] = cue
            self.stage.write_state()
            time.sleep(.55)
            expect(original["automaticTheme"], "automatic")
        self.stage.state["nowPlaying"].update(uid="next", uri="spotify:track:next")
        self.stage.write_state()
        self.stage.eventually(lambda: self.run_dance()["songRevision"] == original["songRevision"] + 1,
                              "new occurrence did not advance identity")
        current = self.run_dance()
        next_identity = {key: current[key] for key in ("songKey", "songRevision", "act")}
        expect("dandelion", "cue", next_identity)
        self.stage.state["scene"].pop("songVisual")
        self.stage.write_state()
        expect(current["automaticTheme"], "automatic", next_identity)
        self.run_dance(restore=True)


if __name__ == "__main__":
    unittest.main()
