"""Private current-lyrics protocol fixtures: no sockets, providers or audio."""
from __future__ import annotations

import copy
import json
import threading
import unittest
from unittest.mock import patch

import lyrics


TARGET = {'uid': 'fixture-occurrence', 'uri': 'spotify:track:' + 'a' * 22, 'deviceId': 'fixture-device'}
RECORDING = {**TARGET, 'artUrl': 'https://fixture.invalid/cover', 'durationMs': 180000}


def ready():
    return {'target': TARGET.copy(), 'availability': 'ready', 'timing': 'line',
            'provider': {'name': 'Fixture', 'attribution': 'Fixture', 'sourceUrl': None},
            'lines': [{'text': 'Invented fixture line', 'startMs': 1234},
                      {'text': '', 'startMs': 2000}, {'text': 'Another fixture', 'startMs': 3456.5}]}


class ConnectedFixture:
    def __init__(self):
        self.lock = threading.RLock()
        self.ws, self.session, self.target = None, 'existing-fixture-session', None
        self.calls, self.unexpected = [], []
        self.result, self.state, self.identifier = ready(), 'done', None
        self.on_begin, self.on_poll, self.bootstrap = None, None, {'ready': True}

    def _connection_key(self):
        return self.ws, self.target, self.session

    def _evaluate_connected(self, expression, timeout=None):
        self.calls.append(expression)
        if expression.startswith('(() =>'):
            return self.bootstrap
        if expression.startswith('window.__laricsLyrics.begin(...'):
            arguments = json.loads(expression.removeprefix('window.__laricsLyrics.begin(...')[:-1])
            self.identifier = arguments[0]
            if self.on_begin:
                self.on_begin()
            return {'id': self.identifier, 'state': 'pending'}
        if expression.startswith('window.__laricsLyrics.poll('):
            if self.on_poll:
                self.on_poll()
            return {'id': self.identifier, 'state': self.state, 'result': copy.deepcopy(self.result)}
        if expression.startswith('window.__laricsLyrics?.cancel?.('):
            return {'state': 'cancelled'}
        self.unexpected.append(expression)
        raise AssertionError('Unexpected private fixture evaluation')

    def _blocked(self, *args, **kwargs):
        self.unexpected.append('connection or live operation')
        raise AssertionError('A lyrics read must not connect, reset, close or control playback')

    evaluate = _connect = _reset_transport = close = status = _blocked


class LyricsReaderTests(unittest.TestCase):
    def setUp(self):
        self.client = ConnectedFixture()
        self.reader = lyrics.LyricsReader(self.client)
        self.addCleanup(lambda: self.assertEqual(self.client.unexpected, []))
        delay = patch.object(lyrics, 'POLL_INTERVAL', .001)
        delay.start(); self.addCleanup(delay.stop)

    def test_preserves_source_starts_and_returns_a_private_copy(self):
        result = self.reader.read(TARGET, RECORDING)
        self.assertEqual(result, ready())
        self.assertEqual(len(self.client.calls), 3)
        result['lines'][0]['text'] = 'Changed fixture'
        self.assertEqual(self.client.result, ready())
        self.assertEqual(self.client.session, 'existing-fixture-session')

    def test_missing_metadata_or_capability_never_starts_a_provider_read(self):
        for recording in ({**RECORDING, 'artUrl': ''}, {**RECORDING, 'durationMs': 0}):
            with self.subTest(recording=recording):
                self.assertEqual(self.reader.read(TARGET, recording)['availability'], 'unavailable')
                self.assertEqual(self.client.calls, [])
        self.client.bootstrap = {'ready': False}
        self.assertEqual(self.reader.read(TARGET, RECORDING)['reason'], 'provider_unavailable')
        self.assertEqual(len(self.client.calls), 1)

    def test_missing_connection_and_changed_connection_do_not_reconnect(self):
        self.client.session = None
        with self.assertRaises(lyrics.LyricsReadError):
            self.reader.read(TARGET, RECORDING)
        self.assertEqual(self.client.calls, [])
        self.client.session = 'existing-fixture-session'
        self.client.on_begin = lambda: setattr(self.client, 'session', 'replacement-session')
        with self.assertRaisesRegex(lyrics.LyricsReadError, 'connection changed'):
            self.reader.read(TARGET, RECORDING)
        self.assertEqual(len(self.client.calls), 2)
        self.assertEqual(self.client.session, 'replacement-session')

    def test_stale_occurrence_or_wrong_result_binding_never_returns_lines(self):
        self.client.state = 'stale'
        with self.assertRaises(lyrics.LyricsReadError) as caught:
            self.reader.read(TARGET, RECORDING)
        self.assertEqual((caught.exception.status, caught.exception.code), (409, 'stale_target'))
        self.client.state = 'done'
        self.client.result['target']['uid'] = 'a-later-duplicate'
        with self.assertRaises(lyrics.LyricsReadError):
            self.reader.read(TARGET, RECORDING)
        with self.assertRaises(lyrics.LyricsReadError) as caught:
            self.reader.read(TARGET, {**RECORDING, 'uri': 'spotify:track:' + 'b' * 22})
        self.assertEqual(caught.exception.status, 409)

    def test_provider_failure_is_sanitized_without_transport_reset(self):
        def fail():
            raise RuntimeError('PRIVATE PROVIDER BODY AND TOKEN')
        self.client.on_poll = fail
        with self.assertRaises(lyrics.LyricsReadError) as caught:
            self.reader.read(TARGET, RECORDING)
        self.assertNotIn('PRIVATE', str(caught.exception))
        self.assertEqual(self.client.session, 'existing-fixture-session')
        self.assertTrue(self.client.calls[-1].startswith('window.__laricsLyrics?.cancel?.('))

    def test_pending_read_yields_shared_lock_and_times_out_without_retry(self):
        acquired = threading.Event()
        worker = None
        def access():
            with self.client.lock:
                acquired.set()
        def begin():
            nonlocal worker
            worker = threading.Thread(target=access)
            worker.start()
        self.client.on_begin = begin
        self.client.on_poll = lambda: self.assertTrue(acquired.wait(.5))
        try:
            self.assertEqual(self.reader.read(TARGET, RECORDING)['availability'], 'ready')
        finally:
            if worker:
                worker.join(1)
                self.assertFalse(worker.is_alive())
        self.client.on_begin = self.client.on_poll = None
        self.client.calls.clear()
        self.client.state = 'pending'
        with patch.object(lyrics, 'READ_TIMEOUT', .015):
            with self.assertRaisesRegex(lyrics.LyricsReadError, 'timed out'):
                self.reader.read(TARGET, RECORDING)
        self.assertEqual(sum(call.startswith('window.__laricsLyrics.begin(') for call in self.client.calls), 1)
        self.assertTrue(self.client.calls[-1].startswith('window.__laricsLyrics?.cancel?.('))

    def test_validator_rejects_malformed_timing_limits_and_payloads(self):
        mutations = [
            lambda v: v.update(target={**TARGET, 'uri': 'spotify:track:' + 'b' * 22}),
            lambda v: v['lines'][0].update(startMs=True),
            lambda v: v['lines'][0].update(startMs=float('nan')),
            lambda v: v['lines'][1].update(startMs=1000),
            lambda v: v['lines'][0].update(text='x' * 2001),
            lambda v: v['lines'][0].update(text='bad\x00data'),
            lambda v: v.update(lines=v['lines'] * 1001),
            lambda v: v['provider'].update(sourceUrl='https://user:password@fixture.invalid'),
            lambda v: v.update(availability='instrumental'),
            lambda v: v.update(timing='syllable'),
        ]
        for mutate in mutations:
            value = ready(); mutate(value)
            with self.subTest(mutation=mutations.index(mutate)):
                with self.assertRaises(lyrics.LyricsReadError):
                    lyrics.validate_result(value, TARGET)
        plain = ready(); plain['timing'] = 'none'
        for line in plain['lines']:
            line['startMs'] = None
        self.assertEqual(lyrics.validate_result(plain, TARGET), plain)
        absent = {'target': TARGET.copy(), 'availability': 'unavailable', 'timing': 'none',
                  'provider': None, 'lines': [], 'reason': 'no_lyrics'}
        self.assertEqual(lyrics.validate_result(absent, TARGET), absent)


if __name__ == '__main__':
    unittest.main()
