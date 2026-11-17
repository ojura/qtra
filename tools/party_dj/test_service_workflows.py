"""Silent integration coverage for reusable DJ and guest workflows."""
from __future__ import annotations

import copy
import io
import json
from pathlib import Path
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from urllib.parse import urlencode

import server
from spotify_native import SpotifyError
from test_karaoke import sheet as karaoke_sheet
from test_server import FakeSpotify, observation, track, TRACK_A, TRACK_B


class WorkflowSpotify(FakeSpotify):
    def __init__(self):
        super().__init__()
        self.current = observation(server.now_ms(), paused=True)['nowPlaying']
        self.queue = [{**track(TRACK_A), 'uid': 'first'}, {**track(TRACK_B), 'uid': 'second'}]
        self.reorder_calls = []
        self.set_paused_calls = []

    def status(self):
        value = observation(server.now_ms(), paused=True)
        value['nowPlaying'] = copy.deepcopy(self.current)
        value['nowPlaying']['observedAt'] = server.now_ms()
        value['queue'] = copy.deepcopy(self.queue)
        value['playbackState'].update(paused=self.current.get('paused', False),
                                      buffering=self.current.get('buffering', False))
        return value

    def reorder_queue(self, ordered, expected, current, device):
        self.reorder_calls.append((ordered, expected, current, device))
        if expected != [item['uid'] for item in self.queue] or current != self.current['uid'] or device != 'fixture-device':
            raise SpotifyError('Concurrent change', dispatched=False)
        mapping = {item['uid']: item for item in self.queue}
        self.queue = [mapping[uid] for uid in ordered]
        return {'ok': True, 'moved': ordered != expected, 'status': self.status()}

    def set_paused(self, paused, current, device):
        self.set_paused_calls.append((paused, current, device))
        if current != self.current['uid'] or device != 'fixture-device':
            raise SpotifyError('The current occurrence or device changed.', dispatched=False)
        changed = self.current.get('paused') is not paused
        self.current.update(paused=paused, playing=not paused, buffering=False)
        return {'ok': True, 'action': 'setPaused', 'paused': paused, 'changed': changed,
                'device': {'id': device}, 'status': self.status()}


class ServiceWorkflows(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='virtual-dj-workflow-')
        self.addCleanup(self.directory.cleanup)
        self.stamp = 1800000000000
        clock = patch.object(server, 'now_ms', side_effect=lambda: self.stamp)
        clock.start(); self.addCleanup(clock.stop)
        monotonic = patch.object(server, 'monotonic_ms', side_effect=lambda: self.stamp)
        monotonic.start(); self.addCleanup(monotonic.stop)
        # These fixtures do not discover a LAN address, generate QR images,
        # create a real Spotify client or bind an HTTP socket.
        for isolated in (patch.object(server.Party, '_lan_url', return_value='http://fixture.invalid:8787'),
                         patch.object(server.Party, '_write_qr', return_value=None)):
            isolated.start(); self.addCleanup(isolated.stop)
        self.client = WorkflowSpotify()
        self.party = server.Party(Path(self.directory.name), client=self.client)
        self.addCleanup(self.party.db.close)

    def message(self, text='Edit this request', guest='fixture-guest'):
        return self.party.message({'guestId': guest, 'text': text})['message']

    def http(self, path, body=None, *, authorized=False, operator=None, peer=('203.0.113.7', 41001)):
        """Exercise actual request parsing and Handler methods using memory only."""
        class MemoryHandler(server.Handler):
            def log_message(self, *args):
                pass
        handler = object.__new__(MemoryHandler)
        handler.server = SimpleNamespace(party=self.party)
        handler.client_address = peer
        data = b'' if body is None else json.dumps(body).encode('utf-8')
        headers = [f'{"GET" if body is None else "POST"} {path} HTTP/1.1', 'Host: fixture.invalid',
                   'Connection: close', f'Content-Length: {len(data)}', 'Content-Type: application/json']
        if authorized:
            headers.append('Authorization: Bearer ' + self.party.admin_token)
        if operator is not None:
            headers.append('X-DJ-Operator: ' + operator)
        handler.rfile = io.BytesIO(('\r\n'.join(headers) + '\r\n\r\n').encode('ascii') + data)
        handler.wfile = io.BytesIO()
        handler.close_connection = True
        handler.handle_one_request()
        head, payload = handler.wfile.getvalue().split(b'\r\n\r\n', 1)
        return int(head.split(b' ', 2)[1]), json.loads(payload)

    def command_events(self):
        return [json.loads(row[0]) for row in self.party.db.execute(
            "SELECT payload FROM events WHERE kind='command' ORDER BY id")]

    def described(self, identifier='description-evidence'):
        self.party.describe_search({'guestId': 'fixture-guest', 'requestId': identifier,
                                    'text': 'A remembered title or meaning'})
        return identifier

    def presence_body(self, session='fixture-agent', sequence=1, state='online', **extra):
        return {'agentSessionId': session, 'sequence': sequence, 'state': state, **extra}

    def parallel(self, *calls):
        barrier = threading.Barrier(len(calls))
        results = [None] * len(calls)
        def run(index, call):
            try:
                barrier.wait(3)
                results[index] = call()
            except BaseException as error:
                results[index] = error
        workers = [threading.Thread(target=run, args=(index, call), daemon=True)
                   for index, call in enumerate(calls)]
        try:
            for worker in workers:
                worker.start()
            for worker in workers:
                worker.join(3)
        finally:
            barrier.abort()
            for worker in workers:
                if worker.is_alive():
                    worker.join(3)
        self.assertFalse(any(worker.is_alive() for worker in workers), 'Fixture workers did not stop.')
        for result in results:
            if isinstance(result, BaseException):
                raise result
        return results

    def test_presence_defaults_offline_and_reader_admin_activity_cannot_advertise_llm(self):
        initial = {'status': 'offline', 'lastSeen': 0, 'expiresAt': 0, 'remainingMs': 0, 'activeCount': 0}
        self.assertEqual(self.party.snapshot()['dj']['llm'], initial)
        sent = self.message()
        for path in ('/health', '/api/state', '/api/chat'):
            self.assertEqual(self.http(path)[0], 200)
        with patch.object(self.client, 'search', create=True, return_value=[]) as direct:
            self.assertEqual(self.http('/api/search?q=Fixture')[0], 200)
        direct.assert_called_once_with('Fixture')
        self.party.admin('heartbeat', {'readThrough': sent['id'], **self.presence_body()})
        self.party.admin('work', {'messageIds': [sent['id']], 'state': 'waiting', 'summary': 'Waiting for fixture input.'})
        self.party.admin('configure', {'bookEnabled': False})
        self.party.admin('reply', {'text': 'A fixture reply.'})
        self.assertEqual(self.party.snapshot()['dj']['watcher']['status'], 'away')
        self.assertEqual(self.http('/api/admin/events?after=-1', authorized=True)[0], 200)
        dj = self.party.chat()['dj']
        self.assertEqual(dj['llm'], initial)
        self.assertEqual((dj['status'], dj['lastSeen']), ('away', 0))
        self.assertEqual(dj['watcher']['status'], 'polling')
        self.assertEqual(self.client.control_calls, [])
        self.assertEqual(self.client.enqueue_calls, [])

    def test_presence_http_publishes_explicit_lease_and_expires_without_renewal(self):
        issued_at = self.stamp
        status, result = self.http('/api/admin/presence', self.presence_body(), authorized=True)
        self.assertEqual(status, 200)
        expected = {'status': 'online', 'lastSeen': issued_at, 'expiresAt': issued_at + 60000,
                    'remainingMs': 60000, 'activeCount': 1}
        self.assertEqual(result['dj']['llm'], expected)
        self.assertEqual((result['dj']['status'], result['dj']['lastSeen']), ('listening', issued_at))
        published = json.loads((self.party.data / 'state.json').read_text())
        self.assertEqual(published['dj']['llm'], expected)
        self.assertNotIn('fixture-agent', json.dumps(published))
        self.stamp += 30000
        self.party.admin('heartbeat', {})
        self.party.events(after=-1)
        self.assertEqual(self.http('/api/state')[0], 200)
        halfway = self.party.chat()['dj']['llm']
        self.assertEqual(halfway, {**expected, 'remainingMs': 30000})
        self.stamp += 30000
        expired = self.party.snapshot()['dj']
        self.assertEqual(expired['llm'], {'status': 'offline', 'lastSeen': issued_at,
            'expiresAt': 0, 'remainingMs': 0, 'activeCount': 0})
        self.assertEqual((expired['status'], expired['lastSeen']), ('away', issued_at))
        self.assertEqual(expired['watcher']['status'], 'polling')
        self.assertEqual(self.client.control_calls, [])
        self.assertEqual(self.client.enqueue_calls, [])

    def test_presence_identical_retries_never_extend_even_after_expiry(self):
        body = self.presence_body()
        self.party.admin('presence', body)
        original = copy.deepcopy(self.party.agent_presence)
        revision = self.party.revision
        published = (self.party.data / 'state.json').read_bytes()
        self.stamp += 15000
        response = self.party.admin('presence', {**body, 'leaseSeconds': 60})
        self.assertEqual(response['dj']['llm']['remainingMs'], 45000)
        self.assertEqual(self.party.agent_presence, original)
        self.assertEqual((self.party.revision, (self.party.data / 'state.json').read_bytes()), (revision, published))
        self.stamp += 45000
        response = self.party.admin('presence', body)
        self.assertEqual(response['dj']['llm']['status'], 'offline')
        self.assertEqual(response['dj']['llm']['remainingMs'], 0)
        self.assertEqual(self.party.agent_presence, original)
        self.assertEqual((self.party.revision, (self.party.data / 'state.json').read_bytes()), (revision, published))

    def test_presence_sequence_conflicts_and_invalid_schema_are_atomic(self):
        sent = self.message()
        self.party.admin('presence', self.presence_body(sequence=2))
        valid = self.presence_body(sequence=3)
        invalid = [dict(valid, agentSessionId=value) for value in (None, '', 'a b', 'a\nb', 'x' * 129)]
        invalid += [dict(valid, sequence=value) for value in (None, True, 0, -1, 2**53, 3.0, '3')]
        invalid += [dict(valid, state=value) for value in (None, False, 'listening', {}, [])]
        invalid += [dict(valid, leaseSeconds=value) for value in (None, False, 29, 121, 60.0, '60')]
        invalid += [dict(valid, readThrough=sent['id']), dict(valid, source='api'),
                    self.presence_body(sequence=1), self.presence_body(sequence=2, state='offline'),
                    self.presence_body(sequence=2, leaseSeconds=30)]
        def state():
            return (copy.deepcopy(self.party.agent_presence), self.party.last_seen,
                    self.party.watcher_last_seen, self.party.revision,
                    (self.party.data / 'state.json').read_bytes(),
                    [tuple(row) for row in self.party.db.execute('SELECT * FROM settings ORDER BY key')],
                    [tuple(row) for row in self.party.db.execute('SELECT * FROM events ORDER BY id')],
                    self.party.message_info(sent['id']))
        before = state()
        self.assertEqual(self.http('/api/admin/presence', valid)[0], 401)
        self.assertEqual(state(), before)
        for body in invalid:
            with self.subTest(body=body):
                self.assertEqual(self.http('/api/admin/presence', body, authorized=True)[0], 400)
                self.assertEqual(state(), before)
        self.assertEqual(self.client.control_calls, [])
        self.assertEqual(self.client.enqueue_calls, [])

    def test_presence_newer_offline_update_rejects_late_renewals(self):
        self.party.admin('presence', self.presence_body())
        self.stamp += 1000
        offline = self.presence_body(sequence=3, state='offline')
        result = self.party.admin('presence', offline)['dj']
        self.assertEqual(result['llm'], {'status': 'offline', 'lastSeen': self.stamp,
            'expiresAt': 0, 'remainingMs': 0, 'activeCount': 0})
        record = copy.deepcopy(self.party.agent_presence)
        for sequence in (1, 2):
            with self.assertRaises(ValueError):
                self.party.admin('presence', self.presence_body(sequence=sequence))
        self.stamp += 1000
        self.party.admin('presence', offline)
        self.assertEqual(self.party.agent_presence, record)
        renewed = self.party.admin('presence', self.presence_body(sequence=4))['dj']['llm']
        self.assertEqual((renewed['status'], renewed['lastSeen'], renewed['remainingMs']), ('online', self.stamp, 60000))

    def test_presence_multiple_sessions_keep_independent_leases_and_honest_projection(self):
        started = self.stamp
        self.party.admin('presence', self.presence_body('first-agent', leaseSeconds=120))
        self.stamp += 10000
        result = self.party.admin('presence', self.presence_body('second-agent', leaseSeconds=30))['dj']['llm']
        self.assertEqual(result, {'status': 'online', 'lastSeen': self.stamp,
            'expiresAt': started + 120000, 'remainingMs': 110000, 'activeCount': 2})
        self.stamp += 5000
        remaining = self.party.admin('presence', self.presence_body('second-agent', 2, 'offline'))['dj']['llm']
        self.assertEqual(remaining, {'status': 'online', 'lastSeen': started,
            'expiresAt': started + 120000, 'remainingMs': 105000, 'activeCount': 1})
        self.stamp = started + 120000
        self.assertEqual(self.party.snapshot()['dj']['llm']['activeCount'], 0)
        self.assertEqual(self.party.agent_presence['first-agent']['sequence'], 1)
        self.assertEqual(self.party.agent_presence['second-agent']['sequence'], 2)

    def test_presence_restart_clears_leases_but_preserves_message_work(self):
        sent = self.message()
        self.party.admin('work', {'messageIds': [sent['id']], 'state': 'waiting', 'summary': 'Waiting for a fixture.'})
        work = self.party.snapshot()['dj']['messageWork']
        self.party.admin('presence', self.presence_body())
        self.party.events(after=-1)
        restored = server.Party(self.party.data, client=self.client)
        self.addCleanup(restored.db.close)
        self.assertEqual(restored.agent_presence, {})
        dj = restored.snapshot()['dj']
        self.assertEqual(dj['llm'], {'status': 'offline', 'lastSeen': 0,
            'expiresAt': 0, 'remainingMs': 0, 'activeCount': 0})
        self.assertEqual(dj['watcher']['status'], 'away')
        self.assertEqual(dj['messageWork'], work)
        self.assertEqual(json.loads((restored.data / 'state.json').read_text())['dj']['llm'], dj['llm'])

    def test_presence_monotonic_expiry_and_future_wall_clock_are_safe(self):
        started = self.stamp
        clock = {'mono': started}
        with patch.object(server, 'monotonic_ms', side_effect=lambda: clock['mono']):
            self.party.admin('presence', self.presence_body())
            self.stamp = started - 1
            clock['mono'] += 1000
            future = self.party.snapshot()['dj']['llm']
            self.assertEqual((future['status'], future['expiresAt'], future['remainingMs'], future['activeCount']),
                             ('offline', 0, 0, 0))
            self.stamp = started + 5000
            clock['mono'] = started + 60000
            expired = self.party.admin('presence', self.presence_body())['dj']['llm']
            self.assertEqual((expired['status'], expired['remainingMs']), ('offline', 0))
            renewed = self.party.admin('presence', self.presence_body(sequence=2))['dj']['llm']
            self.assertEqual((renewed['status'], renewed['expiresAt'], renewed['remainingMs']),
                             ('online', self.stamp + 60000, 60000))
            self.stamp += 60000
            clock['mono'] += 1000
            wall_expired = self.party.snapshot()['dj']['llm']
            self.assertEqual((wall_expired['status'], wall_expired['expiresAt'], wall_expired['remainingMs']),
                             ('offline', 0, 0))

    def test_presence_concurrent_updates_keep_the_highest_sequence(self):
        def update(sequence, state):
            try:
                return self.party.admin('presence', self.presence_body(sequence=sequence, state=state))
            except ValueError:
                return {'rejected': sequence}
        results = self.parallel(lambda: update(2, 'online'), lambda: update(3, 'offline'))
        self.assertIn('dj', results[1])
        self.assertEqual(self.party.agent_presence['fixture-agent']['sequence'], 3)
        self.assertEqual(self.party.snapshot()['dj']['llm']['status'], 'offline')
        revision = self.party.revision
        self.stamp += 1000
        self.party.admin('presence', self.presence_body(sequence=3, state='offline'))
        self.assertEqual(self.party.revision, revision)

    def test_presence_session_capacity_retains_expired_replay_watermarks(self):
        with patch.object(server, 'AGENT_SESSION_LIMIT', 2):
            self.party.admin('presence', self.presence_body('first-agent', leaseSeconds=30))
            self.party.admin('presence', self.presence_body('second-agent', state='offline'))
            self.stamp += 30000
            self.assertEqual(self.party.snapshot()['dj']['llm']['status'], 'offline')
            before = (copy.deepcopy(self.party.agent_presence), self.party.revision)
            with self.assertRaises(ValueError):
                self.party.admin('presence', self.presence_body('third-agent'))
            self.assertEqual((self.party.agent_presence, self.party.revision), before)
            self.party.admin('presence', self.presence_body('first-agent', leaseSeconds=30))
            self.assertEqual(self.party.snapshot()['dj']['llm']['status'], 'offline')
            self.party.admin('presence', self.presence_body('first-agent', sequence=2))
            self.assertEqual(self.party.snapshot()['dj']['llm']['activeCount'], 1)
            with self.assertRaises(ValueError):
                self.party.admin('presence', self.presence_body('first-agent', leaseSeconds=30))
            self.assertEqual(len(self.party.agent_presence), 2)

    def test_watcher_presence_tracks_only_event_polling_and_expires_independently(self):
        self.party.admin('heartbeat', {})
        self.assertEqual(self.party.snapshot()['dj']['watcher']['status'], 'away')
        polled = self.stamp
        self.party.events(after=-1)
        watcher = self.party.snapshot()['dj']['watcher']
        self.assertEqual(watcher, {'status': 'polling', 'lastSeen': polled,
            'expiresAt': polled + 90000, 'remainingMs': 90000})
        self.stamp += 90000
        self.party.admin('heartbeat', {})
        self.party.chat()
        self.assertEqual(self.party.snapshot()['dj']['watcher'], {'status': 'away', 'lastSeen': polled,
            'expiresAt': 0, 'remainingMs': 0})
        self.party.admin('presence', self.presence_body())
        self.assertEqual(self.party.snapshot()['dj']['watcher']['status'], 'away')
        self.party.events(after=-1)
        self.assertEqual(self.party.snapshot()['dj']['watcher']['lastSeen'], self.stamp)
        self.assertEqual(self.party.snapshot()['dj']['llm']['activeCount'], 1)

    def test_search_id_success_keeps_manual_search_and_cache_compatible(self):
        tracks = [track(TRACK_A)]
        with patch.object(self.client, 'search', create=True, return_value=tracks) as direct:
            status, manual = self.http('/api/search?q=Manual')
            self.assertEqual((status, manual), (200, {'tracks': tracks}))
            direct.assert_called_once_with('Manual')
            self.assertEqual(self.party.search_requests, {})
            self.assertEqual(self.http('/api/search?q=Manual'), (200, {'tracks': tracks}))
            self.assertEqual(direct.call_count, 1)
            status, automatic = self.http('/api/search?q=Another&searchId=search-one')
            self.assertEqual((status, automatic), (200, {'tracks': tracks, 'cancelled': False, 'searchId': 'search-one'}))
            self.assertEqual(direct.call_args.args, ('Another',))
            self.assertIsInstance(direct.call_args.kwargs['cancel_event'], threading.Event)
            self.assertFalse(self.party.search_requests['search-one']['active'])
            self.assertNotIn('event', self.party.search_requests['search-one'])
        self.assertEqual(self.client.control_calls, [])
        self.assertEqual(self.client.enqueue_calls, [])

    def test_search_cancel_before_get_suppresses_cached_results_without_extending_retention(self):
        identifier = 'cancel-before'
        with patch.object(self.client, 'search', create=True, return_value=[track(TRACK_A)]) as direct:
            self.party.search('Cached')
            direct.reset_mock()
            expected = {'searchId': identifier, 'cancelRequested': True}
            self.assertEqual(self.http('/api/search/cancel', {'searchId': identifier}), (200, expected))
            expires = self.party.search_requests[identifier]['expiresAt']
            self.stamp += 10000
            self.assertEqual(self.http('/api/search/cancel', {'searchId': identifier}), (200, expected))
            self.assertEqual(self.party.search_requests[identifier]['expiresAt'], expires)
            result = self.http('/api/search?' + urlencode({'q': 'Cached', 'searchId': identifier}))
            self.assertEqual(result, (200, {'tracks': [], 'cancelled': True, 'searchId': identifier}))
            self.assertEqual(self.party.search_requests[identifier]['expiresAt'], expires)
            direct.assert_not_called()
        self.assertEqual(self.party.search_cache['cached'][1], [track(TRACK_A)])

    def test_search_inflight_cancel_suppresses_success_cancel_and_error_returns(self):
        for outcome in ('success', 'cancel', 'error'):
            with self.subTest(outcome=outcome):
                started, release = threading.Event(), threading.Event()
                identifier, query = 'inflight-' + outcome, 'Query ' + outcome
                results = []
                def search(value, *, cancel_event):
                    started.set()
                    self.assertTrue(release.wait(3))
                    self.assertTrue(cancel_event.is_set())
                    if outcome == 'cancel':
                        raise server.SpotifySearchCancelled('Fixture search cancelled.')
                    if outcome == 'error':
                        raise SpotifyError('Fixture provider error.', dispatched=False)
                    return [track(TRACK_A)]
                with patch.object(self.client, 'search', create=True, side_effect=search) as direct:
                    worker = threading.Thread(target=lambda: results.append(self.http('/api/search?' + urlencode({'q': query, 'searchId': identifier}))))
                    worker.start()
                    try:
                        self.assertTrue(started.wait(3))
                        self.assertEqual(self.http('/api/search/cancel', {'searchId': identifier}),
                            (200, {'searchId': identifier, 'cancelRequested': True}))
                    finally:
                        release.set(); worker.join(3)
                    self.assertFalse(worker.is_alive())
                    self.assertEqual(results, [(200, {'tracks': [], 'cancelled': True, 'searchId': identifier})])
                    direct.assert_called_once()
                self.assertNotIn(query.casefold(), self.party.search_cache)
                self.assertNotIn(TRACK_A, self.party.track_cache)
                self.assertFalse(self.party.search_requests[identifier]['active'])
                self.assertNotIn('event', self.party.search_requests[identifier])

    def test_search_old_id_cancellation_cannot_cancel_a_newer_same_query(self):
        arrived, release = threading.Barrier(3), threading.Event()
        results = {}
        identifiers = ('older-search', 'newer-search')
        def search(value, *, cancel_event):
            arrived.wait(3)
            self.assertTrue(release.wait(3))
            if cancel_event.is_set():
                raise server.SpotifySearchCancelled('Fixture search cancelled.')
            return [track(TRACK_A)]
        def run(identifier):
            results[identifier] = self.http('/api/search?' + urlencode({'q': 'Same', 'searchId': identifier}))
        with patch.object(self.client, 'search', create=True, side_effect=search) as direct:
            workers = [threading.Thread(target=run, args=(identifier,)) for identifier in identifiers]
            for worker in workers:
                worker.start()
            crossed = False
            try:
                arrived.wait(3)
                crossed = True
                self.assertEqual(self.http('/api/search/cancel', {'searchId': identifiers[0]})[0], 200)
                with self.party.lock:
                    older = self.party.search_requests[identifiers[0]]['event']
                    newer = self.party.search_requests[identifiers[1]]['event']
                    self.assertIsNot(older, newer)
                    self.assertTrue(older.is_set())
                    self.assertFalse(newer.is_set())
            finally:
                if not crossed:
                    arrived.abort()
                release.set()
                for worker in workers:
                    worker.join(3)
            self.assertFalse(any(worker.is_alive() for worker in workers))
            self.assertEqual(direct.call_count, 2)
        self.assertEqual(results[identifiers[0]], (200, {'tracks': [], 'cancelled': True, 'searchId': identifiers[0]}))
        self.assertEqual(results[identifiers[1]], (200, {'tracks': [track(TRACK_A)], 'cancelled': False, 'searchId': identifiers[1]}))
        self.assertEqual(self.party.search_cache['same'][1], [track(TRACK_A)])

    def test_search_duplicate_active_and_completed_ids_never_dispatch_twice(self):
        started, release = threading.Event(), threading.Event()
        identifier = 'single-search'
        results = []
        def search(value, *, cancel_event):
            started.set()
            self.assertTrue(release.wait(3))
            return [track(TRACK_A)]
        with patch.object(self.client, 'search', create=True, side_effect=search) as direct:
            worker = threading.Thread(target=lambda: results.append(self.http('/api/search?q=First&searchId=' + identifier)))
            worker.start()
            try:
                self.assertTrue(started.wait(3))
                duplicate = self.http('/api/search?q=Different&searchId=' + identifier)
                self.assertEqual(duplicate, (409, {'tracks': [], 'searchId': identifier, 'error': 'This searchId has already been used.'}))
            finally:
                release.set(); worker.join(3)
            self.assertFalse(worker.is_alive())
            self.assertEqual(results[0][0], 200)
            self.assertEqual(self.http('/api/search?q=First&searchId=' + identifier), duplicate)
            direct.assert_called_once()

    def test_search_capacity_preserves_known_cancellation_and_protected_tombstones(self):
        started, release = threading.Event(), threading.Event()
        results = []
        def search(value, *, cancel_event=None):
            started.set()
            self.assertTrue(release.wait(3))
            return [track(TRACK_A)]
        with patch.object(server, 'SEARCH_RETAINED_LIMIT', 4), patch.object(server, 'SEARCH_ACTIVE_LIMIT', 1), \
             patch.object(self.client, 'search', create=True, side_effect=search) as direct:
            worker = threading.Thread(target=lambda: results.append(self.http('/api/search?q=Busy&searchId=active-search')))
            worker.start()
            try:
                self.assertTrue(started.wait(3))
                status, busy = self.http('/api/search?q=New&searchId=blocked-search')
                self.assertEqual(status, 429)
                self.assertEqual(busy['searchId'], 'blocked-search')
                self.assertNotIn('blocked-search', self.party.search_requests)
                for identifier in ('before-one', 'before-two', 'before-three'):
                    self.assertEqual(self.http('/api/search/cancel', {'searchId': identifier})[0], 200)
                self.assertEqual(len(self.party.search_requests), 4)
                self.assertEqual(self.http('/api/search/cancel', {'searchId': 'unseen-search'})[0], 429)
                self.assertEqual(self.http('/api/search/cancel', {'searchId': 'active-search'})[0], 200)
            finally:
                release.set(); worker.join(3)
            self.assertFalse(worker.is_alive())
            self.assertEqual(results, [(200, {'tracks': [], 'cancelled': True, 'searchId': 'active-search'})])
            self.assertEqual(self.http('/api/search?q=Before&searchId=before-one'),
                             (200, {'tracks': [], 'cancelled': True, 'searchId': 'before-one'}))
            self.assertEqual(self.http('/api/search?q=Manual'), (200, {'tracks': [track(TRACK_A)]}))
            self.assertEqual(len(self.party.search_requests), 4)
            self.stamp += 120000
            self.assertEqual(self.http('/api/search?q=After&searchId=after-retention')[0], 200)
            self.assertEqual(set(self.party.search_requests), {'after-retention'})
            self.assertEqual(direct.call_count, 3)

    def test_search_tombstone_retention_uses_monotonic_clock_and_cancel_is_idempotent(self):
        clock = {'mono': 1000}
        with patch.object(server, 'monotonic_ms', side_effect=lambda: clock['mono']):
            self.party.cancel_search({'searchId': 'clock-search'})
            self.assertEqual(self.party.search_requests['clock-search']['expiresAt'], 121000)
            self.stamp += 10**9
            clock['mono'] = 120999
            self.party.cancel_search({'searchId': 'clock-search'})
            self.assertEqual(self.party.search_requests['clock-search']['expiresAt'], 121000)
            with patch.object(self.client, 'search', create=True, return_value=[track(TRACK_A)]) as direct:
                self.assertEqual(self.party.search('Old', search_id='clock-search')['cancelled'], True)
                direct.assert_not_called()
                clock['mono'] = 121000
                self.party.search('New', search_id='fresh-search')
                direct.assert_called_once()
            self.assertNotIn('clock-search', self.party.search_requests)

    def test_search_identifier_and_cancel_schema_validation_precede_all_effects(self):
        self.party.search_cache['kept'] = (0, [track(TRACK_B)])
        self.party.track_cache[TRACK_B] = track(TRACK_B)
        def state():
            return (copy.deepcopy(self.party.search_requests), copy.deepcopy(self.party.search_cache),
                    copy.deepcopy(self.party.track_cache), self.party.revision, self.party.last_seen,
                    (self.party.data / 'state.json').read_bytes())
        before = state()
        invalid = ('', 'short', 'with space', 'bad\nname', 'čudno-ime', 'x' * 129)
        with patch.object(self.client, 'search', create=True, return_value=[]) as direct:
            for identifier in invalid:
                with self.subTest(identifier=identifier):
                    path = '/api/search?' + urlencode({'q': 'Query', 'searchId': identifier})
                    self.assertEqual(self.http(path)[0], 400)
                    self.assertEqual(self.http('/api/search/cancel', {'searchId': identifier})[0], 400)
                    self.assertEqual(state(), before)
            for body in ({}, [], {'searchId': None}, {'searchId': False}, {'searchId': 12345678},
                         {'searchId': 'valid-search', 'text': 'Unfinished draft'}):
                self.assertEqual(self.http('/api/search/cancel', body)[0], 400)
                self.assertEqual(state(), before)
            self.assertEqual(self.http('/api/search?q=Query&searchId=first-search&searchId=second-search')[0], 400)
            with self.assertRaises(ValueError):
                self.party.search(42, search_id='valid-search')
            self.assertEqual(state(), before)
            direct.assert_not_called()

    def test_search_provider_failures_retire_ids_without_partial_cache_or_redispatch(self):
        self.party.track_cache[TRACK_B] = track(TRACK_B)
        self.party.search_cache['kept'] = (0, [track(TRACK_B)])
        before = (copy.deepcopy(self.party.track_cache), copy.deepcopy(self.party.search_cache))
        outcomes = [SpotifyError('Fixture provider failed.', dispatched=False), None, [{}],
                    [{'uri': []}], [track(TRACK_A), {}], [{'uri': 'not-a-track'}]]
        with patch.object(server, 'SEARCH_ACTIVE_LIMIT', 1):
            for index, outcome in enumerate(outcomes):
                identifier = 'failed-search-' + str(index)
                options = {'side_effect': outcome} if isinstance(outcome, Exception) else {'return_value': outcome}
                with self.subTest(outcome=outcome), patch.object(self.client, 'search', create=True, **options) as direct:
                    path = '/api/search?' + urlencode({'q': 'Failure ' + str(index), 'searchId': identifier})
                    self.assertEqual(self.http(path)[0], 503)
                    self.assertEqual((self.party.track_cache, self.party.search_cache), before)
                    record = self.party.search_requests[identifier]
                    self.assertFalse(record['active'])
                    self.assertNotIn('event', record)
                    self.assertEqual(record['expiresAt'], self.stamp + 120000)
                    self.assertEqual(self.http(path)[0], 409)
                    direct.assert_called_once()

    def test_search_dedicated_transport_cancellation_is_a_successful_cancelled_response(self):
        error = server.SpotifySearchCancelled('Fixture search cancelled.')
        self.assertFalse(error.dispatched)
        with patch.object(self.client, 'search', create=True, side_effect=error) as direct:
            self.assertEqual(self.http('/api/search?q=Cancelled&searchId=transport-cancel'),
                             (200, {'tracks': [], 'cancelled': True, 'searchId': 'transport-cancel'}))
            direct.assert_called_once()
        self.assertFalse(self.party.search_requests['transport-cancel']['active'])
        self.assertTrue(self.party.search_requests['transport-cancel']['cancelled'])
        self.assertEqual(self.party.search_cache, {})
        self.assertEqual(self.party.track_cache, {})

    def test_sync_exposes_only_actual_observation_and_history_without_transport(self):
        self.party.request({'requestId': 'pending-request', 'uri': TRACK_A})
        result = self.party.admin('sync', {})
        self.assertEqual([item['uid'] for item in result['observation']['queue']], ['first', 'second'])
        self.assertEqual(result['state']['spotify']['ageMs'], 0)
        self.assertFalse(result['state']['spotify']['stale'])
        self.assertEqual(result['recentTracks'][0]['uid'], self.client.current['uid'])
        self.assertEqual(self.client.control_calls, [])
        self.assertEqual(self.client.enqueue_calls, [])
        self.stamp += 10001
        self.assertTrue(self.party.snapshot()['spotify']['stale'])

    def test_sync_failure_publishes_disconnection_instead_of_old_track(self):
        self.party.admin('sync', {})
        with patch.object(self.client, 'status', side_effect=SpotifyError('No actual player')):
            with self.assertRaises(SpotifyError) as caught:
                self.party.admin('sync', {})
        self.assertFalse(caught.exception.dispatched)
        state = json.loads((self.party.data / 'state.json').read_text())
        self.assertIsNone(state['nowPlaying'])
        self.assertFalse(state['spotify']['connected'])
        self.assertTrue(state['spotify']['stale'])
        self.assertEqual(self.client.control_calls, [])

    def test_sync_generation_prevents_an_older_poll_overwriting_the_result(self):
        started, release = threading.Event(), threading.Event()
        old = self.client.status()
        fresh = copy.deepcopy(old)
        fresh['nowPlaying']['uid'] = 'fresh-occurrence'
        calls = []
        def status():
            calls.append(1)
            if len(calls) == 1:
                started.set()
                self.assertTrue(release.wait(3))
                return old
            return fresh
        with patch.object(self.client, 'status', side_effect=status):
            worker = threading.Thread(target=self.party._refresh)
            worker.start()
            self.assertTrue(started.wait(3))
            try:
                self.party.admin('sync', {})
            finally:
                release.set(); worker.join(3)
        self.assertFalse(worker.is_alive())
        self.assertEqual(self.party.snapshot()['nowPlaying']['uid'], 'fresh-occurrence')

    def reorder(self):
        return {'orderedQueuedUids': ['second', 'first'], 'expectedQueuedUids': ['first', 'second'],
                'expectedCurrentUid': self.client.current['uid'], 'expectedDeviceId': 'fixture-device'}

    def test_reorder_publishes_verified_order_and_preserves_paused_player(self):
        result = self.party.admin('reorder', self.reorder())
        self.assertTrue(result['ok'])
        published = json.loads((self.party.data / 'state.json').read_text())
        self.assertEqual([item['uid'] for item in published['queue']], ['second', 'first'])
        self.assertTrue(published['nowPlaying']['paused'])
        self.assertEqual(len(self.client.reorder_calls), 1)
        outcome = self.command_events()[-1]
        self.assertEqual((outcome['dispatched'], outcome['dispatchUncertain']), (True, False))
        self.assertEqual(self.client.control_calls, [])
        self.assertEqual(self.client.enqueue_calls, [])

    def test_reorder_validates_before_receipts_and_never_retries_a_race(self):
        message = self.message()
        before = (self.party.last_seen, self.party.revision, self.party._control_generation)
        for change in ({'orderedQueuedUids': ['first', 'first']}, {'expectedQueuedUids': None},
                       {'expectedCurrentUid': ''}, {'expectedDeviceId': False}, {'orderedQueuedUids': []}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.party.admin('reorder', {**self.reorder(), **change, 'readThrough': message['id']})
            self.assertEqual((self.party.last_seen, self.party.revision, self.party._control_generation), before)
        self.assertEqual(self.party.chat()['messages'][0]['status'], 'delivered')
        payload = self.reorder()
        self.client.queue.reverse()
        with self.assertRaises(SpotifyError) as caught:
            self.party.admin('reorder', payload)
        self.assertFalse(caught.exception.dispatched)
        self.assertEqual(len(self.client.reorder_calls), 1)

    def test_recall_returns_text_once_and_persists_tombstone(self):
        sent = self.message('Original text\nwith a second line')
        body = {'guestId': 'fixture-guest', 'messageId': sent['id']}
        result = self.party.recall_message(body)
        self.assertEqual(result['recalled']['text'], sent['text'])
        self.assertEqual(self.party.recall_message(body), result)
        self.assertEqual(self.party.chat()['messages'], [])
        self.assertEqual(self.party.chat()['withdrawnMessageIds'], [sent['id']])
        self.assertEqual(self.party.db.execute("SELECT COUNT(*) FROM events WHERE kind='message_recalled'").fetchone()[0], 1)
        restored = server.Party(self.party.data, client=self.client)
        self.addCleanup(restored.db.close)
        self.assertEqual(restored.chat()['withdrawnMessageIds'], [sent['id']])
        with self.assertRaises(ValueError):
            self.party.admin('processing', {'messageIds': [sent['id']], 'active': True})

    def test_recall_checks_guest_latest_message_and_processing_acceptance(self):
        first = self.message('First')
        last = self.message('Second')
        for body in ({'guestId': 'different-guest', 'messageId': last['id']},
                     {'guestId': 'fixture-guest', 'messageId': first['id']}):
            with self.assertRaises(ValueError):
                self.party.recall_message(body)
        self.party.admin('processing', {'messageIds': [last['id']], 'active': True})
        self.party.admin('processing', {'messageIds': [last['id']], 'active': False})
        with self.assertRaises(ValueError):
            self.party.recall_message({'guestId': 'fixture-guest', 'messageId': last['id']})
        self.assertEqual(len(self.party.chat()['messages']), 2)

    def test_read_receipt_alone_preserves_recall_eligibility(self):
        sent = self.message()
        self.party.admin('heartbeat', {'readThrough': sent['id']})
        self.assertIsNotNone(self.party.message_info(sent['id'])['message']['readAt'])
        recalled = self.party.recall_message({'guestId': 'fixture-guest', 'messageId': sent['id']})
        self.assertEqual(recalled['recalled']['text'], sent['text'])
        self.assertEqual(self.party.chat()['withdrawnMessageIds'], [sent['id']])

    def test_description_resolves_to_selectable_metadata_without_enqueue(self):
        body = {'guestId': 'fixture-guest', 'requestId': 'description-001', 'text': 'The Bebek song about a sleepless night in a kafana'}
        pending = self.party.describe_search(body)
        self.assertEqual(self.party.describe_search(body), pending)
        self.assertEqual(pending['search']['status'], 'pending')
        resolved = self.party.admin('resolve-search', {'id': body['requestId'], 'uris': [TRACK_A], 'note': 'Album recording'})
        self.assertEqual(resolved['search']['status'], 'resolved')
        self.assertEqual(resolved['search']['tracks'][0]['uri'], TRACK_A)
        self.assertNotIn('match', resolved['search']['tracks'][0])
        self.assertEqual(self.party.description_search(body['requestId']), resolved)
        self.assertEqual(self.client.enqueue_calls, [])
        self.assertEqual(self.client.control_calls, [])
        self.assertEqual(self.party.db.execute("SELECT COUNT(*) FROM events WHERE kind='search_description'").fetchone()[0], 1)
        with self.assertRaises(ValueError):
            self.party.describe_search({**body, 'text': 'Different description'})

    def test_exact_search_does_not_enter_the_description_workflow(self):
        with patch.object(self.client, 'search', create=True, return_value=[track(TRACK_A)]) as direct:
            result = self.party.search('Bebek')
        direct.assert_called_once_with('Bebek')
        self.assertEqual(result['tracks'][0]['uri'], TRACK_A)
        self.assertEqual(self.party.db.execute('SELECT COUNT(*) FROM search_descriptions').fetchone()[0], 0)

    def test_song_visual_validation_is_atomic_and_persists_without_act_reset(self):
        anchor = self.party.scene['audioActStartedAt']
        valid = {'trackUid': 'queued-occurrence', 'theme': 'bottles'}
        result = self.party.admin('scene', {'songVisual': valid})
        self.assertEqual(result['scene']['songVisual'], valid)
        self.assertEqual(result['scene']['audioActStartedAt'], anchor)
        before = (self.party.revision, self.party.last_seen, copy.deepcopy(self.party.scene))
        for bad in ('bottles', {'theme': 'bottles'}, {'trackUid': '', 'theme': 'bottles'},
                    {'trackUid': 'x', 'theme': 'unknown'}, {'trackUid': 'x', 'theme': 'bottles', 'extra': 1}):
            with self.assertRaises(ValueError):
                self.party.admin('scene', {'songVisual': bad})
            self.assertEqual((self.party.revision, self.party.last_seen, self.party.scene), before)
        self.party.admin('scene', {'songVisual': None})
        self.assertIsNone(self.party.scene['songVisual'])

    def test_watch_can_start_at_current_cursor_without_replaying_the_party(self):
        self.message()
        result = self.party.events(after=-1)
        self.assertEqual(result['events'], [])
        self.assertGreater(result['lastId'], 0)
        sent = self.message('New instruction')
        event = self.party.events(after=result['lastId'])['events'][0]
        self.assertEqual(event['payload']['id'], sent['id'])

    def test_nested_match_evidence_survives_poll_and_restart_without_mutating_metadata(self):
        identifier = self.described()
        original = track(TRACK_A)
        self.party.track_cache[TRACK_A] = original
        matches = [
            {'uri': TRACK_A, 'basis': 'literal_lyrics', 'rationale': 'Supplied words match the cited source.',
             'confidence': 0, 'sources': [{'url': 'https://publisher.invalid/song', 'label': 'Publisher'}]},
            {'uri': TRACK_B, 'basis': 'semantic', 'rationale': 'An alternative meaning.', 'sources': []},
        ]
        status, resolved = self.http('/api/admin/resolve-search',
            {'id': identifier, 'uris': [TRACK_A, TRACK_B], 'note': 'Choose the intended version.', 'matches': matches},
            authorized=True)
        self.assertEqual(status, 200)
        self.assertEqual(resolved['search']['note'], 'Choose the intended version.')
        for candidate, supplied in zip(resolved['search']['tracks'], matches):
            self.assertEqual(candidate['match'], {key: value for key, value in supplied.items() if key != 'uri'})
            self.assertNotIn('matchBasis', candidate)
            self.assertNotIn('rationale', candidate)
        self.assertNotIn('match', original)
        self.assertEqual(self.http('/api/search/describe?id=' + identifier), (200, resolved))
        reopened = server.Party(self.party.data, client=self.client)
        self.addCleanup(reopened.db.close)
        self.assertEqual(reopened.description_search(identifier), resolved)
        self.assertEqual(self.client.enqueue_calls, [])
        self.assertEqual(self.client.control_calls, [])

    def test_match_validation_is_atomic_before_receipts_and_metadata_fetch(self):
        identifier = self.described()
        sent = self.message()
        good = {'uri': TRACK_A, 'basis': 'title', 'rationale': 'Exact title.', 'sources': []}
        link = {'url': 'https://publisher.invalid/song', 'label': 'Publisher'}
        invalid = [None, {}, [good] * 9, [good, good], [{**good, 'uri': TRACK_B}],
                   [{**good, 'basis': 'guess'}], [{**good, 'basis': None}],
                   [{**good, 'rationale': ''}], [{**good, 'rationale': 'x' * 501}],
                   [{**good, 'rationale': 3}], [{**good, 'unexpected': True}],
                   [{**good, 'sources': {}}], [{**good, 'sources': [link] * 9}]]
        invalid += [[{**good, 'confidence': value}] for value in (True, None, '0.5', -0.01, 1.01, float('nan'), float('inf'))]
        invalid += [[{**good, 'sources': [{**link, 'url': url}]}] for url in (
            'javascript:alert(1)', 'file:///tmp/words', 'https:///missing-host',
            'https://name:secret@publisher.invalid/song', 'https://publisher.invalid:bad/song',
            'https://publisher.invalid/space here', 'https://publisher.invalid/' + 'x' * 2048)]
        invalid += [[{**good, 'sources': [{**link, 'label': value}]}] for value in ('', 'x' * 121, 5)]
        before = (self.party.last_seen, self.party.revision, self.party.description_search(identifier),
                  (self.party.data / 'state.json').read_bytes())
        with patch.object(self.client, 'track', wraps=self.client.track) as metadata:
            for matches in invalid:
                with self.subTest(matches=matches):
                    status, unused = self.http('/api/admin/resolve-search',
                        {'id': identifier, 'uris': [TRACK_A], 'matches': matches, 'readThrough': sent['id']},
                        authorized=True)
                    self.assertEqual(status, 400)
                    self.assertEqual((self.party.last_seen, self.party.revision, self.party.description_search(identifier),
                                      (self.party.data / 'state.json').read_bytes()), before)
                    self.assertIsNone(self.party.message_info(sent['id'])['message']['readAt'])
            metadata.assert_not_called()
        self.assertEqual(self.party.db.execute("SELECT COUNT(*) FROM events WHERE kind='search_resolved'").fetchone()[0], 0)

    def test_partial_match_evidence_and_note_only_resolution_remain_compatible(self):
        identifier = self.described()
        resolved = self.party.admin('resolve-search', {'id': identifier, 'uris': [TRACK_A, TRACK_B],
            'matches': [{'uri': TRACK_B, 'basis': 'title', 'rationale': 'Točan naslov.', 'confidence': 1}]})
        self.assertNotIn('match', resolved['search']['tracks'][0])
        self.assertEqual(resolved['search']['tracks'][1]['match'],
                         {'basis': 'title', 'rationale': 'Točan naslov.', 'confidence': 1, 'sources': []})
        clarified = self.party.admin('resolve-search', {'id': identifier, 'uris': [], 'note': 'Which performer?', 'matches': []})
        self.assertEqual(clarified['search']['tracks'], [])
        self.assertEqual(clarified['search']['note'], 'Which performer?')

    def test_message_origin_is_private_and_authenticated_info_uses_actual_http_peer(self):
        body = {'requestId': 'private-origin-message', 'guestId': 'private-guest-identity', 'source': 'native',
                'text': 'An ordinary request.', 'operatorLabel': 'forged body label',
                'httpPeer': {'address': 'forged.invalid', 'port': 9}}
        peer = ('198.51.100.42', 47123)
        status, sent = self.http('/api/message', body, operator='Declared operator', peer=peer)
        self.assertEqual(status, 200)
        item = sent['message']
        self.assertEqual((item['source'], item['sourceDeclared'], item['isTest']), ('native', True, False))
        self.assertIs(type(item['sourceDeclared']), bool)
        self.assertIs(type(item['isTest']), bool)
        path = '/api/admin/message-info?id=' + str(item['id'])
        self.assertEqual(self.http(path)[0], 401)
        status, info = self.http(path, authorized=True)
        self.assertEqual(status, 200)
        self.assertEqual(info['requestId'], body['requestId'])
        self.assertEqual(info['attribution'], {'guestId': body['guestId'], 'source': 'native',
            'sourceDeclared': True, 'operatorLabel': 'Declared operator',
            'httpPeer': {'address': peer[0], 'port': peer[1]}})
        for public_path in ('/api/state', '/api/chat', '/health'):
            public_status, public = self.http(public_path)
            self.assertEqual(public_status, 200)
            serialized = json.dumps(public)
            for private in (body['guestId'], peer[0], 'Declared operator', 'httpPeer', 'operatorLabel'):
                self.assertNotIn(private, serialized)
        self.assertEqual(self.http('/api/admin/events')[0], 401)
        for invalid in ('0', '-1', 'no-id', str(2**54), '999999'):
            self.assertEqual(self.http('/api/admin/message-info?id=' + invalid, authorized=True)[0], 400)

    def test_message_timestamps_preserve_first_acceptance_and_first_read(self):
        unused, sent = self.http('/api/message', {'requestId': 'origin-timestamps', 'guestId': 'fixture-guest',
                                                'source': 'web', 'text': 'Keep my timestamps.'})
        identifier = sent['message']['id']
        created = self.stamp
        self.stamp += 100
        self.party.admin('processing', {'messageIds': [identifier], 'active': True})
        accepted = self.stamp
        self.stamp += 100
        self.party.admin('heartbeat', {'readThrough': identifier})
        read = self.stamp
        self.stamp += 100
        self.party.admin('processing', {'messageIds': [identifier], 'active': True})
        self.party.admin('heartbeat', {'readThrough': identifier})
        info = self.party.message_info(identifier)
        self.assertEqual([info['message'][field] for field in ('createdAt', 'acceptedAt', 'readAt')],
                         [created, accepted, read])
        self.assertEqual(info['attribution']['source'], 'web')

    def test_legacy_migration_keeps_origin_unknown_and_history_unchanged(self):
        directory = Path(self.directory.name) / 'legacy'
        directory.mkdir()
        database = server.sqlite3.connect(directory / 'party.sqlite3')
        database.executescript('''
            CREATE TABLE chat(id INTEGER PRIMARY KEY AUTOINCREMENT, request_id TEXT UNIQUE,
              role TEXT NOT NULL, name TEXT NOT NULL, text TEXT NOT NULL,
              created_at INTEGER NOT NULL, read_at INTEGER);
            CREATE TABLE requests(id TEXT PRIMARY KEY, uri TEXT NOT NULL, requested_by TEXT,
              dedication TEXT, track TEXT NOT NULL, status TEXT NOT NULL,
              created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL, error TEXT);
            CREATE TABLE events(id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT NOT NULL,
              payload TEXT NOT NULL, created_at INTEGER NOT NULL);
        ''')
        historical = '{"id":1,"text":"Legacy exact wording."}'
        database.execute("INSERT INTO chat VALUES(1,'legacy-id','user','Guest','Legacy exact wording.',123,NULL)")
        database.execute("INSERT INTO events(kind,payload,created_at) VALUES('chat',?,123)", (historical,))
        database.commit(); database.close()
        legacy = server.Party(directory, client=self.client)
        self.addCleanup(legacy.db.close)
        info = legacy.message_info(1)
        self.assertEqual(info['attribution'], {'guestId': None, 'source': 'unknown', 'sourceDeclared': False,
                                              'operatorLabel': None, 'httpPeer': None})
        self.assertEqual(info['message']['text'], 'Legacy exact wording.')
        self.assertEqual(legacy.db.execute('SELECT payload FROM events WHERE id=1').fetchone()[0], historical)
        self.assertIsNone(legacy.db.execute('SELECT origin FROM chat WHERE id=1').fetchone()[0])

    def test_concurrent_retries_keep_one_message_and_the_original_origin(self):
        body = {'requestId': 'concurrent-origin-id', 'guestId': 'fixture-guest', 'source': 'web', 'text': 'One instruction.'}
        peers = [('198.51.100.11', 41001), ('198.51.100.12', 41002)]
        results = self.parallel(
            lambda: self.http('/api/message', body, operator='First caller', peer=peers[0]),
            lambda: self.http('/api/message', body, operator='Second caller', peer=peers[1]))
        self.assertEqual(results[0], results[1])
        self.assertEqual(results[0][0], 200)
        identifier = results[0][1]['message']['id']
        initial = self.party.message_info(identifier)['attribution']
        expected = {'198.51.100.11': 'First caller', '198.51.100.12': 'Second caller'}
        self.assertEqual(initial['operatorLabel'], expected[initial['httpPeer']['address']])
        self.assertEqual(self.http('/api/message', body, operator='Later caller', peer=('198.51.100.13', 41003)), results[0])
        for changed in ({'source': 'native'}, {'source': 'test'}, {'guestId': 'different-guest'}):
            self.assertEqual(self.http('/api/message', {**body, **changed})[0], 400)
        self.assertEqual(self.party.message_info(identifier)['attribution'], initial)
        self.assertEqual(self.party.db.execute("SELECT COUNT(*) FROM events WHERE kind='chat'").fetchone()[0], 1)

    def test_concurrent_distinct_messages_do_not_share_caller_context(self):
        def send(number):
            return self.http('/api/message', {'requestId': f'origin-{number}', 'guestId': f'guest-{number}',
                'source': 'native' if number == 1 else 'web', 'text': f'Message {number}.'},
                operator=f'Operator {number}', peer=(f'198.51.100.{number}', 42000 + number))
        results = self.parallel(lambda: send(1), lambda: send(2))
        for number, (status, sent) in enumerate(results, 1):
            self.assertEqual(status, 200)
            origin = self.party.message_info(sent['message']['id'])['attribution']
            self.assertEqual(origin['guestId'], f'guest-{number}')
            self.assertEqual(origin['operatorLabel'], f'Operator {number}')
            self.assertEqual(origin['httpPeer'], {'address': f'198.51.100.{number}', 'port': 42000 + number})

    def test_explicit_test_message_is_labeled_and_cannot_enter_guest_processing_or_queue(self):
        body = {'requestId': 'explicit-test-message', 'guestId': 'fixture-guest', 'source': 'test', 'text': 'Fixture instruction.'}
        status, result = self.http('/api/message', body)
        self.assertEqual(status, 200)
        item = result['message']
        self.assertEqual((item['role'], item['isTest'], item['source']), ('test', True, 'test'))
        self.assertTrue(item['name'].startswith('TEST / '))
        self.assertEqual(item['text'], '[TEST] Fixture instruction.')
        self.assertEqual([row[0] for row in self.party.db.execute('SELECT kind FROM events ORDER BY id')], ['test_message'])
        self.assertEqual(self.http('/api/admin/processing', {'messageIds': [item['id']], 'active': True}, authorized=True)[0], 400)
        self.party.admin('heartbeat', {'readThrough': item['id']})
        self.assertIsNone(self.party.message_info(item['id'])['message']['readAt'])
        self.assertEqual(self.http('/api/request', {**body, 'uri': TRACK_A})[0], 400)
        self.assertEqual(self.party.db.execute('SELECT COUNT(*) FROM requests').fetchone()[0], 0)
        self.assertEqual(self.client.enqueue_calls, [])

    def test_invalid_declared_sources_are_rejected_before_message_creation(self):
        for source in (None, False, 1, [], {}, 'browser', 'unknown', 'web '):
            with self.subTest(source=source):
                self.assertEqual(self.http('/api/message', {'text': 'Invalid source.', 'source': source})[0], 400)
        self.assertEqual(self.party.chat()['messages'], [])
        self.assertEqual(self.party.db.execute('SELECT COUNT(*) FROM events').fetchone()[0], 0)

    def test_processing_and_recall_race_has_one_winner_and_retains_origin(self):
        unused, sent = self.http('/api/message', {'requestId': 'recall-race-message', 'guestId': 'fixture-guest',
                                                'source': 'native', 'text': 'Race only in a private fixture.'})
        identifier = sent['message']['id']
        origin = self.party.message_info(identifier)['attribution']
        results = self.parallel(
            lambda: self.http('/api/admin/processing', {'messageIds': [identifier], 'active': True}, authorized=True),
            lambda: self.http('/api/message/recall', {'guestId': 'fixture-guest', 'messageId': identifier}))
        self.assertEqual(sorted(status for status, unused in results), [200, 400])
        info = self.party.message_info(identifier)
        self.assertEqual(info['attribution'], origin)
        self.assertNotEqual(info['withdrawnAt'] is not None, info['message']['acceptedAt'] is not None)

    def test_command_dispatch_precedes_one_call_and_records_private_returned_context(self):
        self.party.admin('sync', {})
        fresh = self.client.status()
        response = {'ok': True, 'action': 'pause', 'dispatched': True, 'status': fresh}
        def control(action):
            self.client.control_calls.append(action)
            self.assertEqual(self.command_events()[-1]['phase'], 'dispatch')
            return response
        with patch.object(self.client, 'control', side_effect=control):
            status, returned = self.http('/api/admin/pause', {'source': 'api'}, authorized=True,
                                         operator='Private command caller', peer=('198.51.100.70', 47000))
        self.assertEqual((status, returned), (200, response))
        events = self.command_events()
        self.assertEqual([event['phase'] for event in events], ['dispatch', 'returned'])
        self.assertEqual(events[0]['commandId'], events[1]['commandId'])
        self.assertEqual(events[0]['before']['currentUid'], self.client.current['uid'])
        self.assertEqual(events[0]['before']['deviceId'], 'fixture-device')
        self.assertEqual(events[1]['attribution']['operatorLabel'], 'Private command caller')
        self.assertEqual(events[1]['attribution']['httpPeer'], {'address': '198.51.100.70', 'port': 47000})
        self.assertEqual((events[1]['dispatched'], events[1]['dispatchUncertain'], events[1]['returnedStatus']), (True, False, True))
        self.assertEqual(self.client.control_calls, ['pause'])
        for path in ('/api/state', '/api/chat', '/health'):
            serialized = json.dumps(self.http(path)[1])
            for private in ('Private command caller', '198.51.100.70', events[0]['commandId']):
                self.assertNotIn(private, serialized)
        self.assertEqual(self.http('/api/admin/events')[0], 401)
        private_events = self.http('/api/admin/events', authorized=True)[1]['events']
        self.assertEqual(len([event for event in private_events if event['kind'] == 'command']), 2)

    def test_command_failures_preserve_dispatch_uncertainty_without_retry(self):
        failures = [(SpotifyError('Rejected before transport', dispatched=False), False, False),
                    (SpotifyError('Reply lost', dispatched=True), True, True),
                    (RuntimeError('Unknown transport outcome'), None, True)]
        for error, dispatched, uncertain in failures:
            with self.subTest(error=error), patch.object(self.client, 'control', side_effect=error) as control:
                status, returned = self.http('/api/admin/pause', {'source': 'api'}, authorized=True, operator='Failure fixture')
                self.assertEqual(status, 503)
                if type(dispatched) is bool:
                    self.assertIs(returned['dispatched'], dispatched)
                control.assert_called_once_with('pause')
                attempt, outcome = self.command_events()[-2:]
                self.assertEqual((attempt['phase'], outcome['phase']), ('dispatch', 'error'))
                self.assertEqual(attempt['commandId'], outcome['commandId'])
                self.assertEqual((outcome['dispatched'], outcome['dispatchUncertain']), (dispatched, uncertain))
                self.assertEqual(outcome['error'], str(error))
        self.assertEqual(len(self.command_events()), 6)

    def test_invalid_and_unverified_queue_commands_are_logged_without_repetition(self):
        before = (self.party.revision, self.party.last_seen, self.party._control_generation)
        status, unused = self.http('/api/admin/reorder', {'orderedQueuedUids': ['same', 'same']},
                                  authorized=True, operator='Rejected fixture')
        self.assertEqual(status, 400)
        rejected = self.command_events()[-1]
        self.assertEqual((rejected['phase'], rejected['dispatched'], rejected['dispatchUncertain']), ('rejected', False, False))
        self.assertEqual((self.party.revision, self.party.last_seen, self.party._control_generation), before)
        self.assertEqual(self.client.reorder_calls, [])
        self.client.move_result = {'ok': True}
        status, error = self.http('/api/admin/move', {'uid': 'second', 'beforeUid': 'first',
            'expectedQueuedUids': ['first', 'second']}, authorized=True)
        self.assertEqual(status, 503)
        self.assertTrue(error['dispatched'])
        self.assertEqual(len(self.client.move_calls), 1)
        self.assertEqual([event['phase'] for event in self.command_events()], ['rejected', 'dispatch', 'error'])

    def test_enqueue_inherits_request_origin_and_records_known_return(self):
        body = {'requestId': 'attributed-queue-request', 'uri': TRACK_A, 'guestId': 'queue-guest', 'source': 'web'}
        self.assertEqual(self.http('/api/request', body, operator='Queue submitter', peer=('198.51.100.80', 48000))[0], 200)
        self.party._refresh()
        self.party._dispatch()
        events = self.command_events()
        self.assertEqual([event['phase'] for event in events], ['dispatch', 'returned'])
        self.assertTrue(all(event['action'] == 'enqueue' for event in events))
        self.assertEqual(events[0]['attribution']['guestId'], 'queue-guest')
        self.assertEqual(events[0]['attribution']['operatorLabel'], 'Queue submitter')
        self.assertEqual(events[1]['target'], {'requestId': body['requestId'], 'uri': TRACK_A})
        self.assertTrue(events[1]['result']['queued'])
        self.assertEqual((events[1]['dispatched'], events[1]['dispatchUncertain']), (True, False))
        self.assertEqual(self.client.enqueue_calls, [(TRACK_A, body['requestId'])])
        public = json.dumps(self.party.snapshot())
        self.assertNotIn('queue-guest', public)
        self.assertNotIn('Queue submitter', public)

    def test_uncertain_enqueue_is_logged_and_not_dispatched_again(self):
        self.party.request({'requestId': 'uncertain-origin-request', 'uri': TRACK_A, 'source': 'api'},
                           context={'operatorLabel': 'Uncertain submitter'})
        self.party._refresh()
        self.client.enqueue_error = SpotifyError('Enqueue response lost', dispatched=True)
        self.party._dispatch()
        self.party._dispatch()
        events = self.command_events()
        self.assertEqual([event['phase'] for event in events], ['dispatch', 'error'])
        self.assertEqual(events[1]['attribution']['operatorLabel'], 'Uncertain submitter')
        self.assertTrue(events[1]['dispatchUncertain'])
        self.assertEqual(len(self.client.enqueue_calls), 1)

    def test_nonboolean_dispatch_flag_never_authorizes_an_enqueue_retry(self):
        self.party.request({'requestId': 'unknown-dispatch-flag', 'uri': TRACK_A})
        self.party._refresh()
        for value in (None, 0):
            with self.subTest(dispatched=value):
                self.party.db.execute("UPDATE requests SET status='pending' WHERE id='unknown-dispatch-flag'")
                self.party.db.commit()
                before = len(self.client.enqueue_calls)
                self.client.enqueue_error = SpotifyError('Unspecified dispatch outcome', dispatched=value)
                self.party._dispatch()
                self.stamp += 60000
                self.party._dispatch()
                self.assertEqual(len(self.client.enqueue_calls), before + 1)
                self.assertEqual(self.party.db.execute("SELECT status FROM requests WHERE id='unknown-dispatch-flag'").fetchone()[0], 'uncertain')
                event = self.command_events()[-1]
                self.assertIsNone(event['dispatched'])
                self.assertTrue(event['dispatchUncertain'])

    def test_malformed_or_nonqueued_enqueue_return_stays_uncertain_without_retry(self):
        self.party._refresh()
        changes = [None, {}, {'queued': False}, {'queued': 1}, {'uid': None}, {'uid': ''},
                   {'uri': TRACK_B}, {'requestId': 'wrong-request'}]
        for index, changed in enumerate(changes):
            with self.subTest(changed=changed):
                uri = 'spotify:track:' + str(index) * 22
                identifier = f'malformed-enqueue-{index}'
                self.party.request({'requestId': identifier, 'uri': uri})
                valid = {'queued': True, 'uri': uri, 'requestId': identifier, 'uid': f'fixture-{index}'}
                returned = changed if changed is None or changed == {} else {**valid, **changed}
                with patch.object(self.client, 'enqueue', return_value=returned) as enqueue:
                    self.party._dispatch()
                    self.stamp += 6000
                    self.party._dispatch()
                    enqueue.assert_called_once_with(uri, identifier)
                self.assertEqual(self.party.db.execute('SELECT status FROM requests WHERE id=?', (identifier,)).fetchone()[0], 'uncertain')
                outcome = self.command_events()[-1]
                self.assertEqual(outcome['phase'], 'error')
                self.assertTrue(outcome['dispatchUncertain'])
        self.assertEqual(self.party.db.execute("SELECT COUNT(*) FROM events WHERE kind='queued'").fetchone()[0], 0)
        self.assertEqual(list(self.party.caption_queue), [])

    def test_work_public_map_and_active_expiry_preserve_the_unfinished_record(self):
        sent = self.message()
        status, result = self.http('/api/admin/work', {'messageIds': [sent['id']], 'state': 'active',
            'summary': 'Preparing the requested visual.', 'outcome': 'First step ready.', 'evidence': ['Fixture frame prepared.']}, authorized=True)
        self.assertEqual(status, 200)
        stored = result['dj']['messageWork'][str(sent['id'])]
        self.assertEqual((stored['state'], stored['effectiveState']), ('active', 'active'))
        self.assertEqual(stored['activeUntil'], self.stamp + 300000)
        self.assertEqual(result['dj']['processingMessageIds'], [sent['id']])
        message = self.party.chat()['messages'][0]
        self.assertEqual(message['work'], stored)
        self.assertEqual(message['acceptedAt'], self.stamp)
        self.assertIsNone(message['readAt'])
        published = json.loads((self.party.data / 'state.json').read_text())
        self.assertEqual(published['dj']['messageWork'][str(sent['id'])], stored)
        status, empty_poll = self.http('/api/chat?after=' + str(sent['id']))
        self.assertEqual(status, 200)
        self.assertEqual(empty_poll['messages'], [])
        self.assertEqual(empty_poll['dj']['messageWork'][str(sent['id'])], stored)
        self.stamp = stored['activeUntil']
        expired = self.party.snapshot()['dj']
        self.assertEqual(expired['processingMessageIds'], [])
        self.assertEqual(expired['messageWork'][str(sent['id'])], {**stored, 'effectiveState': 'unconfirmed'})
        self.assertEqual(self.party.db.execute('SELECT state,outcome FROM message_work WHERE message_id=?', (sent['id'],)).fetchone()[:],
                         ('active', 'First step ready.'))

    def test_completed_work_requires_an_actual_outcome_and_survives_restart(self):
        sent = self.message()
        self.party.admin('work', {'messageIds': [sent['id']], 'state': 'active', 'summary': 'Rendering the fixture.'})
        accepted = self.stamp
        self.stamp += 1000
        result = self.party.admin('work', {'messageIds': [sent['id']], 'state': 'completed',
            'summary': 'Fixture rendering finished.', 'outcome': 'The original fixture frame is available.',
            'evidence': ['Private fixture renderer returned a frame.']})
        completed = result['dj']['messageWork'][str(sent['id'])]
        self.assertEqual((completed['state'], completed['effectiveState'], completed['activeUntil']), ('completed', 'completed', None))
        reopened = server.Party(self.party.data, client=self.client)
        self.addCleanup(reopened.db.close)
        self.assertEqual(reopened.snapshot()['dj']['messageWork'][str(sent['id'])], completed)
        self.assertEqual(reopened.chat()['messages'][0]['acceptedAt'], accepted)
        self.assertIsNone(reopened.chat()['messages'][0]['readAt'])
        self.assertEqual(reopened.snapshot()['dj']['processingMessageIds'], [])
        self.assertEqual(reopened.db.execute("SELECT COUNT(*) FROM events WHERE kind='work_changed'").fetchone()[0], 2)
        with self.assertRaises(ValueError):
            reopened.recall_message({'guestId': 'fixture-guest', 'messageId': sent['id']})

    def test_waiting_blocked_and_cancelled_work_have_explicit_outcomes_without_live_leases(self):
        sent = self.message()
        for state, summary, outcome in (('waiting', 'Waiting for a performer.', ''),
                                        ('blocked', 'The supplied material is missing.', 'Need the original cue sheet.'),
                                        ('cancelled', '', 'The guest cancelled this turn.')):
            with self.subTest(state=state):
                result = self.party.admin('work', {'messageIds': [sent['id']], 'state': state,
                                                   'summary': summary, 'outcome': outcome})
                work = result['dj']['messageWork'][str(sent['id'])]
                self.assertEqual((work['state'], work['effectiveState'], work['activeUntil']), (state, state, None))
                self.assertEqual(work['outcome'], outcome)
                self.assertEqual(result['dj']['processingMessageIds'], [])
                with self.assertRaises(ValueError):
                    self.party.recall_message({'guestId': 'fixture-guest', 'messageId': sent['id']})

    def test_work_batch_validation_is_atomic_for_receipts_presence_and_all_records(self):
        first, second = self.message('First request.'), self.message('Second request.')
        reply = self.party.message({'text': 'An assistant reply.'}, role='assistant')['message']
        test = self.party.message({'text': 'A fixture message.', 'source': 'test'})['message']
        self.party.admin('work', {'messageIds': [first['id']], 'state': 'active', 'summary': 'Already working.'})
        valid = {'messageIds': [first['id'], second['id']], 'state': 'active', 'summary': 'Preparing both requests.', 'readThrough': second['id']}
        changes = [
            {'messageIds': []}, {'messageIds': [True]}, {'messageIds': [str(first['id'])]},
            {'messageIds': [first['id'], 999999]}, {'messageIds': [first['id'], reply['id']]},
            {'messageIds': [first['id'], test['id']]}, {'messageIds': [first['id']] * 51},
            {'state': 'done'}, {'state': None}, {'summary': ''}, {'summary': '..'},
            {'summary': 'x' * 501}, {'summary': 3}, {'outcome': 'x' * 1001},
            {'state': 'completed', 'outcome': 'Finished.', 'evidence': []},
            {'state': 'completed', 'outcome': '', 'evidence': ['A real fixture observation.']},
            {'state': 'cancelled', 'outcome': ''}, {'evidence': ['']},
            {'evidence': ['x' * 501]}, {'evidence': ['one'] * 9}, {'evidence': {}},
        ]
        changes += [{'leaseSeconds': value} for value in (None, False, 29, 901, 300.0, '300')]
        def state():
            return (self.party.revision, self.party.last_seen, self.party.snapshot()['dj']['messageWork'],
                    [tuple(row) for row in self.party.db.execute('SELECT id,accepted_at,read_at FROM chat ORDER BY id')],
                    self.party.setting('processingMessages'), (self.party.data / 'state.json').read_bytes())
        before = state()
        self.assertEqual(self.http('/api/admin/work', valid)[0], 401)
        for changed in changes:
            with self.subTest(changed=changed):
                self.assertEqual(self.http('/api/admin/work', {**valid, **changed}, authorized=True)[0], 400)
                self.assertEqual(state(), before)

    def test_legacy_processing_clear_waits_and_cannot_overwrite_terminal_work(self):
        first, second = self.message('First request.'), self.message('Second request.')
        self.party.admin('processing', {'messageIds': [first['id']], 'active': True})
        accepted = self.party.message_info(first['id'])['message']['acceptedAt']
        self.party.admin('processing', {'messageIds': [first['id']], 'active': False})
        waiting = self.party.message_info(first['id'])['message']['work']
        self.assertEqual((waiting['state'], waiting['effectiveState']), ('waiting', 'waiting'))
        self.assertTrue(waiting['outcome'])
        terminal = self.party.admin('work', {'messageIds': [first['id']], 'state': 'completed',
            'outcome': 'Requested change is available.', 'evidence': ['Observed the private fixture output.']})['dj']['messageWork'][str(first['id'])]
        self.party.admin('processing', {'messageIds': [first['id']], 'active': False})
        self.assertEqual(self.party.message_info(first['id'])['message']['work'], terminal)
        with self.assertRaises(ValueError):
            self.party.admin('processing', {'messageIds': [first['id'], second['id']], 'active': True})
        self.assertIsNone(self.party.message_info(second['id'])['message']['work'])
        self.party.admin('work', {'messageIds': [first['id']], 'state': 'active', 'summary': 'Explicitly requested follow-up.'})
        self.assertEqual(self.party.message_info(first['id'])['message']['work']['state'], 'active')
        self.assertEqual(self.party.message_info(first['id'])['message']['acceptedAt'], accepted)

    def test_memory_http_processing_requires_admin_and_updates_an_empty_chat_poll(self):
        sent = self.message()
        body = {'messageIds': [sent['id']], 'active': True}
        self.assertEqual(self.http('/api/admin/processing', body)[0], 401)
        self.assertEqual(self.party.last_seen, 0)
        self.assertEqual(self.party.snapshot()['dj']['processingMessageIds'], [])
        status, result = self.http('/api/admin/processing', body, authorized=True)
        self.assertEqual(status, 200)
        self.assertEqual(result['dj']['processingMessageIds'], [sent['id']])
        status, chat = self.http('/api/chat?after=' + str(sent['id']))
        self.assertEqual(status, 200)
        self.assertEqual(chat['messages'], [])
        self.assertEqual(chat['dj']['processingMessageIds'], [sent['id']])
        self.assertEqual(chat['dj']['messageWork'][str(sent['id'])]['effectiveState'], 'active')
        self.assertEqual(self.http('/api/admin/processing', {'messageIds': [sent['id'], 999999], 'active': False}, authorized=True)[0], 400)
        self.assertEqual(self.party.snapshot()['dj']['processingMessageIds'], [sent['id']])
        self.assertEqual(self.http('/api/admin/processing', {'messageIds': [sent['id']], 'active': False}, authorized=True)[0], 200)
        self.assertEqual(self.party.snapshot()['dj']['processingMessageIds'], [])
        self.assertEqual(self.party.message_info(sent['id'])['message']['work']['state'], 'waiting')

    def test_legacy_marker_migration_is_once_only_waiting_and_never_guesses_acceptance_time(self):
        first, second = self.message('Earlier work.'), self.message('Expired earlier work.')
        self.party.set_setting('processingMessages', {str(first['id']): self.stamp + 900000,
                                                      str(second['id']): self.stamp - 1000, '999999': self.stamp + 1000})
        self.party.db.execute("DELETE FROM settings WHERE key='messageWorkMigrated'")
        self.party.db.commit()
        restored = server.Party(self.party.data, client=self.client)
        self.addCleanup(restored.db.close)
        work = restored.snapshot()['dj']['messageWork']
        self.assertEqual(set(work), {str(first['id']), str(second['id'])})
        for record in work.values():
            self.assertEqual((record['state'], record['effectiveState'], record['activeUntil']), ('waiting', 'waiting', None))
            self.assertEqual(record['summary'], 'Raniji rad čeka zabilježen ishod')
            self.assertEqual(record['outcome'], '')
        self.assertTrue(all(message['acceptedAt'] is None for message in restored.chat()['messages']))
        with self.assertRaises(ValueError):
            restored.recall_message({'guestId': 'fixture-guest', 'messageId': second['id']})
        self.assertEqual(restored.setting('processingMessages'), {})
        event_count = restored.db.execute("SELECT COUNT(*) FROM events WHERE kind='work_changed'").fetchone()[0]
        self.stamp += 5000
        reopened = server.Party(self.party.data, client=self.client)
        self.addCleanup(reopened.db.close)
        self.assertEqual(reopened.snapshot()['dj']['messageWork'], work)
        self.assertEqual(reopened.db.execute("SELECT COUNT(*) FROM events WHERE kind='work_changed'").fetchone()[0], event_count)

    def test_terminal_work_wins_a_race_with_legacy_renewal(self):
        sent = self.message()
        self.party.admin('processing', {'messageIds': [sent['id']], 'active': True})
        completed = {'messageIds': [sent['id']], 'state': 'completed', 'outcome': 'The requested fixture is delivered.',
                     'evidence': ['Delivery observed in the private fixture.']}
        results = self.parallel(
            lambda: self.http('/api/admin/work', completed, authorized=True),
            lambda: self.http('/api/admin/processing', {'messageIds': [sent['id']], 'active': True}, authorized=True))
        self.assertEqual(results[0][0], 200)
        self.assertIn(results[1][0], (200, 400))
        work = self.party.message_info(sent['id'])['message']['work']
        self.assertEqual((work['state'], work['outcome'], work['evidence']),
                         ('completed', completed['outcome'], completed['evidence']))

    def test_migrated_unknown_acceptance_survives_completion_reopen_and_legacy_updates(self):
        sent = self.message('Historical work with an unknown acceptance time.')
        self.party.set_setting('processingMessages', {str(sent['id']): self.stamp + 900000})
        self.party.db.execute("DELETE FROM settings WHERE key='messageWorkMigrated'")
        self.party.db.commit()
        restored = server.Party(self.party.data, client=self.client)
        self.addCleanup(restored.db.close)

        def check(party, expected_state):
            info = party.message_info(sent['id'])
            self.assertIsNone(info['message']['acceptedAt'])
            self.assertEqual(info['message']['work']['state'], expected_state)
            with self.assertRaises(ValueError):
                party.recall_message({'guestId': 'fixture-guest', 'messageId': sent['id']})

        check(restored, 'waiting')
        self.stamp += 1000
        restored.admin('work', {'messageIds': [sent['id']], 'state': 'completed',
            'outcome': 'The historical fixture outcome is now recorded.',
            'evidence': ['The original fixture result was inspected.']})
        check(restored, 'completed')
        reopened = server.Party(self.party.data, client=self.client)
        self.addCleanup(reopened.db.close)
        check(reopened, 'completed')
        self.stamp += 1000
        reopened.admin('work', {'messageIds': [sent['id']], 'state': 'active',
                                'summary': 'Explicit follow-up to historical work.'})
        check(reopened, 'active')
        self.stamp += 1000
        reopened.admin('processing', {'messageIds': [sent['id']], 'active': True})
        check(reopened, 'active')
        self.stamp += 1000
        reopened.admin('processing', {'messageIds': [sent['id']], 'active': False})
        check(reopened, 'waiting')
        self.stamp += 1000
        reopened.admin('work', {'messageIds': [sent['id']], 'state': 'waiting',
                                'summary': 'The follow-up is waiting for more input.'})
        check(reopened, 'waiting')

    def test_explicit_work_acceptance_and_recall_have_one_atomic_winner(self):
        sent = self.message()
        results = self.parallel(
            lambda: self.http('/api/admin/work', {'messageIds': [sent['id']], 'state': 'active', 'summary': 'Accepting this work.'}, authorized=True),
            lambda: self.http('/api/message/recall', {'guestId': 'fixture-guest', 'messageId': sent['id']}))
        self.assertEqual(sorted(status for status, unused in results), [200, 400])
        info = self.party.message_info(sent['id'])
        self.assertNotEqual(info['withdrawnAt'] is not None, info['message']['work'] is not None)

    def playback_body(self, paused=True):
        return {'paused': paused, 'expectedCurrentUid': self.client.current['uid'], 'expectedDeviceId': 'fixture-device', 'source': 'api'}

    def test_desired_playback_is_explicit_and_publishes_actual_paused_or_playing_state(self):
        self.party.admin('sync', {})
        for paused, changed in ((True, False), (False, True), (True, True)):
            with self.subTest(paused=paused, changed=changed):
                status, result = self.http('/api/admin/playback', self.playback_body(paused), authorized=True, operator='Playback fixture')
                self.assertEqual(status, 200)
                self.assertEqual((result['action'], result['paused'], result['changed']), ('setPaused', paused, changed))
                published = json.loads((self.party.data / 'state.json').read_text())
                self.assertEqual(published['nowPlaying']['paused'], paused)
                self.assertEqual(published['nowPlaying']['playing'], not paused)
                self.assertEqual(published['playbackState']['paused'], paused)
                outcome = self.command_events()[-1]
                self.assertEqual((outcome['action'], outcome['phase'], outcome['target']['paused']), ('playback', 'returned', paused))
                self.assertEqual((outcome['dispatched'], outcome['dispatchUncertain']), (changed, False))
        self.assertEqual(self.client.set_paused_calls, [(value, self.client.current['uid'], 'fixture-device') for value in (True, False, True)])
        self.assertEqual(self.client.control_calls, [])

    def test_desired_pause_interrupts_speech_before_entering_transport(self):
        order = []
        self.party.audio = SimpleNamespace(interrupt=lambda: order.append('interrupt'),
            snapshot=lambda: {'available': True, 'enabled': True}, observe=lambda *args, **kwargs: None)
        actual = self.client.set_paused
        for paused in (True, False):
            with self.subTest(paused=paused):
                order.clear()
                def transport(value, current, device):
                    self.assertEqual(order, ['interrupt'] if paused else [])
                    order.append('transport')
                    return actual(value, current, device)
                with patch.object(self.client, 'set_paused', side_effect=transport):
                    self.party.admin('playback', self.playback_body(paused))
                self.assertEqual(order, ['interrupt', 'transport'] if paused else ['transport'])

    def test_desired_playback_schema_and_auth_fail_before_receipts_or_transport(self):
        sent = self.message()
        valid = {**self.playback_body(), 'readThrough': sent['id']}
        before = (self.party.last_seen, self.party.revision, self.party._control_generation)
        self.assertEqual(self.http('/api/admin/playback', valid)[0], 401)
        changes = [{'paused': value} for value in (None, 0, 'false', [], {})]
        changes += [{key: value} for key in ('expectedCurrentUid', 'expectedDeviceId')
                    for value in (None, False, '', '   ', 'x' * 513)]
        for change in changes:
            with self.subTest(change=change):
                self.assertEqual(self.http('/api/admin/playback', {**valid, **change}, authorized=True)[0], 400)
                self.assertEqual((self.party.last_seen, self.party.revision, self.party._control_generation), before)
                self.assertIsNone(self.party.message_info(sent['id'])['message']['readAt'])
        self.assertEqual(self.client.set_paused_calls, [])
        self.assertTrue(all(event['phase'] == 'rejected' and event['dispatched'] is False for event in self.command_events()))

    def test_desired_playback_rejects_stale_occurrence_or_device_once(self):
        self.party.admin('sync', {})
        before = copy.deepcopy(self.party.playback)
        for change in ({'expectedCurrentUid': 'stale-occurrence'}, {'expectedDeviceId': 'other-device'}):
            with self.subTest(change=change):
                calls = len(self.client.set_paused_calls)
                status, result = self.http('/api/admin/playback', {**self.playback_body(False), **change}, authorized=True)
                self.assertEqual(status, 503)
                self.assertFalse(result['dispatched'])
                self.assertEqual(len(self.client.set_paused_calls), calls + 1)
                self.assertEqual(self.party.playback, before)
                self.assertEqual(self.command_events()[-1]['dispatched'], False)

    def test_desired_playback_response_loss_or_missing_status_never_retries(self):
        self.party.admin('sync', {})
        before = copy.deepcopy(self.party.playback)
        fresh = self.client.status()
        fresh['nowPlaying'].update(paused=False, playing=True)
        fresh['playbackState']['paused'] = False
        valid = {'ok': True, 'action': 'setPaused', 'paused': False, 'changed': True, 'status': fresh}
        responses = [SpotifyError('Response lost after the desired-state write', dispatched=True),
                     {'ok': True, 'action': 'setPaused', 'paused': False, 'changed': True},
                     {**valid, 'status': {}}]
        for field, bad in (('queue', None), ('queue', [None]), ('queue', [{'uid': []}]),
                           ('nowPlaying', None), ('playbackState', {}), ('observedAt', self.stamp - 10001)):
            responses.append({**valid, 'status': {**fresh, field: bad}})
        published = (self.party.data / 'state.json').read_bytes()
        for response in responses:
            with self.subTest(response=response):
                def apply_once(paused, current, device):
                    self.client.set_paused_calls.append((paused, current, device))
                    if isinstance(response, Exception):
                        raise response
                    return response
                calls = len(self.client.set_paused_calls)
                with patch.object(self.client, 'set_paused', side_effect=apply_once), patch.object(self.client, 'status') as reread:
                    status, result = self.http('/api/admin/playback', self.playback_body(False), authorized=True)
                    self.assertEqual(status, 503)
                    self.assertTrue(result['dispatched'])
                    reread.assert_not_called()
                self.assertEqual(len(self.client.set_paused_calls), calls + 1)
                self.assertEqual(self.party.playback, before)
                self.assertEqual((self.party.data / 'state.json').read_bytes(), published)
                self.assertEqual(self.command_events()[-1]['phase'], 'error')
                self.assertTrue(self.command_events()[-1]['dispatchUncertain'])

    def test_old_poll_cannot_replace_desired_playback_observation(self):
        old = self.client.status()
        actual_status = self.client.status
        started, release = threading.Event(), threading.Event()
        calls = []
        def status():
            calls.append(1)
            if len(calls) == 1:
                started.set()
                if not release.wait(3):
                    raise AssertionError('Old fixture poll was not released.')
                return old
            return actual_status()
        with patch.object(self.client, 'status', side_effect=status):
            worker = threading.Thread(target=self.party._refresh, daemon=True)
            worker.start()
            try:
                self.assertTrue(started.wait(3))
                self.party.admin('playback', self.playback_body(False))
            finally:
                release.set(); worker.join(3)
        self.assertFalse(worker.is_alive())
        self.assertFalse(self.party.snapshot()['nowPlaying']['paused'])
        self.assertEqual(len(self.client.set_paused_calls), 1)

    def test_preloaded_karaoke_binds_on_accepted_observation_and_persists_once(self):
        self.party.admin('karaoke', karaoke_sheet())
        self.assertEqual(self.party.setting('karaoke')['trackUid'], '')
        self.client.current = {**track(TRACK_A), 'uid': 'bound-fixture-occurrence', 'paused': False,
                               'playing': True, 'buffering': False, 'progressMs': 1500,
                               'observedAt': self.stamp, 'clockValid': True}
        self.party.admin('sync', {})
        saved = self.party.setting('karaoke')
        self.assertEqual(saved['trackUid'], 'bound-fixture-occurrence')
        self.assertEqual(self.party.snapshot()['karaoke']['positionMs'], 1500)
        reopened = server.Party(self.party.data, client=self.client)
        self.addCleanup(reopened.db.close)
        reopened.admin('sync', {})
        self.assertEqual(reopened.setting('karaoke'), saved)
        self.client.current['uid'] = 'different-occurrence-of-the-same-uri'
        reopened.admin('sync', {})
        self.assertEqual(reopened.snapshot()['karaoke']['status'], 'waiting')
        self.assertEqual(reopened.setting('karaoke'), saved)

    def test_karaoke_media_sources_are_distinct_from_transport_origin_sources(self):
        for source in ('spotify', 'manual'):
            with self.subTest(media_source=source):
                status, result = self.http('/api/admin/karaoke', karaoke_sheet(source), authorized=True)
                self.assertEqual(status, 200)
                self.assertEqual(result['karaoke']['source'], source)
                self.assertEqual(self.party.setting('karaoke')['source'], source)
        sent = self.message()
        before = (self.party.last_seen, self.party.revision, self.party._control_generation,
                  self.party.setting('karaoke'))
        for source in ('spotify', 'manual', None):
            with self.subTest(transport_source=source):
                status, unused = self.http('/api/admin/playback',
                    {**self.playback_body(), 'source': source, 'readThrough': sent['id']}, authorized=True)
                self.assertEqual(status, 400)
                self.assertEqual((self.party.last_seen, self.party.revision, self.party._control_generation,
                                  self.party.setting('karaoke')), before)
                self.assertIsNone(self.party.message_info(sent['id'])['message']['readAt'])
        self.assertEqual(self.client.set_paused_calls, [])

    def test_preloaded_karaoke_binds_requested_uri_when_the_recording_is_relinked(self):
        self.party.request({'requestId': 'relinked-karaoke-request', 'uri': TRACK_A})
        self.party._refresh(); self.party._dispatch(); self.party._refresh()
        row = self.party.db.execute("SELECT track FROM requests WHERE id='relinked-karaoke-request'").fetchone()
        uid = json.loads(row[0])['uid']
        self.party.admin('karaoke', karaoke_sheet())
        self.assertEqual(self.party.setting('karaoke')['trackUid'], '')
        self.client.current = {**track(TRACK_B), 'uid': uid, 'paused': False, 'playing': True,
                               'buffering': False, 'progressMs': 1200, 'observedAt': self.stamp, 'clockValid': True}
        self.client.queue = [item for item in self.client.queue if item['uid'] != uid]
        self.party.admin('sync', {})
        self.assertEqual(self.party.setting('karaoke')['trackUid'], uid)
        self.assertEqual(self.party.snapshot()['karaoke']['status'], 'playing')
        self.assertEqual(self.party.snapshot()['nowPlaying']['requestedUri'], TRACK_A)

    def test_audio_observer_receives_actual_device_invalid_clock_and_source_timestamps(self):
        recorded = []
        def observe(current, requests, *, playback_state):
            recorded.append((copy.deepcopy(current), copy.deepcopy(requests), copy.deepcopy(playback_state)))
        self.party.audio = SimpleNamespace(snapshot=lambda: {'available': False, 'enabled': False}, observe=observe)
        for device in ('first-fixture-device', 'changed-fixture-device'):
            raw = observation(self.stamp - 250, uri=TRACK_A, uid='audio-fixture-occurrence')
            raw['nowPlaying']['clockValid'] = True
            raw['playbackState'].update(clockValid=False, sourceTimestamp=0,
                                        device={'id': device, 'name': 'Private fixture'})
            with patch.object(self.client, 'status', return_value=raw):
                self.party._refresh()
            current, requests, playback = recorded[-1]
            self.assertEqual(current['observedAt'], self.stamp - 250)
            self.assertIs(current['clockValid'], True)
            self.assertEqual(playback, raw['playbackState'])
            self.assertIs(playback['clockValid'], False)
            self.assertEqual(playback['sourceTimestamp'], 0)
            self.assertEqual(playback['device']['id'], device)
            self.assertEqual(requests, [])
        self.assertEqual(len(recorded), 2)


if __name__ == '__main__':
    unittest.main()
