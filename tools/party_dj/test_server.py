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


def observation(stamp, *, uri=TRACK_B, uid='current-occurrence', paused=False, buffering=False):
    return {'nowPlaying': {**track(uri), 'uid': uid, 'playing': not paused and not buffering,
                           'paused': paused, 'buffering': buffering, 'progressMs': 12000,
                           'observedAt': stamp},
            'queue': [], 'explicitQueueComplete': True, 'observedAt': stamp,
            'playbackState': {'source': 'harmony', 'paused': paused, 'buffering': buffering,
                              'positionMs': 12000, 'sourceTimestamp': stamp,
                              'device': {'id': 'fixture-device', 'name': 'Test player'}}}


class FakeSpotify:
    mode = 'fixture'

    def __init__(self):
        self.queue = []
        self.current = {**track(TRACK_B), 'playing': True}
        self.explicit_queue_complete = True
        self.enqueue_calls = []
        self.enqueue_error = None
        self.metadata_barrier = None
        self.control_calls = []
        self.control_result = None

    def track(self, uri):
        if self.metadata_barrier is not None:
            self.metadata_barrier.wait(timeout=5)
        return track(uri)

    def status(self):
        return copy.deepcopy({'nowPlaying': self.current, 'queue': self.queue,
                              'explicitQueueComplete': self.explicit_queue_complete})

    def control(self, action):
        self.control_calls.append(action)
        return copy.deepcopy(self.control_result if self.control_result is not None
                             else {'ok': True, 'action': action})

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

    def queue_events(self):
        return [json.loads(row['payload']) for row in self.party.db.execute(
            "SELECT payload FROM events WHERE kind='queue_changed' ORDER BY id")]

    def test_initial_queue_observation_establishes_baseline_without_change_event(self):
        for queue in ([], [{**track(TRACK_A), 'uid': 'first-occurrence'}]):
            with self.subTest(queue=queue):
                client = FakeSpotify()
                client.queue = queue
                party = server.Party(self.party.data / f'initial-{len(queue)}', client=client)
                self.addCleanup(party.db.close)
                party._refresh()
                initial = party.events()
                self.assertEqual([event['kind'] for event in initial['events']], ['track', 'spotify'])
                self.assertEqual(initial['state']['queue'], [{**item, 'status': 'queued'} for item in queue])
                party._refresh()
                self.assertEqual(party.events(after=initial['lastId'])['events'], [])

    def test_queue_changes_emit_ordered_actual_occurrences_for_reorder_add_and_remove(self):
        first = {'uid': 'first', 'uri': TRACK_A, 'source': 'queue'}
        second = {'uid': 'second', 'uri': TRACK_B, 'source': 'queue'}
        third = {'uid': 'third', 'uri': TRACK_RELINKED, 'source': 'context'}
        self.client.queue = [first, second]
        self.party._refresh()
        expected = []
        for queue in ([second, first], [second, first, third], [first, third], []):
            with self.subTest(queue=queue):
                self.client.queue = copy.deepcopy(queue)
                self.party._refresh()
                expected.append({'queue': queue})
                self.assertEqual(self.queue_events(), expected)
                self.party._refresh()
                self.assertEqual(self.queue_events(), expected)
        self.assertEqual(self.client.control_calls, [])
        self.assertEqual(self.client.enqueue_calls, [])

    def test_queue_changes_distinguish_duplicate_uids_and_no_uid_multiplicity(self):
        first = {'uid': 'repeat-1', 'uri': TRACK_A, 'source': 'queue'}
        second = {'uid': 'repeat-2', 'uri': TRACK_A, 'source': 'queue'}
        replacement = {'uid': 'repeat-3', 'uri': TRACK_A, 'source': 'queue'}
        unidentified = {'uid': None, 'uri': TRACK_A, 'source': 'queue'}
        self.client.queue = [first, second]
        self.party._refresh()
        expected = []
        cases = [([second, first], [second, first]),
                 ([second, replacement], [second, replacement]),
                 ([replacement], [replacement]),
                 ([track(TRACK_A)], [unidentified]),
                 ([track(TRACK_A), track(TRACK_A)], [unidentified, unidentified]),
                 ([track(TRACK_A)], [unidentified])]
        for queue, occurrences in cases:
            with self.subTest(queue=queue):
                self.client.queue = queue
                self.party._refresh()
                expected.append({'queue': occurrences})
                self.assertEqual(self.queue_events(), expected)

    def test_queue_changes_include_uri_and_source_but_ignore_observation_metadata(self):
        observed = observation(self.stamp)
        observed['queue'] = [{**track(TRACK_A), 'uid': 'same-occurrence'}]
        with patch.object(self.client, 'status', return_value=observed):
            self.party._refresh()
            last_id = self.party.events()['lastId']
            self.stamp += 2500
            observed['observedAt'] = self.stamp
            observed['nowPlaying'].update(progressMs=14500, observedAt=self.stamp, title='Updated title')
            observed['playbackState'].update(positionMs=14500, sourceTimestamp=self.stamp)
            observed['queue'][0].update(title='New metadata', artist='New artist',
                                        albumArt='fixture-art', requestedBy='Guest')
            self.party._refresh()
            self.assertEqual(self.party.events(after=last_id)['events'], [])
            self.assertEqual(self.party.snapshot()['nowPlaying']['progressMs'], 14500)
            expected = []
            for uri, source in [(TRACK_RELINKED, 'queue'), (TRACK_RELINKED, 'context'),
                                (TRACK_RELINKED, None)]:
                observed['queue'][0]['uri'] = uri
                if source is None:
                    observed['queue'][0].pop('source')
                else:
                    observed['queue'][0]['source'] = source
                self.party._refresh()
                expected.append({'queue': [{'uid': 'same-occurrence', 'uri': uri, 'source': source}]})
                self.assertEqual(self.queue_events(), expected)

    def test_pending_request_overlays_do_not_emit_actual_queue_changes(self):
        self.party._refresh()
        self.request()
        self.assertEqual(self.party.snapshot()['queue'][0]['id'], 'request-a')
        last_id = self.party.events()['lastId']
        self.party._refresh()
        self.assertEqual(self.party.events(after=last_id)['events'], [])
        self.assertEqual(self.queue_events(), [])
        self.assertEqual(self.client.queue, [])
        self.assertEqual(self.client.enqueue_calls, [])

    def test_disconnect_retains_queue_baseline_for_same_and_changed_reconnect(self):
        first = {'uid': 'first', 'uri': TRACK_A, 'source': 'queue'}
        second = {'uid': 'second', 'uri': TRACK_B, 'source': 'queue'}
        self.client.queue = [first]
        self.party._refresh()
        last_id = self.party.events()['lastId']
        for queue, changed in [([first], False), ([second], True)]:
            with self.subTest(queue=queue):
                with patch.object(self.client, 'status', side_effect=SpotifyError('Disconnected')):
                    self.party._refresh()
                    self.party._refresh()
                disconnected = self.party.events(after=last_id)
                self.assertEqual([event['kind'] for event in disconnected['events']], ['spotify'])
                self.assertFalse(disconnected['state']['spotify']['connected'])
                self.assertEqual(disconnected['state']['queue'], [])
                self.client.queue = queue
                self.party._refresh()
                reconnected = self.party.events(after=disconnected['lastId'])
                self.assertEqual([event['kind'] for event in reconnected['events']],
                                 ['spotify', 'queue_changed'] if changed else ['spotify'])
                self.assertEqual(reconnected['state']['queue'], [{**item, 'status': 'queued'} for item in queue])
                if changed:
                    self.assertEqual(reconnected['events'][-1]['payload'], {'queue': queue})
                last_id = reconnected['lastId']
        self.assertEqual(self.queue_events(), [{'queue': [second]}])

    def test_queue_change_wakes_existing_event_wait_with_updated_state(self):
        first = {'uid': 'first', 'uri': TRACK_A, 'source': 'queue'}
        second = {'uid': 'second', 'uri': TRACK_B, 'source': 'queue'}
        self.client.queue = [first, second]
        self.party._refresh()
        last_id = self.party.events()['lastId']
        waiting = threading.Event()
        condition_wait = self.party.changed.wait

        def signal_wait(timeout=None):
            waiting.set()
            return condition_wait(timeout)

        with patch.object(self.party.changed, 'wait', side_effect=signal_wait):
            with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
                pending = pool.submit(self.party.events, after=last_id, wait=45)
                try:
                    self.assertTrue(waiting.wait(timeout=5))
                    self.stamp += 2500
                    self.client.current.update(progressMs=2500, observedAt=self.stamp)
                    self.party._refresh()
                    self.assertEqual(self.party.events(after=last_id)['events'], [])
                    self.assertFalse(pending.done())
                    self.client.queue = [second, first]
                    self.party._refresh()
                    result = pending.result(timeout=5)
                    self.assertEqual([event['kind'] for event in result['events']], ['queue_changed'])
                    self.assertEqual(result['events'][0]['payload'], {'queue': [second, first]})
                    self.assertEqual(result['state']['queue'],
                                     [{**item, 'status': 'queued'} for item in [second, first]])
                finally:
                    with self.party.changed:
                        self.party.stop.set()
                        self.party.changed.notify_all()
                    pending.result(timeout=5)

    def test_transport_publishes_verified_pause_and_resume_before_returning(self):
        for action, paused in [('pause', True), ('resume', False)]:
            with self.subTest(action=action):
                old = observation(self.stamp, paused=not paused)
                self.client.current = old['nowPlaying']
                self.party._refresh()
                with self.party.lock:
                    self.party._publish()
                fresh = observation(self.stamp + 1000, paused=paused)
                self.client.control_result = {'ok': True, 'action': action, 'status': fresh}
                calls_before = len(self.client.control_calls)
                with patch.object(self.client, 'status', wraps=self.client.status) as status:
                    result = self.party.admin(action, {})
                status.assert_not_called()
                self.assertEqual(self.client.control_calls[calls_before:], [action])
                self.assertEqual(result, self.client.control_result)
                self.assertEqual(self.party.playback, fresh)
                self.assertEqual(self.party.snapshot()['nowPlaying'], fresh['nowPlaying'])
                published = json.loads((self.party.data / 'state.json').read_text())
                self.assertEqual(published['nowPlaying'], fresh['nowPlaying'])
                self.assertTrue(published['spotify']['connected'])

    def test_verified_transport_reconciles_relinked_uid_and_request_metadata(self):
        self.party.request({'requestId': 'request-a', 'uri': TRACK_A,
                            'requestedBy': 'Test guest', 'dedication': 'For the dancers'})
        self.party._refresh()
        self.party._dispatch()
        self.party._refresh()
        uid = self.client.queue[0]['uid']
        fresh = observation(self.stamp + 1000, uri=TRACK_RELINKED, uid=uid, paused=True)
        fresh['queue'] = [{**track(TRACK_A), 'uid': 'another-occurrence'}]
        self.client.control_result = {'ok': True, 'action': 'pause', 'status': fresh}
        self.party.admin('pause', {})
        self.assertEqual(self.row()['status'], 'playing')
        state = self.party.snapshot()
        current = state['nowPlaying']
        self.assertEqual(current['uri'], TRACK_RELINKED)
        self.assertEqual(current['uid'], uid)
        self.assertEqual(current['requestedUri'], TRACK_A)
        self.assertEqual(current['requestId'], 'request-a')
        self.assertEqual(current['requestedBy'], 'Test guest')
        self.assertEqual(current['dedication'], 'For the dancers')
        self.assertEqual(state['queue'][0]['uid'], 'another-occurrence')
        self.assertNotEqual(state['queue'][0]['id'], 'request-a')
        self.assertEqual(json.loads((self.party.data / 'state.json').read_text())['nowPlaying'], current)
        self.assertEqual(self.client.control_calls, ['pause'])
        self.assertEqual(len(self.client.enqueue_calls), 1)

    def test_next_observes_buffering_without_claiming_a_confirmed_skip(self):
        old = observation(self.stamp)
        self.client.current = old['nowPlaying']
        self.party._refresh()
        fresh = observation(self.stamp + 1000, buffering=True)
        with patch.object(self.client, 'status', return_value=fresh) as status:
            result = self.party.admin('next', {})
        status.assert_called_once_with()
        self.assertEqual(result, {'ok': True, 'action': 'next'})
        self.assertEqual(self.client.control_calls, ['next'])
        self.assertEqual(fresh['nowPlaying']['uid'], old['nowPlaying']['uid'])
        self.assertEqual(self.party.snapshot()['nowPlaying'], fresh['nowPlaying'])
        self.assertEqual(self.party.playback['playbackState'], fresh['playbackState'])
        published = json.loads((self.party.data / 'state.json').read_text())
        self.assertEqual(published['nowPlaying'], fresh['nowPlaying'])

    def test_failed_post_transport_observation_does_not_repeat_transport(self):
        self.party._refresh()
        with patch.object(self.client, 'status', side_effect=SpotifyError('Observation lost')) as status:
            result = self.party.admin('next', {})
        status.assert_called_once_with()
        self.assertEqual(result, {'ok': True, 'action': 'next'})
        self.assertEqual(self.client.control_calls, ['next'])
        published = json.loads((self.party.data / 'state.json').read_text())
        self.assertFalse(published['spotify']['connected'])
        self.assertEqual(published['spotify']['error'], 'Observation lost')
        self.assertIsNone(published['nowPlaying'])
        self.assertEqual(published['queue'], [])

    def test_late_poll_result_or_error_cannot_replace_verified_transport_state(self):
        for error in (None, SpotifyError('Old poll failed')):
            with self.subTest(error=error):
                old = observation(self.stamp)
                old['queue'] = [{'uid': 'old-occurrence', 'uri': TRACK_A, 'source': 'queue'}]
                self.client.current = old['nowPlaying']
                self.client.queue = old['queue']
                self.party._refresh()
                fresh = observation(self.stamp + 1000, paused=True)
                fresh['queue'] = [{'uid': 'new-occurrence', 'uri': TRACK_B, 'source': 'queue'}]
                self.client.control_result = {'ok': True, 'action': 'pause', 'status': fresh}
                last_id = self.party.events()['lastId']
                observed = threading.Event()
                release = threading.Event()

                def delayed_status():
                    observed.set()
                    if not release.wait(timeout=5):
                        raise AssertionError('The test did not release the old observation.')
                    if error is not None:
                        raise error
                    return old

                with patch.object(self.client, 'status', side_effect=delayed_status) as status:
                    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
                        pending = pool.submit(self.party._refresh)
                        try:
                            self.assertTrue(observed.wait(timeout=5))
                            with self.party.lock:
                                release.set()
                                self.party.admin('pause', {})
                        finally:
                            release.set()
                        pending.result(timeout=5)
                status.assert_called_once_with()
                self.assertEqual(self.party.snapshot()['nowPlaying'], fresh['nowPlaying'])
                self.assertEqual(self.party.snapshot()['queue'],
                                 [{**item, 'status': 'queued'} for item in fresh['queue']])
                queue_events = [event for event in self.party.events(after=last_id)['events']
                                if event['kind'] == 'queue_changed']
                self.assertEqual([event['payload'] for event in queue_events], [{'queue': fresh['queue']}])
                last_id = self.party.events()['lastId']
                with patch.object(self.client, 'status', return_value=fresh):
                    self.party._refresh()
                self.assertEqual(self.party.events(after=last_id)['events'], [])
                self.assertTrue(self.party.spotify['connected'])
                self.assertIsNone(self.party.spotify['error'])
                with self.party.lock:
                    self.party._publish()
                published = json.loads((self.party.data / 'state.json').read_text())
                self.assertEqual(published['nowPlaying'], fresh['nowPlaying'])
                self.assertTrue(published['spotify']['connected'])

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

    def removed_request(self):
        self.queued_request()
        occurrence = self.client.queue[0].copy()
        self.client.queue = []
        self.party._refresh()
        self.stamp += 60000
        self.party._refresh()
        self.assertEqual(self.row()['status'], 'removed')
        return occurrence

    def test_exact_queue_occurrence_restores_a_removed_request_without_resending(self):
        occurrence = self.removed_request()
        self.client.queue = [occurrence]
        self.party._refresh()
        self.assertEqual(self.row()['status'], 'queued')
        queued = next(item for item in self.party.snapshot()['queue'] if item.get('uid') == occurrence['uid'])
        self.assertEqual(queued['id'], 'request-a')
        self.assertEqual(queued['requestedBy'], 'Test guest')
        self.party._refresh()
        self.party._dispatch()
        self.assertEqual(len(self.client.enqueue_calls), 1)
        restored = self.party.db.execute("SELECT payload FROM events WHERE kind='queue_restored'").fetchall()
        self.assertEqual(len(restored), 1)
        self.assertEqual(json.loads(restored[0][0])['uid'], occurrence['uid'])

    def test_removed_request_recovers_when_its_relinked_occurrence_is_now_playing(self):
        occurrence = self.removed_request()
        self.finish_relinked_request(occurrence['uid'])
        self.assertEqual(len(self.client.enqueue_calls), 1)

    def test_removed_request_is_not_restored_by_uri_without_its_exact_uid(self):
        self.removed_request()
        for candidate in ({**track(TRACK_A), 'uid': 'new-occurrence'}, track(TRACK_A)):
            self.client.queue = [candidate]
            self.party._refresh()
            self.assertEqual(self.row()['status'], 'removed')
            self.assertNotEqual(self.party.snapshot()['queue'][0]['id'], 'request-a')
        self.assertEqual(self.party.db.execute("SELECT COUNT(*) FROM events WHERE kind='queue_restored'").fetchone()[0], 0)

    def test_restored_queue_uid_does_not_claim_an_untagged_current_repeat(self):
        occurrence = self.removed_request()
        self.client.queue = [occurrence]
        self.client.current = {**track(TRACK_A), 'playing': True}
        for unused in range(2):
            self.party._refresh()
            self.assertEqual(self.row()['status'], 'queued')
            self.assertNotIn('requestId', self.party.snapshot()['nowPlaying'])
            self.assertEqual(self.party.snapshot()['queue'][0]['id'], 'request-a')

    def test_restored_and_rerequested_occurrences_keep_separate_queue_metadata(self):
        first = self.removed_request()
        self.request('request-repeat')
        self.party._dispatch()
        self.party._refresh()
        second = self.client.queue[0].copy()
        self.assertNotEqual(first['uid'], second['uid'])
        self.client.queue = [first, second]
        self.party._refresh()
        queue = self.party.snapshot()['queue']
        self.assertEqual([(item['uid'], item['id']) for item in queue],
                         [(first['uid'], 'request-a'), (second['uid'], 'request-repeat')])
        self.assertTrue(all(item['requestedBy'] == 'Test guest' for item in queue))
        self.party._dispatch()
        self.assertEqual(len(self.client.enqueue_calls), 2)

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

    def test_visual_act_rotates_after_10_minutes_without_restoring_legacy_geometry(self):
        self.assertEqual(server.AUDIO_ACT_MS, 600000)
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

    def book_fixture(self):
        directory = self.party.data / 'book'
        directory.mkdir()
        filename = 'fixture-photo.jpg'
        # The server checks existence; these tests never decode or render images.
        (directory / filename).write_bytes(b'fixture-image')
        (directory / 'cards.json').write_text(json.dumps([
            {'path': filename, 'kind': 'photo', 'caption': 'Fixture photo'}]))
        return filename

    def test_book_enabled_defaults_true_and_disable_clears_and_persists(self):
        filename = self.book_fixture()
        self.assertTrue(self.party.snapshot()['bookEnabled'])
        self.assertTrue(self.party.admin('configure', {})['bookEnabled'])
        self.party.admin('book', {'filename': filename})
        self.assertIsNotNone(self.party.snapshot()['bookMoment'])
        result = self.party.admin('configure', {'bookEnabled': False})
        self.assertFalse(result['bookEnabled'])
        self.assertFalse(self.party.setting('bookEnabled'))
        self.assertFalse(self.party.snapshot()['bookEnabled'])
        self.assertIsNone(self.party.book_moment)
        published = json.loads((self.party.data / 'state.json').read_text())
        self.assertFalse(published['bookEnabled'])
        self.assertIsNone(published['bookMoment'])
        reopened = server.Party(self.party.data, client=self.client)
        self.addCleanup(reopened.db.close)
        self.assertFalse(reopened.snapshot()['bookEnabled'])
        self.assertFalse(reopened.admin('configure', {})['bookEnabled'])
        self.assertIsNone(reopened.snapshot()['bookMoment'])

    def test_disabled_book_blocks_automatic_and_manual_display_until_reenabled(self):
        filename = self.book_fixture()
        self.party.admin('configure', {'bookEnabled': False})
        self.stamp += 106000
        with patch.object(self.party, '_book_cards', wraps=self.party._book_cards) as cards:
            with self.party.lock:
                self.party._advance_show()
            cards.assert_not_called()
        self.assertIsNone(self.party.book_moment)
        with self.assertRaisesRegex(ValueError, 'disabled'):
            self.party.admin('book', {'filename': filename})
        self.assertIsNone(self.party.book_moment)
        self.assertFalse(self.party._show_book({'path': filename}))
        self.assertIsNone(self.party.book_moment)
        self.assertTrue(self.party.admin('configure', {'bookEnabled': True})['bookEnabled'])
        self.assertTrue(self.party.setting('bookEnabled'))
        manual = self.party.admin('book', {'filename': filename})
        self.assertEqual(Path(manual['bookMoment']['imagePath']).name, filename)
        self.stamp += 106000
        with self.party.lock:
            self.party._advance_show()
        self.assertEqual(self.party.book_moment['createdAt'], self.stamp)
        self.assertEqual(Path(self.party.book_moment['imagePath']).name, filename)

    def test_invalid_book_enabled_leaves_book_and_other_configuration_unchanged(self):
        filename = self.book_fixture()
        self.party.admin('book', {'filename': filename})

        def unchanged_state():
            return (self.party.setting('bookEnabled'), self.party.snapshot()['bookEnabled'],
                    copy.deepcopy(self.party.book_moment), self.party.setting('speechEnabled'),
                    self.party.request_url, self.party.setting('requestUrl'), self.party.revision,
                    (self.party.data / 'state.json').read_bytes())

        before = unchanged_state()
        with patch.object(self.party, '_write_qr') as write_qr:
            for invalid in (None, 0, 1, 'false', 'true', [], {}):
                with self.subTest(value=invalid):
                    with self.assertRaisesRegex(ValueError, 'bookEnabled'):
                        self.party.admin('configure', {'bookEnabled': invalid, 'speechEnabled': False,
                                                      'requestUrl': 'https://fixture.invalid/changed'})
                    self.assertEqual(unchanged_state(), before)
            write_qr.assert_not_called()

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
