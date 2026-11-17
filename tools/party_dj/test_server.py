"""Request and conversation regressions using isolated state and no Spotify session."""
from __future__ import annotations

import concurrent.futures
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
import urllib.error
import urllib.request

import server
from test_karaoke import sheet as karaoke_sheet
from spotify_native import SpotifyError


TRACK_A = 'spotify:track:' + 'a' * 22
TRACK_B = 'spotify:track:' + 'b' * 22
TRACK_RELINKED = 'spotify:track:' + 'c' * 22


def track(uri):
    return {'id': uri.rsplit(':', 1)[-1], 'uri': uri, 'title': uri[-1].upper(),
            'artist': 'Test artist', 'durationMs': 180000, 'source': 'queue'}


class FakeSpotify:
    mode = 'fixture'

    def __init__(self):
        self.queue = []
        self.current = {**track(TRACK_B), 'playing': True}
        self.explicit_queue_complete = True
        self.enqueue_calls = []
        self.enqueue_error = None
        self.metadata_barrier = None

    def track(self, uri):
        if self.metadata_barrier is not None:
            self.metadata_barrier.wait(timeout=5)
        return track(uri)

    def status(self):
        return copy.deepcopy({'nowPlaying': self.current, 'queue': self.queue,
                              'explicitQueueComplete': self.explicit_queue_complete})

    def enqueue(self, uri, request_id):
        self.enqueue_calls.append((uri, request_id))
        if self.enqueue_error is not None:
            raise self.enqueue_error
        queued = {**track(uri), 'uid': f'fixture-queue-{len(self.enqueue_calls)}'}
        self.queue.append(queued)
        return {'queued': True, 'uri': uri, 'requestId': request_id, 'uid': queued['uid']}


class PartyTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='larics-dj-test-')
        self.addCleanup(self.directory.cleanup)
        self.stamp = 1800000000000
        self.clock = patch.object(server, 'now_ms', side_effect=lambda: self.stamp)
        self.clock.start()
        self.addCleanup(self.clock.stop)
        self.client = FakeSpotify()
        self.party = server.Party(Path(self.directory.name), client=self.client)
        self.addCleanup(self.party.db.close)

    def request(self, request_id='request-a', uri=TRACK_A):
        return self.party.request({'requestId': request_id, 'uri': uri,
                                   'requestedBy': 'Test guest'})

    def row(self, request_id='request-a'):
        return self.party.db.execute('SELECT * FROM requests WHERE id=?',
                                     (request_id,)).fetchone()

    def queued_request(self):
        self.request()
        self.party._refresh()
        self.party._dispatch()
        self.party._refresh()
        self.assertEqual(self.row()['status'], 'queued')

    def test_request_id_reuse_during_metadata_fetch_rejects_a_different_uri(self):
        self.client.metadata_barrier = threading.Barrier(2)

        def send(uri):
            try:
                return uri, self.request('shared-request-id', uri), None
            except ValueError as error:
                return uri, None, error

        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            results = list(pool.map(send, [TRACK_A, TRACK_B]))
        successes = [item for item in results if item[2] is None]
        failures = [item for item in results if item[2] is not None]
        self.assertEqual(len(successes), 1, results)
        self.assertEqual(len(failures), 1, results)
        requested, response, _ = successes[0]
        self.assertEqual(response['request']['uri'], requested)
        self.assertEqual(self.party.db.execute('SELECT COUNT(*) FROM requests').fetchone()[0], 1)

    def test_retrying_one_request_does_not_enqueue_it_twice(self):
        first = self.request()
        second = self.request()
        self.assertEqual(first['request']['id'], second['request']['id'])
        event = self.party.db.execute("SELECT payload FROM events WHERE kind='request'").fetchone()
        self.assertEqual(json.loads(event['payload'])['id'], first['request']['id'])
        self.party._refresh()
        self.party._dispatch()
        self.party._dispatch()
        self.assertEqual(self.client.enqueue_calls, [(TRACK_A, 'request-a')])

    def test_removed_song_needs_grace_and_two_complete_observations(self):
        self.queued_request()
        self.client.queue = []
        self.party._refresh()
        self.assertEqual(self.row()['status'], 'queued')
        self.stamp += 60000
        self.party._refresh()
        self.assertEqual(self.row()['status'], 'removed')
        self.assertFalse(any(item['uri'] == TRACK_A for item in self.party.snapshot()['queue']))
        response = self.request('request-after-removal')
        self.assertEqual(response['status'], 'pending')
        self.assertEqual(response['request']['id'], 'request-after-removal')

    def test_one_complete_missing_observation_does_not_remove_an_old_request(self):
        self.queued_request()
        self.stamp += 60000
        self.client.queue = []
        self.party._refresh()
        self.assertEqual(self.row()['status'], 'queued')

    def test_incomplete_queue_observations_do_not_remove_a_request(self):
        self.queued_request()
        self.client.queue = []
        self.client.explicit_queue_complete = False
        self.stamp += 60000
        self.party._refresh()
        self.stamp += 60000
        self.party._refresh()
        self.assertEqual(self.row()['status'], 'queued')

    def test_known_request_uid_falls_back_when_queue_entry_has_no_uid(self):
        self.queued_request()
        self.client.queue = [track(TRACK_A)]
        for _ in range(2):
            self.stamp += 60000
            self.party._refresh()
        self.assertEqual(self.row()['status'], 'queued')

    def test_different_queue_uid_does_not_claim_the_original_request(self):
        self.queued_request()
        self.client.queue = [{**track(TRACK_A), 'uid': 'another-occurrence'}]
        for _ in range(2):
            self.stamp += 60000
            self.party._refresh()
        self.assertEqual(self.row()['status'], 'removed')

    def test_confirmed_pre_dispatch_failure_retries_after_recovery(self):
        self.request()
        self.party._refresh()
        self.client.enqueue_error = SpotifyError('Connection failed before dispatch', dispatched=False)
        self.party._dispatch()
        self.assertEqual(len(self.client.enqueue_calls), 1)
        self.client.enqueue_error = None
        self.stamp += 60000
        self.party._refresh()
        self.party._dispatch()
        self.assertEqual(len(self.client.enqueue_calls), 2)
        self.assertEqual(self.row()['status'], 'queued')

    def test_ambiguous_dispatch_is_not_retried_when_queue_is_empty(self):
        self.request()
        self.party._refresh()
        self.client.enqueue_error = SpotifyError('Reply lost after dispatch', dispatched=True)
        self.party._dispatch()
        self.client.enqueue_error = None
        for _ in range(3):
            self.stamp += 60000
            self.party._refresh()
            self.party._dispatch()
        self.assertEqual(len(self.client.enqueue_calls), 1)
        self.assertEqual(self.row()['status'], 'uncertain')

    def test_ambiguous_dispatch_is_resolved_when_the_song_appears(self):
        self.request()
        self.party._refresh()
        self.client.enqueue_error = SpotifyError('Reply lost after dispatch', dispatched=True)
        self.party._dispatch()
        self.client.enqueue_error = None
        self.client.queue = [track(TRACK_A)]
        self.party._refresh()
        self.party._dispatch()
        self.assertEqual(self.row()['status'], 'queued')
        self.assertEqual(len(self.client.enqueue_calls), 1)

    def finish_relinked_request(self, uid):
        self.client.queue = []
        self.client.current = {**track(TRACK_RELINKED), 'uid': uid, 'playing': True}
        self.party._refresh()
        self.assertEqual(self.row()['status'], 'playing')
        state = self.party.snapshot()
        self.assertFalse(any(item.get('id') == 'request-a' for item in state['queue']))
        self.assertEqual(state['nowPlaying']['uri'], TRACK_RELINKED)
        self.assertEqual(state['nowPlaying']['requestedUri'], TRACK_A)
        self.assertEqual(state['nowPlaying']['requestId'], 'request-a')
        self.assertEqual(state['nowPlaying']['requestedBy'], 'Test guest')
        self.stamp += 60000
        self.party._refresh()
        self.assertEqual(self.row()['status'], 'playing')
        self.client.current = {**track(TRACK_B), 'uid': 'next-track', 'playing': True}
        self.party._refresh()
        self.assertEqual(self.row()['status'], 'played')
        self.assertNotIn('requestedUri', self.party.snapshot()['nowPlaying'])

    def test_enqueue_uid_tracks_relinked_playback_through_completion(self):
        self.queued_request()
        uid = self.client.queue[0]['uid']
        self.assertEqual(json.loads(self.row()['track']).get('uid'), uid)
        self.finish_relinked_request(uid)
        self.assertEqual(len(self.client.enqueue_calls), 1)

    def test_ambiguous_reconciliation_keeps_uid_for_later_relinked_playback(self):
        self.request()
        self.party._refresh()
        self.client.enqueue_error = SpotifyError('Reply lost after dispatch', dispatched=True)
        self.party._dispatch()
        self.assertEqual(self.row()['status'], 'uncertain')
        self.client.enqueue_error = None
        self.client.queue = [{**track(TRACK_A), 'uid': 'observed-ambiguous-queue-item'}]
        self.party._refresh()
        self.assertEqual(self.row()['status'], 'queued')
        self.assertEqual(json.loads(self.row()['track']).get('uid'), 'observed-ambiguous-queue-item')
        self.finish_relinked_request('observed-ambiguous-queue-item')
        self.assertEqual(len(self.client.enqueue_calls), 1)

    def test_concurrent_message_retries_create_one_message_and_one_event(self):
        body = {'requestId': 'one-message-id', 'name': 'Guest', 'text': 'Play something joyful.'}
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            messages = list(pool.map(lambda _: self.party.message(body), range(2)))
        self.assertEqual(messages[0]['message']['id'], messages[1]['message']['id'])
        self.assertEqual(len(self.party.chat()['messages']), 1)
        self.assertEqual(self.party.db.execute("SELECT COUNT(*) FROM events WHERE kind='chat'").fetchone()[0], 1)

    def test_message_id_cannot_change_text_or_impersonate_a_reply(self):
        self.party.message({'requestId': 'one-message-id', 'text': 'Hello DJ'})
        with self.assertRaises(ValueError):
            self.party.message({'requestId': 'one-message-id', 'text': 'A different message'})
        with self.assertRaises(ValueError):
            self.party.message({'requestId': 'one-message-id', 'text': 'Hello DJ'}, role='assistant')

    def test_visual_act_rotates_after_45_minutes_without_restoring_legacy_geometry(self):
        self.party.admin('scene', {'audioStyle': 'city', 'mode': 'auto', 'effect': 7})
        started = self.stamp
        self.assertEqual(self.party.scene['effect'], 0)
        self.stamp += server.AUDIO_ACT_MS - 1
        self.party.admin('scene', {'palette': 'ice', 'effect': 6})
        with self.party.lock:
            self.party._advance_show()
        self.assertEqual(self.party.scene['audioStyle'], 'city')
        self.assertEqual(self.party.scene['effect'], 0)
        self.assertEqual(self.party.scene['audioActStartedAt'], started)
        self.stamp += 1
        with self.party.lock:
            self.party._advance_show()
        self.assertEqual(self.party.scene['audioStyle'], 'waterfall')
        self.assertEqual(self.party.scene['palette'], 'ice')
        self.assertEqual(self.party.scene['audioActStartedAt'], self.stamp)
        event = self.party.db.execute("SELECT payload FROM events WHERE kind='audio_act'").fetchone()
        self.assertEqual(json.loads(event[0])['style'], 'waterfall')

    def test_visual_act_schedule_survives_restart_and_changes_only_once_when_overdue(self):
        self.party.admin('scene', {'audioStyle': 'waterfall'})
        started = self.stamp
        self.stamp += 3 * server.AUDIO_ACT_MS
        reopened = server.Party(Path(self.directory.name), client=self.client)
        self.addCleanup(reopened.db.close)
        self.assertEqual(reopened.scene['audioActStartedAt'], started)
        with reopened.lock:
            reopened._advance_show()
            reopened._advance_show()
        self.assertEqual(reopened.scene['audioStyle'], 'city')
        self.assertEqual(reopened.scene['audioActStartedAt'], self.stamp)
        self.assertEqual(reopened.db.execute("SELECT COUNT(*) FROM events WHERE kind='audio_act'").fetchone()[0], 1)

    def test_ring_retains_geometry_controls_and_invalid_style_leaves_scene_intact(self):
        self.party.admin('scene', {'audioStyle': 'ring', 'effect': 6, 'mode': 'laser', 'palette': 'ice'})
        before = self.party.scene.copy()
        with self.assertRaises(ValueError):
            self.party.admin('scene', {'audioStyle': 'unknown', 'palette': 'orange'})
        self.assertEqual(self.party.scene, before)
        self.stamp += 2 * server.AUDIO_ACT_MS
        with self.party.lock:
            self.party._advance_show()
        self.assertEqual(self.party.scene['audioStyle'], 'ring')
        self.assertEqual(self.party.scene['effect'], 6)

    def test_karaoke_sheet_persists_and_follows_the_selected_spotify_occurrence(self):
        self.client.current = {**track(TRACK_A), 'uid': 'karaoke-occurrence',
                               'playing': True, 'progressMs': 1500, 'observedAt': self.stamp}
        self.party._refresh()
        self.party.admin('karaoke', karaoke_sheet())
        self.assertEqual(self.party.snapshot()['karaoke']['trackUid'], 'karaoke-occurrence')
        self.stamp += 250
        self.assertEqual(self.party.snapshot()['karaoke']['positionMs'], 1750)
        reopened = server.Party(Path(self.directory.name), client=self.client)
        self.addCleanup(reopened.db.close)
        reopened._refresh()
        self.assertEqual(reopened.snapshot()['karaoke']['positionMs'], 1750)
        self.client.current = {**self.client.current, 'uri': TRACK_B, 'uid': 'following-track'}
        reopened._refresh()
        self.assertEqual(reopened.snapshot()['karaoke']['status'], 'waiting')
        reopened.admin('karaoke', {'command': 'stop'})
        self.assertIsNone(reopened.snapshot()['karaoke'])
        self.assertIsNone(reopened.setting('karaoke'))

    def test_invalid_karaoke_replacement_preserves_the_persisted_sheet(self):
        self.party.admin('karaoke', karaoke_sheet())
        before = self.party.setting('karaoke')
        bad = karaoke_sheet()
        bad['lines'][0]['syllables'][0]['endMs'] = 0
        with self.assertRaises(ValueError):
            self.party.admin('karaoke', bad)
        self.assertEqual(self.party.setting('karaoke'), before)
        self.assertEqual(self.party.karaoke.saved(), before)

    def test_disconnected_controller_stops_presenting_an_old_track_as_current(self):
        self.party._refresh()
        self.assertIsNotNone(self.party.snapshot()['nowPlaying'])
        with patch.object(self.client, 'status', side_effect=SpotifyError('Browser closed')):
            self.party._refresh()
        state = self.party.snapshot()
        self.assertFalse(state['spotify']['connected'])
        self.assertIsNone(state['nowPlaying'])
        self.assertEqual(state['queue'], [])
        self.client.current = {**track(TRACK_A), 'playing': True}
        self.party._refresh()
        self.assertEqual(self.party.snapshot()['nowPlaying']['uri'], TRACK_A)

    def test_native_sized_messages_are_preserved_and_oversize_is_rejected(self):
        words = 'A' * 1499 + '!'
        result = self.party.message({'requestId': 'full-native-message', 'text': words})
        self.assertEqual(result['message']['text'], words)
        with self.assertRaises(ValueError):
            self.party.message({'requestId': 'too-long', 'text': words + 'x'})
        self.assertEqual(len(self.party.chat()['messages']), 1)

    def guest_messages(self, count):
        return [self.party.message({'requestId': f'guest-processing-{index}',
                'text': f'Guest request {index}'})['message']['id'] for index in range(count)]

    def test_processing_leases_renew_selected_ids_and_expire_independently(self):
        first, second = self.guest_messages(2)
        started = self.stamp
        result = self.party.admin('processing', {'messageIds': [second, first, first], 'active': True})
        self.assertEqual(result['dj']['processingMessageIds'], [first, second])
        published = json.loads((self.party.data / 'state.json').read_text())
        self.assertEqual(published['dj']['processingMessageIds'], [first, second])
        self.assertTrue(all(m['status'] == 'delivered' for m in self.party.chat()['messages']))
        self.stamp += server.PROCESSING_LEASE_MS // 2
        self.party.admin('processing', {'messageIds': [first], 'active': True})
        saved = self.party.setting('processingMessages')
        self.assertEqual(saved[str(first)], self.stamp + server.PROCESSING_LEASE_MS)
        self.assertEqual(saved[str(second)], started + server.PROCESSING_LEASE_MS)
        self.stamp = started + server.PROCESSING_LEASE_MS
        self.assertEqual(self.party.chat()['dj']['processingMessageIds'], [first])
        self.party.admin('reply', {'text': 'The request is being handled.', 'readThrough': second})
        self.assertEqual(self.party.chat()['dj']['processingMessageIds'], [first])
        for unused in range(2):
            result = self.party.admin('processing', {'messageIds': [first], 'active': False})
            self.assertEqual(result['dj']['processingMessageIds'], [])
        self.assertEqual(self.party.setting('processingMessages'), {})

    def test_processing_leases_survive_restart_and_expire_without_an_admin_write(self):
        message_id = self.guest_messages(1)[0]
        self.party.admin('processing', {'messageIds': [message_id], 'active': True})
        expires = self.stamp + server.PROCESSING_LEASE_MS
        self.stamp += 1000
        reopened = server.Party(self.party.data, client=self.client)
        self.addCleanup(reopened.db.close)
        self.assertEqual(reopened.chat()['dj']['processingMessageIds'], [message_id])
        self.stamp = expires - 1
        self.assertEqual(reopened.snapshot()['dj']['processingMessageIds'], [message_id])
        self.stamp = expires
        self.assertEqual(reopened.chat()['dj']['processingMessageIds'], [])
        self.assertEqual(reopened.snapshot()['dj']['processingMessageIds'], [])

    def test_invalid_processing_batch_changes_no_receipt_presence_or_settings(self):
        first, second = self.guest_messages(2)
        reply_id = self.party.message({'text': 'A DJ reply'}, role='assistant')['message']['id']
        self.party.admin('processing', {'messageIds': [first], 'active': True})
        def unchanged_state():
            return (self.party.processing_messages.copy(), self.party.setting('processingMessages'),
                    self.party.last_seen, self.party.revision,
                    (self.party.data / 'state.json').read_bytes(),
                    [tuple(row) for row in self.party.db.execute('SELECT id,read_at FROM chat ORDER BY id')])
        before = unchanged_state()
        self.stamp += 1000
        invalid = [
            {'messageIds': [second, reply_id + 1], 'active': True},
            {'messageIds': [reply_id], 'active': True},
            {'messageIds': [str(second)], 'active': True},
            {'messageIds': [True], 'active': True},
            {'messageIds': [float(second)], 'active': True},
            {'messageIds': [0], 'active': True},
            {'messageIds': [-1], 'active': True},
            {'messageIds': [2**63], 'active': True},
            {'messageIds': [], 'active': True},
            {'messageIds': [second] * (server.PROCESSING_LIMIT + 1), 'active': True},
            {'messageIds': second, 'active': True},
            {'messageIds': [second]},
            {'messageIds': [second], 'active': 1},
            {'active': False},
        ]
        for body in invalid:
            with self.subTest(body=body):
                with self.assertRaises(ValueError):
                    self.party.admin('processing', {**body, 'readThrough': second})
                self.assertEqual(unchanged_state(), before)

    def test_processing_capacity_rejects_overflow_then_prunes_expired_leases(self):
        identifiers = self.guest_messages(server.PROCESSING_LIMIT + 1)
        self.party.admin('processing', {'messageIds': identifiers[:-1], 'active': True})
        saved = self.party.setting('processingMessages')
        last_seen = self.party.last_seen
        self.stamp += 1000
        with self.assertRaises(ValueError):
            self.party.admin('processing', {'messageIds': [identifiers[-1]], 'active': True,
                                            'readThrough': identifiers[-1]})
        self.assertEqual(self.party.setting('processingMessages'), saved)
        self.assertEqual(self.party.last_seen, last_seen)
        self.assertTrue(all(m['status'] == 'delivered' for m in self.party.chat()['messages']))
        self.stamp += server.PROCESSING_LEASE_MS
        result = self.party.admin('processing', {'messageIds': [identifiers[-1]], 'active': True})
        self.assertEqual(result['dj']['processingMessageIds'], [identifiers[-1]])
        self.assertEqual(len(self.party.setting('processingMessages')), 1)

    def test_processing_http_requires_admin_and_cli_updates_an_empty_chat_poll(self):
        message_id = self.guest_messages(1)[0]
        class QuietHandler(server.Handler):
            def log_message(self, *args):
                pass
        http = server.ThreadingHTTPServer(('127.0.0.1', 0), QuietHandler)
        http.party = self.party
        http.daemon_threads = True
        worker = threading.Thread(target=lambda: http.serve_forever(poll_interval=.05), daemon=True)
        worker.start()
        base = f'http://127.0.0.1:{http.server_port}'
        def request(path, body=None, token=None):
            headers = {'Content-Type': 'application/json'}
            if token:
                headers['Authorization'] = 'Bearer ' + token
            data = None if body is None else json.dumps(body).encode()
            try:
                response = urllib.request.urlopen(urllib.request.Request(base + path, data, headers), timeout=5)
            except urllib.error.HTTPError as error:
                response = error
            with response:
                return response.status, json.load(response)
        try:
            body = {'messageIds': [message_id], 'active': True}
            status, unused = request('/api/admin/processing', body)
            self.assertEqual(status, 401)
            self.assertEqual(self.party.last_seen, 0)
            self.assertEqual(self.party.chat()['dj']['processingMessageIds'], [])
            command = subprocess.run([sys.executable, str(server.ROOT / 'djctl.py'), 'processing',
                '--json', json.dumps(body), '--url', base, '--data', str(self.party.data)],
                capture_output=True, text=True, timeout=10)
            self.assertEqual(command.returncode, 0, command.stderr)
            self.assertEqual(json.loads(command.stdout)['dj']['processingMessageIds'], [message_id])
            status, chat = request(f'/api/chat?after={message_id}')
            self.assertEqual(status, 200)
            self.assertEqual(chat['messages'], [])
            self.assertEqual(chat['dj']['processingMessageIds'], [message_id])
            status, unused = request('/api/admin/processing',
                {'messageIds': [message_id, message_id + 1], 'active': False}, self.party.admin_token)
            self.assertEqual(status, 400)
            self.assertEqual(self.party.chat()['dj']['processingMessageIds'], [message_id])
            status, cleared = request('/api/admin/processing',
                {'messageIds': [message_id], 'active': False}, self.party.admin_token)
            self.assertEqual(status, 200)
            self.assertEqual(cleared['dj']['processingMessageIds'], [])
        finally:
            http.shutdown()
            http.server_close()
            worker.join(timeout=2)


if __name__ == '__main__':
    unittest.main()
