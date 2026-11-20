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
        # No status call may supply the animation's repaint. The private display
        # renders the multisampled scene in software, so allow half a second.
        time.sleep(.5)
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
        time.sleep(.55)
        self.assertEqual(self.run_dance()["act"], "disco", "a background anchor still controls the independent form cycle")
        self.run_dance(act="streamers")
        self.stage.eventually(lambda: self.run_dance()["act"] == "streamers", "scheduled act did not change")
        self.beats.disconnect()
        self.assertTrue(self.beats.subscribed.wait(3), "beat stream did not reconnect")
        self.beats.beat(8)
        self.stage.eventually(lambda: self.run_dance()["beats"] == 8, "beat stream did not recover")
        if capture:
            self.stage.run_stage(capture=str(Path(capture).with_suffix(".streamers.png")))
        self.run_dance(act="conga")
        self.stage.eventually(lambda: self.run_dance()["act"] == "conga", "third act did not start")
        if capture:
            self.stage.run_stage(capture=str(Path(capture).with_suffix(".conga.png")))
        self.run_dance(act="stadium")
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
        resumed = self.run_dance(act="auto", startAct="disco")
        self.assertEqual(resumed["act"], "disco")
        self.assertTrue(resumed["autoActs"])
        self.run_dance(restore=True)

    def test_kolo_and_tram_rotate_independently_and_preserve_desk(self):
        self.stage.set_text("partyMessage", "Kolo i tramvaj, molim.")
        self.client.request("object.invoke", {"objectName": "partyMessage", "method": "setFocus"})
        draft = self.stage.run_stage(getComposerDraft=True)["composerDraft"]
        bounds = self.stage.magic_bounds()
        first = self.run_dance(stateFile=str(self.stage.state_file), agentSocket=self.beats.path, act="kolo")
        self.assertEqual(first["formIntervalMs"], 60000)
        self.assertEqual(first["formation"], "closed-ring")
        self.assertEqual(first["availableActs"], ["disco", "streamers", "conga", "stadium", "kolo", "tram"])
        self.stage.eventually(lambda: self.run_dance()["frames"] > 3, "the new circle did not render")
        capture = os.environ.get("PARTY_DANCE_CAPTURE")
        if capture:
            self.stage.run_stage(capture=str(Path(capture).with_suffix(".kolo.png")))
        before = self.run_dance()["frames"]
        tram = self.run_dance(act="tram")
        self.assertEqual(tram["formation"], "tram-passengers")
        self.stage.eventually(lambda: self.run_dance()["frames"] > before + 3, "the tram did not render")
        if capture:
            self.stage.run_stage(capture=str(Path(capture).with_suffix(".tram.png")))
        self.run_dance(act="auto", startAct="kolo", formSeconds=1)
        self.stage.state_file.write_text("{broken")
        self.stage.eventually(lambda: self.run_dance()["act"] == "tram", "form rotation depended on state-file updates")
        self.assertTrue(self.run_dance()["stateError"])
        scheduled = self.run_dance(formSeconds=2)
        self.run_dance(restore=True)
        reloaded = self.run_dance(stateFile=str(self.stage.state_file), agentSocket=self.beats.path)
        self.assertEqual(reloaded["act"], "tram")
        self.assertEqual(reloaded["nextActAt"], scheduled["nextActAt"], "reload restarted the remaining form interval")
        self.assertEqual(reloaded["formIntervalMs"], 2000)
        self.stage.write_state()
        self.stage.eventually(lambda: self.run_dance()["act"] == "disco", "the six-form cycle did not wrap")
        self.run_dance(act="kolo", formSeconds=1)
        time.sleep(1.15)
        held = self.run_dance()
        self.assertEqual(held["act"], "kolo", "automatic rotation overrode a manual form choice")
        self.assertFalse(held["autoActs"])
        self.assertEqual(held["nextActAt"], 0)
        self.assertEqual(self.stage.run_stage(getComposerDraft=True)["composerDraft"], draft)
        self.assertEqual(self.stage.magic_bounds(), bounds)
        self.assertTrue(self.client.request("object.get", {"objectName": "partyMessage", "property": "focus"}))
        self.assertFalse(self.stage.server.playback_requests or self.stage.server.skip_requests or self.stage.server.requests
                         or self.stage.server.message_attempts, "a visual form issued a playback or guest action")
        self.run_dance(restore=True)

    def test_acts_place_the_fleet_on_their_marks(self):
        self.run_dance(stateFile=str(self.stage.state_file), agentSocket=self.beats.path, act="tram")

        def actors():
            return {a["name"]: a for a in self.stage.run_stage()["sceneDepth"]["actors"]}

        def bottom(actor):
            return actor["bounds"][1] + actor["bounds"][3]

        self.stage.eventually(lambda: {"dance.tram.body", "fleet-VIV"} <= actors().keys(), "the tram act did not render")
        scene = actors()
        tram = scene["dance.tram.body"]
        for walker in ("fleet-VIV", "fleet-Navines"):
            # Walkers wait on the platform: nearer than the tram and below its wheels.
            self.assertLess(scene[walker]["depth"], tram["depth"], walker)
            self.assertGreater(bottom(scene[walker]), bottom(tram), walker)
        # The flyer stays above the roof, in front of the overhead wire.
        drone = scene["fleet-EagleRed"]
        self.assertLess(drone["depth"], tram["depth"])
        self.assertLessEqual(bottom(drone), tram["bounds"][1] + 4)
        for name in ("EagleRed", "VIV", "Navines"):
            self.assertIn(f"fleet-{name}-tag", scene)

        self.run_dance(act="kolo")
        self.stage.eventually(lambda: any(n.startswith("dance.kolo.dancer.") for n in actors()), "the kolo did not render")
        scene = actors()
        dancers = [a for n, a in scene.items() if n.startswith("dance.kolo.dancer.")]
        left = min(a["bounds"][0] for a in dancers)
        right = max(a["bounds"][0] + a["bounds"][2] for a in dancers)
        for walker in ("fleet-VIV", "fleet-Navines"):
            # The pair turns its own ring inside the kolo instead of standing in its path.
            box = scene[walker]["bounds"]
            self.assertLess(left, box[0]); self.assertGreater(right, box[0] + box[2])
        self.assertIn("fleet-EagleRed-shadow", scene)
        first = scene["fleet-VIV"]["bounds"]
        self.stage.eventually(lambda: actors()["fleet-VIV"]["bounds"][:2] != first[:2], "the inner pair did not move")
        self.run_dance(restore=True)

    def test_shared_scene_budget_and_independent_generation_release(self):
        self.stage.run_stage(composerDraft={"text": "Jedan zajednički prostor, isti nacrt.", "cursor": 20, "anchor": 4})
        draft = self.stage.editor_draft()
        first = self.run_dance(stateFile=str(self.stage.state_file), agentSocket=self.beats.path, act="kolo")
        self.assertTrue(self.beats.subscribed.wait(3))

        def scene_id():
            objects = [o for o in self.client.request("object.list", {}) if o.get("objectName") == "partySceneDepth"]
            self.assertLessEqual(len(objects), 1, "separate module registries duplicated the compositor")
            return objects[0]["id"] if objects else None

        self.stage.eventually(lambda: self.stage.run_stage()["sceneDepth"]["owners"] == 2
                              and self.run_dance()["frames"] >= 3, "both modules did not join the scene")
        shared = scene_id()
        self.assertIsNotNone(shared)
        scene = self.stage.run_stage()["sceneDepth"]
        self.assertEqual(scene["error"], "")
        self.assertTrue(any(a["name"].startswith("dance.kolo.dancer.") for a in scene["actors"]))
        names = {a["name"] for a in scene["actors"]}
        self.assertIn("fleet-VIV", names)
        # Name tags are separate scene items, so the floor in front of a robot
        # cannot cover its tag; the flyer's floor shadow marks its depth.
        self.assertTrue({"fleet-EagleRed-tag", "fleet-VIV-tag", "fleet-Navines-tag", "fleet-EagleRed-shadow"} <= names, names)
        # The busiest act uses 41 items; the fleet adds at most ten (ambience,
        # three bodies, their tags, the flyer's shadow, a delivery and its tag).
        self.assertLessEqual(len(scene["actors"]), 51)
        capture = os.environ.get("PARTY_DEPTH_CAPTURE")
        if capture:
            self.stage.run_stage(capture=str(Path(capture).with_suffix(".kolo.png")))

        # A real 4K widget exercises the wide streamer path. Every actor is drawn
        # into at most four window-sized atlas pages; all seven ribbons and both
        # prop holders survive.
        window = subprocess.check_output(["xdotool", "search", "--pid", str(self.stage.process.pid), "--name", "LARICS"],
                                         env=self.stage.environment, text=True).splitlines()[-1]
        subprocess.run(["xdotool", "windowsize", "--sync", window, "3840", "2160"],
                       env=self.stage.environment, check=True)
        before = self.run_dance()["frames"]
        self.run_dance(act="streamers")
        self.stage.eventually(lambda: self.run_dance()["frames"] > before + 2
                              and self.run_dance()["height"] >= 2100, "the4K actor surface did not render")
        large = self.stage.run_stage()["sceneDepth"]
        names = [a["name"] for a in large["actors"]]
        self.assertEqual(sum(n.startswith("dance.streamer.") for n in names), 7, large)
        self.assertEqual(sum(n.startswith("dance.fan.") for n in names), 2, large)
        atlas = large["atlas"]
        self.assertLessEqual(atlas["pages"], 4, large)
        self.assertLessEqual(large["rasterPixels"], atlas["pages"] * atlas["width"] * atlas["pageHeight"], large)
        self.assertEqual(large["error"], "")
        if capture:
            self.stage.run_stage(capture=str(Path(capture).with_suffix(".streamers4k.png")))
        subprocess.run(["xdotool", "windowsize", "--sync", window, "2560", "1600"],
                       env=self.stage.environment, check=True)

        # Release the registry's first module generation while the second still
        # owns it, then load a distinct DSO and rejoin the exact same QObject.
        before = self.run_dance()["frames"]
        self.stage.run_stage(restore=True)
        self.assertEqual(scene_id(), shared)
        self.stage.eventually(lambda: self.run_dance()["frames"] > before + 2, "dance stopped when the first owner left")
        import shutil
        stage_copy = self.stage.folder / "party-stage-next.so"
        shutil.copyfile(fixture.BUILD / "agent_snippet_party_stage.so", stage_copy)
        self.stage.module = self.client.request("snippet.load", {"path": str(stage_copy)})["moduleId"]
        self.stage.run_stage(stateFile=str(self.stage.state_file), composerDraft=draft)
        self.stage.eventually(lambda: self.stage.run_stage()["sceneDepth"]["owners"] == 2, "the new stage generation did not join")
        self.assertEqual(scene_id(), shared)
        self.assertEqual(self.stage.editor_draft(), draft)
        self.run_dance(restore=True)
        self.assertEqual(scene_id(), shared)
        self.assertEqual(self.stage.run_stage()["sceneDepth"]["owners"], 1)
        dance_copy = self.stage.folder / "party-dance-next.so"
        shutil.copyfile(fixture.BUILD / "agent_snippet_party_dance.so", dance_copy)
        self.module = self.client.request("snippet.load", {"path": str(dance_copy)})["moduleId"]
        self.run_dance(stateFile=str(self.stage.state_file), agentSocket=self.beats.path)
        self.stage.eventually(lambda: self.stage.run_stage()["sceneDepth"]["owners"] == 2, "the new dance generation did not join")
        self.assertEqual(scene_id(), shared)
        self.run_dance(restore=True); self.stage.run_stage(restore=True)
        self.assertIsNone(scene_id(), "last-owner release retained the registry")

        # Reverse acquisition: the dance DSO now creates the registry, the stage
        # joins, and the creating dance owner can leave first.
        self.run_dance(stateFile=str(self.stage.state_file), agentSocket=self.beats.path, act="conga")
        reverse = scene_id(); self.assertIsNotNone(reverse)
        self.stage.run_stage(stateFile=str(self.stage.state_file), composerDraft=draft)
        self.stage.eventually(lambda: self.stage.run_stage()["sceneDepth"]["owners"] == 2, "reverse load order failed")
        self.assertEqual(scene_id(), reverse)
        self.run_dance(restore=True)
        before = self.stage.run_stage()["sceneDepth"]["frames"]
        self.stage.eventually(lambda: self.stage.run_stage()["sceneDepth"]["frames"] > before + 2,
                              "the stage stopped when the creating dance owner left")
        self.assertEqual(self.stage.editor_draft(), draft)
        self.assertFalse(self.stage.server.playback_requests or self.stage.server.skip_requests or self.stage.server.requests
                         or self.stage.server.message_attempts)
        self.stage.run_stage(restore=True)
        self.assertIsNone(scene_id())


    def test_shared_scene_rejects_worker_release_and_allows_gui_retry(self):
        self.run_dance(stateFile=str(self.stage.state_file), agentSocket=self.beats.path, act="kolo")
        self.stage.run_stage(composerDraft={"text": "Sačuvaj nacrt pri odbijenom uklanjanju.", "cursor": 18, "anchor": 4})
        draft = self.stage.editor_draft()
        self.stage.eventually(lambda: self.stage.run_stage()["sceneDepth"]["owners"] == 2, "owners did not join")
        registry = next(o["id"] for o in self.client.request("object.list", {}) if o.get("objectName") == "partySceneDepth")
        source = self.stage.folder / "private_executor_target.cpp"
        source.write_text(r"""#include "agent/agent_abi.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QThread>
namespace {
QPointer<QObject> target;
QPointer<QThread> worker;
void run(const RuntimeAgentHost* host) {
    auto request=QJsonDocument::fromJson(QByteArray(host->request_json(host->invocation_context))).object();
    const auto action=request.value("action").toString();
    if(action=="create") {
        auto* cube=static_cast<QObject*>(host->find_qobject(host->agent_context,"cubeView"));
        worker=new QThread(cube); worker->setObjectName("privateReleaseThread");
        target=new QObject(cube); target->setObjectName("privateReleaseTarget");
    } else if(action=="move") {
        target->setParent(nullptr);target->moveToThread(worker);worker->start();
    } else if(action=="stop") {
        if(target) QMetaObject::invokeMethod(target,[]{delete target.data();},Qt::BlockingQueuedConnection);
        if(worker){worker->quit();worker->wait();delete worker.data();}
    }
    host->complete_json(host->invocation_context,"{\"ok\":true}");
}
const RuntimeAgentSnippet descriptor{RUNTIME_AGENT_ABI,sizeof(RuntimeAgentSnippet),"private release executor target",&run};
}
extern "C" RUNTIME_AGENT_EXPORT const RuntimeAgentSnippet* runtime_agent_snippet_init(){return &descriptor;}
""")
        library = self.stage.folder / "private_executor_target.so"
        compiled = subprocess.run([fixture.sys.executable, str(fixture.ROOT / "tools" / "compile_snippet.py"),
            "--compile-db", str(fixture.BUILD / "compile_commands.json"), "--context", "snippets/party_stage.cpp",
            "--source", str(source), "--output", str(library), "--compact"], capture_output=True, text=True)
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        module = self.client.request("snippet.load", {"path": str(library)})["moduleId"]

        def target(action):
            operation = self.client.request("snippet.run", {"moduleId": module, "executor": "gui", "request": {"action": action}})
            result = self.client.wait_for_operation(operation["operationId"], timeout=10)["data"]
            self.assertEqual(result["outcome"], "completed", result)

        target("create")
        identifier = next(o["id"] for o in self.client.request("object.list", {}) if o.get("objectName") == "privateReleaseTarget")
        target("move")
        try:
            for owner in (self.stage.module, self.module):
                operation = self.client.request("snippet.release", {"moduleId": owner, "executor": "object", "target": {"id": identifier}})
                result = self.client.wait_for_operation(operation["operationId"], timeout=10)["data"]
                self.assertEqual(result["outcome"], "failed", result)
                self.assertIn("GUI", json.dumps(result))
                self.assertEqual(self.stage.run_stage()["sceneDepth"]["owners"], 2)
                self.assertEqual(next(o["id"] for o in self.client.request("object.list", {})
                                      if o.get("objectName") == "partySceneDepth"), registry)
                self.assertEqual(self.stage.editor_draft(), draft)
            for owner, remaining in ((self.module, 1), (self.stage.module, 0)):
                operation = self.client.request("snippet.release", {"moduleId": owner, "executor": "gui"})
                result = self.client.wait_for_operation(operation["operationId"], timeout=10)["data"]
                self.assertEqual(result["outcome"], "completed", result)
                if remaining:
                    self.assertEqual(self.stage.run_stage()["sceneDepth"]["owners"], remaining)
                    self.assertEqual(self.stage.editor_draft(), draft)
                else:
                    self.assertFalse(any(o.get("objectName") == "partySceneDepth" for o in self.client.request("object.list", {})))
        finally:
            target("stop")
        self.assertFalse(self.stage.server.playback_requests or self.stage.server.skip_requests or self.stage.server.requests
                         or self.stage.server.message_attempts)


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
