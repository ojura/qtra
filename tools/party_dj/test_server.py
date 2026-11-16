"""Request and conversation regressions using isolated state and no Spotify session."""
from __future__ import annotations

import concurrent.futures
import copy
import json
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch

import server
from spotify_native import SpotifyError


TRACK_A = 'spotify:track:' + 'a' * 22
TRACK_B = 'spotify:track:' + 'b' * 22


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
        self.queue.append(track(uri))
        return {'queued': True, 'uri': uri, 'requestId': request_id}


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


if __name__ == '__main__':
    unittest.main()
