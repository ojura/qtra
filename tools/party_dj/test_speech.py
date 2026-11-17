"""Speech scheduling and stream mixing tests, without audible output."""
import copy
import math
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch

from speech import DedicationAudio, SpeechCancelled, SpeechError, SpotifyMixer
import server
from test_server import FakeSpotify, TRACK_A, TRACK_RELINKED, track


class FakePrepared:
    path = Path('/unused-fixture.wav')
    seconds = 1

    def __init__(self):
        self.closed = False

    def close(self):
        self.closed = True


class FakeVoice:
    available = True

    def __init__(self):
        self.calls = []
        self.prepared = []
        self.started = threading.Event()
        self.release = None
        self.error = None

    def prepare(self, text):
        self.calls.append(text)
        self.started.set()
        if self.release is not None and not self.release.wait(3):
            raise AssertionError('The fixture did not release preparation.')
        if self.error is not None:
            raise self.error
        result = FakePrepared()
        self.prepared.append(result)
        return result


class FakeMixer:
    available = True

    def __init__(self):
        self.calls = []
        self.started = threading.Event()
        self.release = None
        self.error = None
        self.before_start = None
        self.after_start = None
        self.result = {'kind': 'process_exit', 'returnCode': 0,
                       'audibleVerification': 'unperformed', 'simulated': True}

    def play(self, prepared, valid, stop, on_started=None):
        if not valid():
            raise SpeechCancelled('Song changed.')
        if self.before_start:
            self.before_start()
        if self.error:
            raise self.error
        if on_started:
            on_started()
        if self.after_start:
            self.after_start()
        self.calls.append(prepared)
        self.started.set()
        if self.release is not None and not self.release.wait(3):
            raise AssertionError('The fixture did not release playback.')
        if not valid() or stop.is_set():
            raise SpeechCancelled('Bound playback changed.')
        return self.result


class FakePlayer:
    def __init__(self):
        self.returncode = None
        self.polls = 0
        self.terminated = False

    def poll(self):
        self.polls += 1
        if self.polls >= 4 and self.returncode is None:
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
        self.exit_code = None

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
        self.player.returncode = self.exit_code
        self.output = sink
        self.voice_volume = volume
        return self.player


class SilentTestCase(unittest.TestCase):
    def setUp(self):
        super().setUp()
        # Accidental real transports fail closed, even if a worker catches the error.
        for name in ('subprocess.run', 'subprocess.Popen', 'socket.socket'):
            boundary = patch(name, side_effect=AssertionError('Forbidden test boundary: ' + name))
            guarded = boundary.start()
            self.addCleanup(boundary.stop)
            self.addCleanup(guarded.assert_not_called)
        for name, result in (('_lan_url', 'http://fixture.invalid'), ('_write_qr', None)):
            boundary = patch.object(server.Party, name, return_value=result)
            boundary.start()
            self.addCleanup(boundary.stop)


class MixerTests(SilentTestCase):
    def setUp(self):
        super().setUp()
        self.pulse = FakePulse()
        self.mixer = SpotifyMixer(self.pulse, sleep=lambda _: None)
        self.original = self.pulse.volumes(self.pulse.current)

    def test_speech_fades_only_spotify_and_restores_both_channels(self):
        starts = []
        result = self.mixer.play(FakePrepared(), lambda: True, threading.Event(),
                                 on_started=lambda: starts.append(True))
        self.assertEqual(starts, [True])
        self.assertEqual(result, {'kind': 'process_exit', 'returnCode': 0,
                                  'audibleVerification': 'unperformed'})
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

    def test_nonzero_player_exit_is_failure_without_a_started_callback(self):
        self.pulse.exit_code = 1
        starts = []
        with self.assertRaisesRegex(SpeechError, 'could not start'):
            self.mixer.play(FakePrepared(), lambda: True, threading.Event(),
                            on_started=lambda: starts.append(True))
        self.assertEqual(starts, [])
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


class DedicationTests(SilentTestCase):
    def setUp(self):
        super().setUp()
        self.directory = tempfile.TemporaryDirectory(prefix='dedication-test-')
        self.addCleanup(self.directory.cleanup)
        self.now = 100.0
        self.voice, self.mixer = FakeVoice(), FakeMixer()
        self.audio = DedicationAudio(Path(self.directory.name), self.voice, self.mixer,
                                     clock=lambda: self.now, wall_clock=lambda: 1800000000 + self.now)
        self.addCleanup(self.audio.close)

    def wait_for(self, predicate):
        with self.audio.changed:
            self.assertTrue(self.audio.changed.wait_for(predicate, timeout=3),
                            'Speech worker did not finish the fixture operation.')

    def current(self, uid='first', **extra):
        return {'uid': uid, 'uri': TRACK_A, 'playing': True, 'progressMs': 2000,
                'observedAt': int((1800000000 + self.now) * 1000), **extra}

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
        ready = self.audio.snapshot()
        self.assertEqual(ready['preparations'][0]['status'], 'ready')
        self.assertTrue(ready['preparations'][0]['ready'])
        self.assertEqual(ready['attention'], [])
        self.now += 3
        current = self.current('queue-2', requestId='one-request', dedication=request['dedication'])
        self.audio.observe(current, [])
        self.audio.observe(current, [])
        self.wait_for(lambda: self.audio.last and self.audio.last['status'] == 'completed')
        self.assertEqual(len(self.mixer.calls), 1)
        self.assertEqual(self.voice.calls, ['Za cijelu ekipu.'])
        self.assertEqual(self.audio.last['evidence']['audibleVerification'], 'unperformed')
        self.assertTrue(self.audio.last['evidence']['simulated'])
        self.audio.close()
        restarted = DedicationAudio(Path(self.directory.name), FakeVoice(), FakeMixer(),
                                     clock=lambda: self.now, wall_clock=lambda: 1800000000 + self.now)
        self.addCleanup(restarted.close)
        restarted.observe(self.current('another-song'), [])
        restarted.observe(current, [])
        self.assertEqual(len(restarted.jobs), 0)

    def test_song_change_before_worker_runs_cancels_the_dedication(self):
        request = {'id': 'one-request', 'status': 'queued', 'dedication': 'Za ekipu.'}
        self.audio.observe(self.current(), [request])
        self.audio.start()
        self.wait_for(lambda: 'one-request' in self.audio.ready)
        self.now += 3
        with self.audio.changed:
            self.audio.observe(self.current('queue-2', requestId='one-request', dedication='Za ekipu.'), [])
            self.audio.observe(self.current('queue-3'), [])
        self.wait_for(lambda: self.audio.last and self.audio.last['status'] == 'cancelled')
        self.assertEqual(self.mixer.calls, [])
        self.assertTrue(self.voice.prepared[0].closed)

    def test_automatic_dedication_not_ready_at_start_is_missed_without_synthesis(self):
        self.audio.observe(self.current(), [])
        current = self.current('queue-2', requestId='one-request', dedication='Za ekipu.')
        self.audio.observe(current, [])
        self.audio.observe(current, [])
        self.assertEqual(self.audio.last['status'], 'missed')
        self.assertEqual(self.audio.snapshot()['attention'], [])
        self.assertEqual(list(self.audio.jobs), [])
        self.assertEqual(self.voice.calls, [])
        self.assertEqual(self.mixer.calls, [])

    def test_missing_invalid_future_or_expired_observations_cannot_start_speech(self):
        self.audio.observe(self.current(), [])
        stamp = int((1800000000 + self.now) * 1000)
        cases = [
            ('missing-progress', 'progressMs', None), ('bool-progress', 'progressMs', False),
            ('nan-progress', 'progressMs', float('nan')), ('infinite-progress', 'progressMs', float('inf')),
            ('negative-progress', 'progressMs', -1), ('text-progress', 'progressMs', '0'),
            ('oversized-progress', 'progressMs', 10**1000),
            ('missing-clock', 'observedAt', None), ('bool-clock', 'observedAt', True),
            ('nan-clock', 'observedAt', float('nan')), ('infinite-clock', 'observedAt', float('inf')),
            ('text-clock', 'observedAt', str(stamp)), ('negative-clock', 'observedAt', -1),
            ('future-clock', 'observedAt', stamp + 1), ('expired-clock', 'observedAt', stamp - 10000),
            ('paused', 'paused', True), ('buffering', 'buffering', True)]
        for name, field, value in cases:
            with self.subTest(case=name):
                current = self.current('uid-' + name, requestId='request-' + name, dedication='Za ekipu.')
                if value is None:
                    current.pop(field, None)
                else:
                    current[field] = value
                self.audio.observe(current, [])
                self.assertFalse(self.audio.submit('trial-' + name, 'Pozdrav ekipi.')['queued'])
                self.assertEqual(list(self.audio.jobs), [])
                self.assertEqual(self.audio.history, {})
        self.assertEqual(self.voice.calls, [])
        self.assertEqual(self.mixer.calls, [])

    def test_invalid_current_clock_flag_blocks_fresh_numeric_fallbacks(self):
        self.audio.observe(self.current(), [])
        for name, value in (('false', False), ('null', None), ('zero', 0),
                            ('one', 1), ('text', 'true')):
            with self.subTest(clockValid=name):
                current = self.current('clock-' + name, requestId='request-' + name,
                                       dedication='Za ekipu.', progressMs=0, clockValid=value)
                self.audio.observe(current, [], playback_state={'clockValid': True})
                self.assertFalse(self.audio.submit('trial-' + name, 'Pozdrav ekipi.')['queued'])
                self.assertEqual(list(self.audio.jobs), [])
                self.assertEqual(self.audio.history, {})
        self.assertEqual(self.voice.calls, [])
        self.assertEqual(self.mixer.calls, [])

    def test_raw_invalid_clock_overrides_numeric_fallback_and_cancels_pending_speech(self):
        self.audio.observe(self.current(), [])
        for name, value in (('false', False), ('null', None), ('zero', 0),
                            ('one', 1), ('text', 'true')):
            with self.subTest(clockValid=name):
                current = self.current('raw-clock-' + name, requestId='request-' + name,
                                       dedication='Za ekipu.', progressMs=0, clockValid=True)
                self.audio.observe(current, [], playback_state={'clockValid': value})
                self.assertFalse(self.audio.submit('trial-' + name, 'Pozdrav ekipi.')['queued'])
                self.assertEqual(list(self.audio.jobs), [])
                self.assertEqual(self.audio.history, {})
        current = self.current('queued-clock', progressMs=0, clockValid=True)
        self.audio.observe(current, [], playback_state={'clockValid': True})
        self.assertTrue(self.audio.submit('trial-queued', 'Pozdrav ekipi.')['queued'])
        job = self.audio.jobs[0]
        self.audio.observe(current, [], playback_state={'clockValid': False})
        self.audio.observe(current, [], playback_state={'clockValid': True})
        self.assertEqual(list(self.audio.jobs), [])
        self.assertEqual(self.audio.history['manual:trial-queued']['status'], 'cancelled')
        self.assertFalse(self.audio._valid(job))
        self.assertEqual(self.voice.calls, [])
        self.assertEqual(self.mixer.calls, [])

    def test_valid_zero_progress_admits_an_already_prepared_occurrence(self):
        request = {'id': 'one-request', 'status': 'queued', 'dedication': 'Za ekipu.'}
        self.audio.observe(self.current(), [request])
        self.audio.start()
        self.wait_for(lambda: 'one-request' in self.audio.ready)
        self.now += 1
        self.audio.observe(self.current('queue-2', requestId='one-request',
                                        dedication=request['dedication'], progressMs=0), [])
        self.wait_for(lambda: self.audio.last and self.audio.last['status'] == 'completed')
        self.assertEqual(len(self.mixer.calls), 1)

    def test_fresh_uri_with_missing_or_invalid_uid_cannot_admit_speech(self):
        self.audio.start()
        cases = [('missing', None), ('empty', ''), ('blank', '  '), ('true', True),
                 ('false', False), ('zero', 0), ('number', 123), ('list', ['uid']), ('object', {'id': 'uid'})]
        for name, uid in cases:
            with self.subTest(uid=name):
                rid = 'request-' + name
                request = {'id': rid, 'status': 'queued', 'dedication': 'Za ekipu.'}
                self.audio.observe(self.current('before-' + name), [request])
                self.wait_for(lambda: rid in self.audio.ready)
                self.now += 3
                current = self.current(uid, requestId=rid, dedication=request['dedication'], progressMs=0)
                if name == 'missing':
                    current.pop('uid')
                self.audio.observe(current, [])
                self.assertFalse(self.audio.submit('trial-' + name, 'Pozdrav ekipi.')['queued'])
                self.assertEqual(self.audio.last['status'], 'missed')
                self.assertIn('no verified UID', self.audio.last['error'])
                self.assertEqual(list(self.audio.jobs), [])
                self.assertEqual(self.mixer.calls, [])
                self.assertTrue(self.voice.prepared[-1].closed)

    def test_manual_speech_accepts_fresh_string_uid_at_exact_zero_progress(self):
        self.audio.observe(self.current('manual-zero', progressMs=0, clockValid=True), [],
                           playback_state={'clockValid': True})
        self.assertTrue(self.audio.submit('trial-zero', 'Pozdrav ekipi.')['queued'])
        self.assertEqual(self.audio.jobs[0]['track'], 'manual-zero')
        self.audio.start()
        self.wait_for(lambda: self.audio.last and self.audio.last['status'] == 'completed')
        self.assertEqual(len(self.mixer.calls), 1)

    def test_observation_expiry_during_preparation_cancels_before_start(self):
        self.voice.release = threading.Event()
        self.addCleanup(self.voice.release.set)
        current = self.current()
        self.audio.observe(current, [])
        self.audio.submit('trial-one', 'Pozdrav ekipi.')
        self.audio.start()
        self.assertTrue(self.voice.started.wait(3))
        self.now += 10
        self.voice.release.set()
        self.wait_for(lambda: self.audio.last and self.audio.last['status'] == 'cancelled')
        self.assertEqual(self.mixer.calls, [])
        self.audio.observe(current, [])
        self.assertFalse(self.audio.submit('trial-two', 'Pozdrav ekipi.')['queued'])
        self.assertEqual(self.audio.current_seen, self.now)

    def test_invalid_wall_clock_and_raw_pause_state_cannot_admit_manual_speech(self):
        self.audio.observe(self.current(), [])
        for value in (True, float('nan'), float('inf')):
            with self.subTest(clock=value), patch.object(self.audio, 'wall_clock', return_value=value):
                self.assertFalse(self.audio.submit('trial-one', 'Pozdrav ekipi.')['queued'])
        for flag in ('paused', 'buffering'):
            self.audio.observe(self.current(), [], playback_state={flag: True, 'device': {'id': 'a'}})
            self.assertFalse(self.audio.submit('trial-' + flag, 'Pozdrav ekipi.')['queued'])
        self.assertEqual(self.voice.calls, [])
        self.assertEqual(self.mixer.calls, [])

    def test_late_preparation_is_closed_and_cannot_recover_a_missed_occurrence(self):
        self.voice.release = threading.Event()
        self.addCleanup(self.voice.release.set)
        request = {'id': 'one-request', 'status': 'queued', 'dedication': 'Za ekipu.'}
        self.audio.observe(self.current(), [request])
        self.audio.start()
        self.assertTrue(self.voice.started.wait(3))
        current = self.current('queue-2', requestId='one-request', dedication=request['dedication'])
        self.now += 3
        self.audio.observe(current, [])
        self.assertEqual(self.audio.last['status'], 'missed')
        self.voice.release.set()
        self.wait_for(lambda: 'one-request' not in self.audio.preparing)
        self.assertTrue(self.voice.prepared[0].closed)
        self.assertNotIn('one-request', self.audio.ready)
        self.audio.observe(current, [])
        self.assertEqual(self.mixer.calls, [])
        self.assertEqual(len(self.voice.calls), 1)
        self.assertEqual(self.audio.last['status'], 'missed')

    def test_ready_after_actual_song_start_is_missed_even_before_first_observation(self):
        request = {'id': 'one-request', 'status': 'queued', 'dedication': 'Za ekipu.'}
        self.audio.observe(self.current(), [request])
        self.audio.start()
        self.wait_for(lambda: 'one-request' in self.audio.ready)
        self.now += 1
        self.audio.observe(self.current('queue-2', requestId='one-request',
                                        dedication=request['dedication'], progressMs=2000), [])
        self.assertEqual(self.audio.last['status'], 'missed')
        self.assertIn('after its occurrence started', self.audio.last['error'])
        self.assertTrue(self.voice.prepared[0].closed)
        self.assertEqual(self.mixer.calls, [])

    def test_pause_skip_device_and_disable_cancellation_survive_return_to_same_track(self):
        self.audio.start()
        device = {'device': {'id': 'device-a'}}
        for action in ('pause', 'interrupt', 'skip', 'device', 'disable'):
            with self.subTest(action=action):
                current = self.current('occurrence-' + action)
                self.mixer.started.clear()
                self.mixer.release = threading.Event()
                self.addCleanup(self.mixer.release.set)
                self.audio.observe(current, [], playback_state=device)
                request_id = 'trial-' + action
                self.audio.submit(request_id, 'Pozdrav ekipi.')
                self.assertTrue(self.mixer.started.wait(3))
                self.assertTrue(self.audio.snapshot()['speaking'])
                if action == 'pause':
                    self.audio.observe({**current, 'playing': False, 'paused': True}, [],
                                       playback_state=device)
                elif action == 'interrupt':
                    self.audio.interrupt()
                elif action == 'skip':
                    self.audio.observe(self.current('another-occurrence'), [], playback_state=device)
                elif action == 'device':
                    self.audio.observe(current, [], playback_state={'device': {'id': 'device-b'}})
                else:
                    self.audio.set_enabled(False)
                    self.audio.set_enabled(True)
                self.audio.observe(current, [], playback_state=device)
                self.mixer.release.set()
                key = 'manual:' + request_id
                self.wait_for(lambda: self.audio.history[key]['status'] == 'cancelled')
                self.assertNotIn('evidence', self.audio.history[key])
                self.assertTrue(self.voice.prepared[-1].closed)
                self.assertEqual(self.audio.snapshot()['attention'], [])

    def test_pause_during_manual_preparation_cancels_before_mixer_start(self):
        self.voice.release = threading.Event()
        self.addCleanup(self.voice.release.set)
        current = self.current()
        self.audio.observe(current, [])
        self.audio.submit('trial-one', 'Pozdrav ekipi.')
        self.audio.start()
        self.assertTrue(self.voice.started.wait(3))
        self.audio.observe({**current, 'playing': False}, [])
        self.audio.observe(current, [])
        self.voice.release.set()
        self.wait_for(lambda: self.audio.last and self.audio.last['status'] == 'cancelled')
        self.assertEqual(self.mixer.calls, [])
        self.assertTrue(self.voice.prepared[0].closed)

    def test_removed_or_disabled_preparation_is_closed_on_completion(self):
        self.audio.start()
        for action in ('removed', 'disabled'):
            with self.subTest(action=action):
                self.audio.set_enabled(True)
                self.voice.started.clear()
                self.voice.release = threading.Event()
                self.addCleanup(self.voice.release.set)
                rid = 'request-' + action
                request = {'id': rid, 'status': 'queued', 'dedication': 'Za ekipu.'}
                self.audio.observe(self.current(), [request])
                self.assertTrue(self.voice.started.wait(3))
                if action == 'removed':
                    self.audio.observe(self.current(), [])
                else:
                    self.audio.set_enabled(False)
                self.voice.release.set()
                self.wait_for(lambda: rid not in self.audio.preparing)
                self.assertTrue(self.voice.prepared[-1].closed)
                self.assertNotIn(rid, self.audio.ready)
                self.assertEqual(self.audio.snapshot()['preparations'], [])

    def test_preparation_failure_is_visible_as_exceptional_attention_without_handoff(self):
        self.voice.error = SpeechError('Fixture preparation failed.')
        request = {'id': 'one-request', 'status': 'queued', 'dedication': 'Za ekipu.'}
        self.audio.observe(self.current(), [request])
        self.audio.start()
        self.wait_for(lambda: 'one-request' in self.audio.prepare_errors)
        state = self.audio.snapshot()
        self.assertEqual(state['preparations'][0]['status'], 'failed')
        self.assertFalse(state['preparations'][0]['ready'])
        self.assertEqual(state['attention'], [{'requestId': 'one-request', 'kind': 'preparation_failed',
                                              'error': 'Fixture preparation failed.'}])
        self.now += 3
        self.audio.observe(self.current('queue-2', requestId='one-request', dedication=request['dedication']), [])
        self.assertEqual(self.audio.last['status'], 'missed')
        self.assertEqual(self.mixer.calls, [])
        self.assertEqual(len(self.voice.calls), 1)

    def test_preparation_snapshot_is_bounded_and_unavailability_is_explicit(self):
        requests = [{'id': f'request-{i}', 'status': 'queued', 'uid': f'uid-{i}',
                     'dedication': 'Za ekipu.'} for i in range(10)]
        self.audio.observe(self.current(), requests)
        preparations = self.audio.snapshot()['preparations']
        self.assertEqual([item['requestId'] for item in preparations], ['request-0', 'request-1', 'request-2'])
        self.assertEqual([item['occurrenceUid'] for item in preparations], ['uid-0', 'uid-1', 'uid-2'])
        self.assertTrue(all(not item['ready'] for item in preparations))
        voice = FakeVoice()
        voice.available = False
        unavailable = DedicationAudio(Path(self.directory.name), voice, FakeMixer())
        self.addCleanup(unavailable.close)
        self.assertFalse(unavailable.snapshot()['available'])
        self.assertEqual(unavailable.snapshot()['attention'][0]['kind'], 'unavailable')
        self.assertFalse(unavailable.submit('trial-one', 'Pozdrav ekipi.')['queued'])

    def test_start_and_completion_states_report_process_evidence_without_audibility_claim(self):
        phases = []
        self.mixer.before_start = lambda: phases.append(self.audio.snapshot()['last']['status'])
        self.mixer.after_start = lambda: phases.append(self.audio.snapshot()['last']['status'])
        self.audio.observe(self.current(), [])
        self.audio.submit('trial-one', 'Pozdrav ekipi.')
        self.audio.start()
        self.wait_for(lambda: self.audio.last and self.audio.last['status'] == 'completed')
        self.assertEqual(phases, ['starting', 'speaking'])
        self.assertEqual(self.audio.last['evidence'], self.mixer.result)
        self.assertFalse(self.audio.snapshot()['speaking'])
        self.assertEqual(self.audio.snapshot()['attention'], [])

    def test_mixer_failure_or_missing_completion_evidence_never_records_completed(self):
        self.audio.observe(self.current(), [])
        self.audio.start()
        for case in ('failed', 'missing-evidence', 'invalid-exit-code'):
            with self.subTest(case=case):
                self.mixer.error = SpeechError('Fixture player failed.') if case == 'failed' else None
                self.mixer.result = ({'kind': 'process_exit', 'returnCode': False}
                                     if case == 'invalid-exit-code' else None)
                request_id = 'trial-' + case
                self.audio.submit(request_id, 'Pozdrav ekipi.')
                key = 'manual:' + request_id
                self.wait_for(lambda: self.audio.history[key]['status'] == 'failed')
                self.assertNotIn('evidence', self.audio.history[key])
                self.assertEqual(self.audio.snapshot()['attention'], [])

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
        party.admin('configure', {'speechEnabled': True})
        party._refresh()
        party.request({'requestId': 'dedication-a', 'uri': TRACK_A, 'dedication': 'Za plesače.'})
        party._dispatch()
        uid = client.queue[0]['uid']
        party._refresh()
        self.audio.start()
        self.wait_for(lambda: 'dedication-a' in self.audio.ready)
        self.now += 3
        client.queue = []
        client.current = {**track(TRACK_RELINKED), 'uid': uid, 'playing': True, 'progressMs': 500,
                          'observedAt': int((1800000000 + self.now) * 1000)}
        party._refresh()
        self.wait_for(lambda: self.audio.last and self.audio.last['status'] == 'completed')
        self.assertIn(uid, self.audio.last['id'])
        self.assertEqual(self.audio.last['text'], 'Za plesače.')
        self.assertEqual(len(self.mixer.calls), 1)


if __name__ == '__main__':
    unittest.main()
