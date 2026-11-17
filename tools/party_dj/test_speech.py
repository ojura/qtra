"""Speech scheduling and stream mixing tests, without audible output."""
import copy
import math
from pathlib import Path
import tempfile
import threading
import time
import unittest

from speech import DedicationAudio, SpeechCancelled, SpeechError, SpotifyMixer
import server
from test_server import FakeSpotify, TRACK_A, TRACK_RELINKED, track


class FakePrepared:
    path = Path('/unused-fixture.wav')
    seconds = 1

    def close(self):
        self.closed = True


class FakeVoice:
    available = True

    def __init__(self):
        self.calls = []

    def prepare(self, text):
        self.calls.append(text)
        return FakePrepared()


class FakeMixer:
    available = True

    def __init__(self):
        self.calls = []

    def play(self, prepared, valid, stop):
        if not valid():
            raise SpeechCancelled('Song changed.')
        self.calls.append(prepared)


class FakePlayer:
    def __init__(self):
        self.returncode = None
        self.polls = 0
        self.terminated = False

    def poll(self):
        self.polls += 1
        if self.polls >= 3:
            self.returncode = 0
        return self.returncode

    def terminate(self):
        self.terminated = True
        self.returncode = -15

    def wait(self, timeout=None):
        return self.returncode


class FakePulse:
    available = True

    def __init__(self):
        self.current = {'index': 7, 'sink': 9, 'corked': False, 'mute': False,
                        'properties': {'object.serial': '45'},
                        'volume': {'left': {'value': 65536}, 'right': {'value': 50000}}}
        self.sets = []
        self.player = None
        self.play_error = False
        self.manual_change = False
        self.route_change = False

    def find_spotify(self):
        return copy.deepcopy(self.current)

    def stream(self, index):
        if self.player and self.manual_change:
            self.current['volume']['left']['value'] = 12000
            self.current['volume']['right']['value'] = 12000
        if self.player and self.route_change:
            self.current['sink'] = 10
        return copy.deepcopy(self.current)

    @staticmethod
    def volumes(stream):
        return tuple(v['value'] for v in stream['volume'].values())

    def set_volume(self, index, values):
        self.sets.append(values)
        for channel, value in zip(self.current['volume'].values(), values):
            channel['value'] = value

    def sink_name(self, index):
        return 'party-speaker'

    def play(self, path, sink, volume):
        if self.play_error:
            raise SpeechError('Player failed to start.')
        self.player = FakePlayer()
        self.output = sink
        self.voice_volume = volume
        return self.player


class MixerTests(unittest.TestCase):
    def setUp(self):
        self.pulse = FakePulse()
        self.mixer = SpotifyMixer(self.pulse, sleep=lambda _: None)
        self.original = self.pulse.volumes(self.pulse.current)

    def test_speech_fades_only_spotify_and_restores_both_channels(self):
        self.mixer.play(FakePrepared(), lambda: True, threading.Event())
        self.assertEqual(self.pulse.output, 'party-speaker')
        self.assertEqual(self.pulse.volumes(self.pulse.current), self.original)
        values = [v[0] for v in self.pulse.sets]
        trough = values.index(min(values))
        self.assertGreater(trough, 2)
        self.assertEqual(values[:trough+1], sorted(values[:trough+1], reverse=True))
        self.assertEqual(values[trough:], sorted(values[trough:]))
        attenuation_db = 60 * math.log10(min(values) / self.original[0])
        self.assertAlmostEqual(attenuation_db, -10, places=2)
        self.assertEqual(self.pulse.voice_volume, 57000)

    def test_speech_volume_follows_a_low_spotify_stream_level(self):
        for channel in self.pulse.current['volume'].values():
            channel['value'] = 16384
        self.mixer.play(FakePrepared(), lambda: True, threading.Event())
        self.assertEqual(self.pulse.voice_volume, 14250)
        self.assertEqual(self.pulse.volumes(self.pulse.current), (16384, 16384))

    def test_failed_player_restores_music(self):
        self.pulse.play_error = True
        with self.assertRaisesRegex(SpeechError, 'failed to start'):
            self.mixer.play(FakePrepared(), lambda: True, threading.Event())
        self.assertEqual(self.pulse.volumes(self.pulse.current), self.original)

    def test_host_volume_change_is_preserved(self):
        self.pulse.manual_change = True
        with self.assertRaisesRegex(SpeechCancelled, 'Host changed'):
            self.mixer.play(FakePrepared(), lambda: True, threading.Event())
        self.assertEqual(self.pulse.volumes(self.pulse.current), (12000, 12000))
        self.assertTrue(self.pulse.player.terminated)

    def test_output_change_stops_speech_and_restores_stream(self):
        self.pulse.route_change = True
        with self.assertRaisesRegex(SpeechCancelled, 'Playback changed'):
            self.mixer.play(FakePrepared(), lambda: True, threading.Event())
        self.assertTrue(self.pulse.player.terminated)
        self.assertEqual(self.pulse.volumes(self.pulse.current), self.original)

    def test_skip_during_fade_restores_music_without_starting_voice(self):
        calls = []
        def valid():
            calls.append(True)
            return len(calls) < 5
        with self.assertRaises(SpeechCancelled):
            self.mixer.play(FakePrepared(), valid, threading.Event())
        self.assertIsNone(self.pulse.player)
        self.assertEqual(self.pulse.volumes(self.pulse.current), self.original)


class DedicationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='dedication-test-')
        self.addCleanup(self.directory.cleanup)
        self.voice, self.mixer = FakeVoice(), FakeMixer()
        self.audio = DedicationAudio(Path(self.directory.name), self.voice, self.mixer)
        self.addCleanup(self.audio.close)

    def wait_for(self, predicate):
        deadline = time.monotonic() + 3
        while not predicate():
            if time.monotonic() > deadline:
                self.fail('Speech worker did not finish the fixture operation.')
            time.sleep(.01)

    def current(self, uid='first', **extra):
        return {'uid': uid, 'uri': TRACK_A, 'playing': True, 'progressMs': 2000, **extra}

    def test_startup_current_and_empty_dedications_stay_silent(self):
        self.audio.observe(self.current(requestId='old-request', dedication='Old dedication'), [])
        self.audio.observe(self.current('next', requestId='empty-request', dedication=''), [])
        self.assertEqual(len(self.audio.jobs), 0)
        self.assertEqual(self.audio.history, {})

    def test_prepared_dedication_plays_once_across_observations_and_restart(self):
        request = {'id': 'one-request', 'status': 'queued', 'dedication': 'Za cijelu ekipu.'}
        self.audio.observe(self.current(), [request])
        self.audio.start()
        self.wait_for(lambda: 'one-request' in self.audio.ready)
        current = self.current('queue-2', requestId='one-request', dedication=request['dedication'])
        self.audio.observe(current, [])
        self.audio.observe(current, [])
        self.wait_for(lambda: self.audio.last and self.audio.last['status'] == 'played')
        self.assertEqual(len(self.mixer.calls), 1)
        self.assertEqual(self.voice.calls, ['Za cijelu ekipu.'])
        self.audio.close()
        restarted = DedicationAudio(Path(self.directory.name), FakeVoice(), FakeMixer())
        self.addCleanup(restarted.close)
        restarted.observe(self.current('another-song'), [])
        restarted.observe(current, [])
        self.assertEqual(len(restarted.jobs), 0)

    def test_song_change_before_worker_runs_cancels_the_dedication(self):
        self.audio.observe(self.current(), [])
        self.audio.observe(self.current('queue-2', requestId='one-request', dedication='Za ekipu.'), [])
        self.audio.observe(self.current('queue-3'), [])
        self.audio.start()
        self.wait_for(lambda: self.audio.last and self.audio.last['status'] == 'cancelled')
        self.assertEqual(self.mixer.calls, [])

    def test_manual_retries_are_idempotent_and_text_cannot_change(self):
        self.audio.observe(self.current(), [])
        first = self.audio.submit('trial-one', 'Pozdrav ekipi.')
        second = self.audio.submit('trial-one', 'Pozdrav ekipi.')
        self.assertEqual(first['id'], second['id'])
        self.assertEqual(len(self.audio.jobs), 1)
        with self.assertRaises(ValueError):
            self.audio.submit('trial-one', 'Druga poruka.')

    def test_disabling_speech_rejects_manual_and_automatic_voice(self):
        self.audio.observe(self.current(), [])
        self.audio.set_enabled(False)
        self.audio.observe(self.current('queue-2', requestId='one-request', dedication='Za ekipu.'), [])
        self.assertEqual(len(self.audio.jobs), 0)
        self.assertFalse(self.audio.submit('trial-one', 'Pozdrav ekipi.')['queued'])
        self.assertFalse(self.audio.snapshot()['enabled'])

    def test_party_passes_dedication_through_catalogue_relinking(self):
        client = FakeSpotify()
        party = server.Party(Path(self.directory.name), client=client, audio=self.audio)
        self.addCleanup(party.db.close)
        party._refresh()
        party.request({'requestId': 'dedication-a', 'uri': TRACK_A, 'dedication': 'Za plesače.'})
        party._dispatch()
        uid = client.queue[0]['uid']
        client.queue = []
        client.current = {**track(TRACK_RELINKED), 'uid': uid, 'playing': True, 'progressMs': 500}
        party._refresh()
        self.assertEqual(len(self.audio.jobs), 1)
        self.assertIn(uid, self.audio.jobs[0]['key'])
        self.assertEqual(self.audio.jobs[0]['text'], 'Za plesače.')


if __name__ == '__main__':
    unittest.main()
