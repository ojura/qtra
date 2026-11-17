"""Operational DJ desk fixtures: no Spotify, browser, audio or live service calls."""
from __future__ import annotations

import contextlib
import copy
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import MagicMock, patch
from urllib.error import HTTPError, URLError

import desk


NOW = 1_000_000
URI_A = 'spotify:track:' + 'a' * 22
URI_B = 'spotify:track:' + 'b' * 22
URI_C = 'spotify:track:' + 'c' * 22


def track(uid, uri=URI_A, **fields):
    return {'uid': uid, 'uri': uri, 'title': uid, 'artist': 'Fixture',
            'source': 'queue', **fields}


def observation(queue=()):
    current = track('current', URI_B, playing=True, progressMs=42000, observedAt=NOW)
    device = {'id': 'native-device', 'name': 'Standalone Spotify'}
    observed = {'observedAt': NOW, 'explicitQueueComplete': True,
                'nowPlaying': current, 'playbackState': {'device': device},
                'queue': list(queue)}
    state = {'spotify': {'connected': True, 'observedAt': NOW, 'stale': False},
             'nowPlaying': copy.deepcopy(current), 'playbackState': {'device': device},
             'queue': copy.deepcopy(list(queue)), 'withdrawnMessageIds': [], 'dj': {}}
    return {'observation': observed, 'state': state, 'recentTracks': [], 'lastId': 10}


class ScriptedClient:
    """Each read/write must match the test's explicit sequence exactly once."""
    def __init__(self, *steps):
        self.steps = list(steps)
        self.calls = []

    def call(self, path, body=None, timeout=None):
        self.calls.append((path, copy.deepcopy(body)))
        if not self.steps:
            raise AssertionError('Unexpected service call: ' + path)
        expected, result = self.steps.pop(0)
        if path != expected:
            raise AssertionError(f'Expected {expected}, got {path}')
        if isinstance(result, Exception):
            raise result
        return copy.deepcopy(result)

    def sync(self):
        return self.call('/api/admin/sync', {})


class DeskTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.data = Path(self.temp.name)
        self.cursor = self.data / 'cursor.json'
        (self.data / 'admin-token').write_text('fixture-only-token')
        clock = patch.object(desk, 'now_ms', return_value=NOW)
        clock.start()
        self.addCleanup(clock.stop)
        offline = patch.object(desk.urllib.request, 'urlopen',
            side_effect=AssertionError('A desk test attempted real HTTP.'))
        offline.start()
        self.addCleanup(offline.stop)
        native = patch.object(desk, 'AgentClient', side_effect=AssertionError('A desk test attempted the real native socket.'))
        native.start()
        self.addCleanup(native.stop)

    def native_fixture(self, result, *, pid=731, module_name='LARICS / LYRICS party stage', extra_modules=()):
        (self.data / 'qtra-modules.json').write_text(json.dumps({
            'pid': 731, 'socket': str(self.data / 'fixture.sock'), 'modules': {'party_stage': '55'}}))
        client = MagicMock()
        client.__enter__.return_value = client
        responses = {'hello': {'pid': pid, 'uid': desk.os.getuid(), 'name': 'qt-runtime-cube'},
                     'module.list': [{'id': '55', 'name': module_name}, *extra_modules],
                     'snippet.run': {'operationId': 'fixture-op'}}
        client.request.side_effect = lambda command, params: copy.deepcopy(responses[command])
        client.wait_for_operation.return_value = {'data': {'outcome': 'completed', 'result': result}}
        return client

    def test_presence_is_one_explicit_write_and_lost_response_is_not_repeated(self):
        client = ScriptedClient(('/api/admin/presence', {'ok': True}))
        with patch.object(desk, 'ServiceClient', return_value=client), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(desk.main(['presence', 'online', '--agent-session', 'actual-dj.1', '--sequence', '7']), 0)
        self.assertEqual(client.calls, [('/api/admin/presence', {
            'agentSessionId': 'actual-dj.1', 'sequence': 7, 'state': 'online', 'leaseSeconds': 60})])
        lost = ScriptedClient(('/api/admin/presence', desk.DeskError({'error': 'Response lost', 'responseLost': True})))
        with self.assertRaises(desk.DeskError):
            desk.set_presence(lost, 'offline', 'actual-dj.1', 8)
        self.assertEqual(len(lost.calls), 1)

    def test_invalid_presence_is_rejected_before_any_service_call(self):
        for fields in [('online', '', 1, 60), ('online', 'space id', 1, 60), ('online', '_dj', 1, 60),
                       ('watching', 'dj', 1, 60), ('online', 'dj', True, 60),
                       ('online', 'dj', 0, 60), ('online', 'dj', 2**53, 60),
                       ('online', 'dj', 1, 29), ('online', 'dj', 1, 121)]:
            with self.subTest(fields=fields):
                client = ScriptedClient()
                with self.assertRaises(desk.DeskError):
                    desk.set_presence(client, *fields)
                self.assertFalse(client.calls)

    def test_watcher_exposes_separate_presence_without_renewing_it(self):
        state = observation()['state']
        state['dj'] = {'status': 'away', 'lastSeen': 0,
                       'llm': {'status': 'offline', 'remainingMs': 0, 'activeCount': 0},
                       'watcher': {'status': 'polling', 'remainingMs': 90000}}
        client = ScriptedClient(('/api/admin/events?after=10&wait=0',
                                {'state': state, 'events': [], 'lastId': 10}))
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            desk.watch(client, after=10, once=True, wait=0)
        self.assertEqual(json.loads(output.getvalue())['state']['dj'], state['dj'])
        self.assertEqual(client.calls, [('/api/admin/events?after=10&wait=0', None)])

    def test_composer_save_and_intent_preserve_the_exact_draft_binding(self):
        draft = {'guestId': 'fixture-guest', 'composerId': 'fixture-editor', 'draftRevision': 4,
                 'text': '  Reci DJ-u\nbez pretrage  ', 'editorMode': 'message',
                 'cursor': 8, 'anchor': 3, 'selectedTrack': {}, 'alternateDraft': {'text': 'Other draft'}}
        client = self.native_fixture({'composerDraft': draft})
        saved = self.data / 'draft.json'
        with patch.object(desk, 'AgentClient', return_value=client), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(desk.main(['--data', str(self.data), 'composer', '--save', str(saved)]), 0)
        self.assertEqual(json.loads(saved.read_text()), draft)
        self.assertEqual(client.request.call_args_list[-1].args, ('snippet.run', {
            'moduleId': '55', 'executor': 'gui', 'request': {'getComposerDraft': True}}))
        setter = self.native_fixture({'searchIntentResult': {'applied': True, 'effectiveIntent': 'chat'}})
        with patch.object(desk, 'AgentClient', return_value=setter), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(desk.main(['--data', str(self.data), 'intent', 'chat', '--draft', str(saved)]), 0)
        payload = setter.request.call_args_list[-1].args[1]
        self.assertEqual(payload, {'moduleId': '55', 'executor': 'gui', 'request': {
            'setSearchIntent': {key: draft[key] for key in desk.COMPOSER_FIELDS} | {'intent': 'chat'}}})
        self.assertEqual([call.args[0] for call in setter.request.call_args_list], ['hello', 'module.list', 'snippet.run'])

    def test_stale_intent_is_reported_without_reading_or_rebinding_a_new_draft(self):
        draft = {'guestId': 'g', 'composerId': 'editor-A', 'draftRevision': 3, 'text': 'A', 'editorMode': 'message'}
        saved = self.data / 'draft.json'; saved.write_text(json.dumps(draft))
        client = self.native_fixture({'searchIntentResult': {'applied': False, 'error': 'Composer changed.'}})
        output = io.StringIO()
        with patch.object(desk, 'AgentClient', return_value=client), contextlib.redirect_stdout(output):
            self.assertEqual(desk.main(['--data', str(self.data), 'intent', 'music', '--draft', str(saved)]), 1)
        self.assertEqual(json.loads(output.getvalue()), {'applied': False, 'error': 'Composer changed.'})
        self.assertEqual(client.wait_for_operation.call_count, 1)
        self.assertEqual([call.args[0] for call in client.request.call_args_list], ['hello', 'module.list', 'snippet.run'])

    def test_stale_native_process_or_module_stops_before_invocation(self):
        for fields in ({'pid': 732}, {'module_name': 'Unrelated module'}):
            with self.subTest(fields=fields):
                client = self.native_fixture({}, **fields)
                with patch.object(desk, 'AgentClient', return_value=client), self.assertRaises(desk.DeskError):
                    desk.read_composer(self.data)
                self.assertNotIn('snippet.run', [call.args[0] for call in client.request.call_args_list])
                client.wait_for_operation.assert_not_called()

    def test_incomplete_binding_and_old_native_response_are_rejected(self):
        for draft in ({'text': 'Legacy'}, {'guestId': 'g', 'composerId': 'c', 'draftRevision': True,
                                         'text': 'A', 'editorMode': 'message'}):
            with self.subTest(draft=draft), self.assertRaises(desk.DeskError):
                desk.set_search_intent(self.data, draft, 'chat')
        client = self.native_fixture({'composerDraft': {'text': 'Legacy'}})
        with patch.object(desk, 'AgentClient', return_value=client), self.assertRaises(desk.DeskError):
            desk.read_composer(self.data)

    def test_older_resident_stage_is_rejected_even_when_newer_generation_has_not_run(self):
        client = self.native_fixture({}, extra_modules=[{
            'id': '56', 'name': 'LARICS / LYRICS party stage', 'hadSuccessfulRun': False, 'hadAttemptedRun': False}])
        with patch.object(desk, 'AgentClient', return_value=client), self.assertRaisesRegex(desk.DeskError, 'newer'):
            desk.read_composer(self.data)
        self.assertEqual([call.args[0] for call in client.request.call_args_list], ['hello', 'module.list'])
        client.wait_for_operation.assert_not_called()

    def test_front_plan_uses_occurrences_and_preserves_remaining_order(self):
        sync = observation([track('same-first'), track('middle', URI_B),
                            track('same-second'), track('context', source='context')])
        sync['recentTracks'] = [track('previous-occurrence')]
        plan = desk.plan_order(sync, ['same-second'])
        self.assertEqual(plan['expectedQueuedUids'], ['same-first', 'middle', 'same-second'])
        self.assertEqual(plan['orderedQueuedUids'], ['same-second', 'same-first', 'middle'])
        self.assertEqual(plan['expectedCurrentUid'], 'current')
        self.assertEqual(plan['expectedDeviceId'], 'native-device')
        self.assertEqual(plan['previouslyObserved'], ['same-second', 'same-first'])
        self.assertTrue(plan['changed'])
        self.assertEqual([item['uid'] for item in sync['observation']['queue']],
                         ['same-first', 'middle', 'same-second', 'context'])

    def test_full_order_requires_every_uid_exactly_once(self):
        sync = observation([track('one'), track('two'), track('three')])
        for selected in (['two', 'one'], ['two', 'one', 'one'],
                         ['two', 'one', URI_A], ['two', 'one', 'unknown']):
            with self.subTest(selected=selected), self.assertRaises(desk.DeskError):
                desk.plan_order(sync, selected, front=False)
        plan = desk.plan_order(sync, ['three', 'one', 'two'], front=False)
        self.assertEqual(plan['orderedQueuedUids'], ['three', 'one', 'two'])
        with self.assertRaises(desk.DeskError):
            desk.plan_order(sync, ['one', 'one'])

    def test_empty_single_and_already_ordered_plans_are_guarded_no_ops(self):
        for uids in ([], ['one'], ['one', 'two']):
            with self.subTest(uids=uids):
                plan = desk.plan_order(observation([track(uid) for uid in uids]), uids, front=False)
                self.assertFalse(plan['changed'])
                self.assertEqual(plan['orderedQueuedUids'], uids)
                self.assertEqual(plan['expectedCurrentUid'], 'current')
                self.assertEqual(plan['expectedDeviceId'], 'native-device')

    def test_partial_stale_disconnected_or_unidentified_observations_are_rejected(self):
        changes = {
            'disconnected': lambda s: s['state']['spotify'].update(connected=False),
            'stale': lambda s: s['observation'].update(observedAt=NOW - 10001),
            'future': lambda s: s['observation'].update(observedAt=NOW + 1001),
            'boolean-time': lambda s: s['observation'].update(observedAt=True),
            'nonfinite-time': lambda s: s['observation'].update(observedAt=float('nan')),
            'missing-time': lambda s: s['observation'].pop('observedAt'),
            'partial': lambda s: s['observation'].update(explicitQueueComplete=False),
            'missing-completeness': lambda s: s['observation'].pop('explicitQueueComplete'),
            'missing-current': lambda s: s['observation']['nowPlaying'].pop('uid'),
            'missing-device': lambda s: s['observation']['playbackState'].update(device=None),
            'nonstring-device': lambda s: s['observation']['playbackState']['device'].update(id=True),
            'invalid-current-clock': lambda s: s['observation']['nowPlaying'].update(clockValid=False),
            'invalid-playback-clock': lambda s: s['observation']['playbackState'].update(clockValid=False),
            'null-spotify': lambda s: s['state'].update(spotify=None),
            'duplicate-uid': lambda s: s['observation']['queue'].append(track('one')),
            'missing-uid': lambda s: s['observation']['queue'][0].pop('uid'),
            'missing-queue': lambda s: s['observation'].pop('queue'),
            'null-queue': lambda s: s['observation'].update(queue=None),
        }
        for label, change in changes.items():
            with self.subTest(case=label):
                sync = observation([track('one')])
                change(sync)
                with self.assertRaises(desk.DeskError):
                    desk.actual_queue(sync)

    def test_apply_sends_only_exact_plan_preconditions_and_never_replans_a_race(self):
        plan = desk.plan_order(observation([track('one'), track('two')]), ['two'])
        error = desk.DeskError({'error': 'The queue changed manually.', 'dispatched': False})
        client = ScriptedClient(('/api/admin/reorder', error))
        with self.assertRaises(desk.DeskError) as caught:
            desk.apply_order(client, plan)
        self.assertIs(caught.exception, error)
        self.assertEqual(client.calls, [('/api/admin/reorder',
            {key: plan[key] for key in desk.REORDER_FIELDS})])
        self.assertFalse(client.steps)

    def test_apply_preserves_uncertain_or_lost_response_without_retry(self):
        plan = desk.plan_order(observation([track('one'), track('two')]), ['two'])
        for detail in ({'error': 'Order not confirmed.', 'dispatched': True},
                       {'error': 'Response lost.', 'responseLost': True}):
            with self.subTest(detail=detail):
                client = ScriptedClient(('/api/admin/reorder', desk.DeskError(detail)))
                with self.assertRaises(desk.DeskError) as caught:
                    desk.apply_order(client, plan)
                self.assertEqual(caught.exception.detail, detail)
                self.assertEqual(len(client.calls), 1)

    def test_apply_rejects_wrong_plan_object_types_before_any_write(self):
        for plan in (None, [], 'reorder', 42):
            with self.subTest(plan=plan):
                client = ScriptedClient()
                with self.assertRaises(desk.DeskError):
                    desk.apply_order(client, plan)
                self.assertFalse(client.calls)

    def test_add_front_enqueues_once_then_promotes_its_exact_observed_uid(self):
        request_id = 'stable-new-request'
        queued = observation([track('same-manual'), track('middle', URI_B), track('new-occurrence')])
        queued['state']['queue'][-1].update(id=request_id, status='queued')
        client = ScriptedClient(
            ('/api/admin/sync', observation([track('same-manual'), track('middle', URI_B)])),
            ('/api/admin/request', {'request': {'id': request_id, 'status': 'pending'}}),
            ('/api/admin/sync', queued),
            ('/api/admin/reorder', {'ok': True, 'moved': True}))
        with contextlib.redirect_stderr(io.StringIO()):
            result = desk.add_track(client, URI_A, front=True, request_id=request_id)
        self.assertTrue(result['reorder']['moved'])
        writes = [(path, body) for path, body in client.calls if path != '/api/admin/sync']
        self.assertEqual([path for path, body in writes], ['/api/admin/request', '/api/admin/reorder'])
        self.assertEqual(writes[0][1]['requestId'], request_id)
        self.assertEqual(writes[1][1], {
            'orderedQueuedUids': ['new-occurrence', 'same-manual', 'middle'],
            'expectedQueuedUids': ['same-manual', 'middle', 'new-occurrence'],
            'expectedCurrentUid': 'current', 'expectedDeviceId': 'native-device'})
        self.assertFalse(client.steps)

    def test_add_front_accepts_existing_request_identity_and_observed_playing_state(self):
        current = observation()
        current['state']['nowPlaying']['requestId'] = 'existing-request'
        client = ScriptedClient(
            ('/api/admin/sync', observation()),
            ('/api/admin/request', {'alreadyQueued': True,
                                   'request': {'id': 'existing-request', 'status': 'playing'}}),
            ('/api/admin/sync', current))
        with contextlib.redirect_stderr(io.StringIO()):
            result = desk.add_track(client, URI_A, front=True, request_id='new-client-request')
        self.assertTrue(result['alreadyPlaying'])
        self.assertEqual(len(client.calls), 3)

    def test_add_front_pending_timeout_and_uncertainty_never_submit_again(self):
        for status in ('pending', 'uncertain'):
            with self.subTest(status=status):
                pending = observation()
                pending['state']['queue'] = [{'id': 'stable-request', 'uri': URI_A, 'status': status}]
                client = ScriptedClient(
                    ('/api/admin/sync', observation()),
                    ('/api/admin/request', {'request': {'id': 'stable-request', 'status': status}}),
                    ('/api/admin/sync', pending),
                    *([('/api/admin/sync', pending)] if status == 'pending' else []))
                clock = [0]
                def advance(unused):
                    clock[0] += 8
                with contextlib.redirect_stderr(io.StringIO()), \
                        patch.object(desk.time, 'monotonic', side_effect=lambda: clock[0]), \
                        patch.object(desk.time, 'sleep', side_effect=advance):
                    result = desk.add_track(client, URI_A, front=True, request_id='stable-request')
                self.assertTrue(result['frontPending'])
                self.assertEqual(sum(path == '/api/admin/request' for path, body in client.calls), 1)
                self.assertFalse(any(path == '/api/admin/reorder' for path, body in client.calls))
                self.assertFalse(client.steps)

    def test_add_front_manual_reorder_race_does_not_repeat_enqueue_or_reorder(self):
        queued = observation([track('old'), track('requested')])
        queued['state']['queue'][1].update(id='stable-request', status='queued')
        client = ScriptedClient(
            ('/api/admin/sync', observation([track('old')])),
            ('/api/admin/request', {'request': {'id': 'stable-request', 'status': 'pending'}}),
            ('/api/admin/sync', queued),
            ('/api/admin/reorder', desk.DeskError({'error': 'Manual reorder.', 'dispatched': False})))
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(desk.DeskError):
            desk.add_track(client, URI_A, front=True, request_id='stable-request')
        self.assertEqual(sum(path == '/api/admin/request' for path, body in client.calls), 1)
        self.assertEqual(sum(path == '/api/admin/reorder' for path, body in client.calls), 1)
        self.assertFalse(client.steps)

    def test_add_front_declines_promotion_after_intervening_queue_current_or_device_edits(self):
        initial = observation([track('old-one'), track('old-two')])
        changes = {
            'reordered': lambda s: s['observation']['queue'].reverse(),
            'added': lambda s: s['observation']['queue'].append(track('manual-add')),
            'removed': lambda s: s['observation']['queue'].pop(0),
            'same-uri-new-uid': lambda s: s['observation']['queue'][0].update(uid='replacement'),
            'current': lambda s: s['observation']['nowPlaying'].update(uid='manual-current'),
            'device': lambda s: s['observation']['playbackState']['device'].update(id='manual-device'),
        }
        for label, change in changes.items():
            with self.subTest(change=label):
                after = observation([track('old-one'), track('old-two')])
                change(after)
                after['observation']['queue'].append(track('requested'))
                after['state']['queue'] = copy.deepcopy(after['observation']['queue'])
                after['state']['queue'][-1].update(id='stable-request', status='queued')
                client = ScriptedClient(
                    ('/api/admin/sync', initial),
                    ('/api/admin/request', {'request': {'id': 'stable-request', 'status': 'pending'}}),
                    ('/api/admin/sync', after))
                with contextlib.redirect_stderr(io.StringIO()):
                    result = desk.add_track(client, URI_A, front=True, request_id='stable-request')
                self.assertTrue(result['frontPending'])
                self.assertTrue(result['queueChanged'])
                self.assertEqual(result['request']['id'], 'stable-request')
                self.assertFalse(any(path == '/api/admin/reorder' for path, body in client.calls))

    def test_add_front_remembers_manual_edits_seen_before_its_uid_appears(self):
        for change in ('queue', 'current', 'device'):
            with self.subTest(change=change):
                initial = observation([track('old-one'), track('old-two')])
                pending = copy.deepcopy(initial)
                pending['state']['queue'].append({'id': 'stable-request', 'status': 'pending', 'uri': URI_A})
                if change == 'queue':
                    pending['observation']['queue'].reverse()
                elif change == 'current':
                    pending['observation']['nowPlaying']['uid'] = 'intervening-current'
                else:
                    pending['observation']['playbackState']['device']['id'] = 'intervening-device'
                ready = observation([track('old-one'), track('old-two'), track('requested')])
                ready['state']['queue'][-1].update(id='stable-request', status='queued')
                client = ScriptedClient(
                    ('/api/admin/sync', initial),
                    ('/api/admin/request', {'request': {'id': 'stable-request', 'status': 'pending'}}),
                    ('/api/admin/sync', pending),
                    ('/api/admin/sync', ready),
                    ('/api/admin/reorder', {'ok': True, 'moved': True}))
                with contextlib.redirect_stderr(io.StringIO()), patch.object(desk.time, 'sleep'):
                    result = desk.add_track(client, URI_A, front=True, request_id='stable-request')
                self.assertTrue(result.get('frontPending'))
                self.assertTrue(result.get('queueChanged'))
                self.assertFalse(any(path == '/api/admin/reorder' for path, body in client.calls))

    def test_add_front_distinguishes_pending_new_occurrences_even_with_the_same_uri(self):
        for pending_uid in ('manual-same-recording', 'requested'):
            with self.subTest(pending_uid=pending_uid):
                initial = observation([track('old-one'), track('old-two')])
                pending = observation([track('old-one'), track('old-two'), track(pending_uid)])
                pending['state']['queue'] = copy.deepcopy(initial['state']['queue']) + [
                    {'id': 'stable-request', 'status': 'pending', 'uri': URI_A}]
                ready = observation([track('old-one'), track('old-two'), track('requested')])
                ready['state']['queue'][-1].update(id='stable-request', status='queued')
                client = ScriptedClient(
                    ('/api/admin/sync', initial),
                    ('/api/admin/request', {'request': {'id': 'stable-request', 'status': 'pending'}}),
                    ('/api/admin/sync', pending),
                    ('/api/admin/sync', ready),
                    ('/api/admin/reorder', {'ok': True, 'moved': True}))
                with contextlib.redirect_stderr(io.StringIO()), patch.object(desk.time, 'sleep'):
                    result = desk.add_track(client, URI_A, front=True, request_id='stable-request')
                mutations = [body for path, body in client.calls if path == '/api/admin/reorder']
                if pending_uid == 'requested':
                    self.assertEqual(len(mutations), 1, 'the observed request occurrence was mistaken for a manual addition')
                    self.assertTrue(result['reorder']['moved'])
                else:
                    self.assertTrue(result.get('frontPending'))
                    self.assertTrue(result.get('queueChanged'))
                    self.assertFalse(mutations, 'a different observed occurrence was accepted through its matching URI')

    def test_add_front_preserves_manual_placement_of_its_new_occurrence(self):
        for reverts_before_binding in (False, True):
            with self.subTest(reverts_before_binding=reverts_before_binding):
                initial = observation([track('old-one'), track('old-two')])
                manually_placed = observation([track('old-one'), track('requested'), track('old-two')])
                steps = [('/api/admin/sync', initial),
                         ('/api/admin/request', {'request': {'id': 'stable-request', 'status': 'pending'}})]
                if reverts_before_binding:
                    manually_placed['state']['queue'] = copy.deepcopy(initial['state']['queue']) + [
                        {'id': 'stable-request', 'status': 'pending', 'uri': URI_A}]
                    steps.append(('/api/admin/sync', manually_placed))
                    ready = observation([track('old-one'), track('old-two'), track('requested')])
                    ready['state']['queue'][-1].update(id='stable-request', status='queued')
                else:
                    ready = manually_placed
                    ready['state']['queue'][1].update(id='stable-request', status='queued')
                steps.extend([('/api/admin/sync', ready),
                              ('/api/admin/reorder', {'ok': True, 'moved': True})])
                client = ScriptedClient(*steps)
                with contextlib.redirect_stderr(io.StringIO()), patch.object(desk.time, 'sleep'):
                    result = desk.add_track(client, URI_A, front=True, request_id='stable-request')
                self.assertTrue(result.get('frontPending'))
                self.assertTrue(result.get('queueChanged'))
                self.assertEqual(result['request']['id'], 'stable-request')
                self.assertFalse(any(path == '/api/admin/reorder' for path, body in client.calls))

    def test_version_repeat_checks_real_metadata_across_distinct_uris(self):
        recent = observation()
        recent['recentTracks'] = [track('prior', URI_A, title='Ča je život vengo fantažija', artist='Oliver Dragojević')]
        for title in ('Ca je zivot vengo fantazija - 2018 Remaster',
                      'Ca je zivot vengo fantazija (Remastered)',
                      'Ca je zivot vengo fantazija – Radio Edit'):
            with self.subTest(title=title):
                candidate = track('catalogue', URI_C, title=title, artist='OLIVER DRAGÓJEVIC!')
                client = ScriptedClient(('/api/admin/sync', recent),
                    ('/api/track?' + desk.urllib.parse.urlencode({'uri': URI_C}), {'track': candidate}))
                with self.assertRaisesRegex(desk.DeskError, 'allow-repeat'):
                    desk.add_track(client, URI_C, request_id='version-request')
                self.assertEqual(len(client.calls), 2)
                self.assertFalse(any(path == '/api/admin/request' for path, body in client.calls))

    def test_different_song_identity_is_allowed_and_repeat_override_skips_metadata(self):
        recent = observation()
        recent['recentTracks'] = [track('prior', URI_A, title='Same title', artist='First artist')]
        for allow_repeat, candidate in (
                (False, track('catalogue', URI_C, title='Same title', artist='Second artist')),
                (False, track('catalogue', URI_C, title='Different title', artist='First artist')),
                (True, None)):
            with self.subTest(allow_repeat=allow_repeat, candidate=candidate):
                steps = [('/api/admin/sync', recent)]
                if not allow_repeat:
                    steps.append(('/api/track?' + desk.urllib.parse.urlencode({'uri': URI_C}), {'track': candidate}))
                steps.append(('/api/admin/request', {'request': {'id': 'version-request', 'status': 'pending'}}))
                client = ScriptedClient(*steps)
                with contextlib.redirect_stderr(io.StringIO()):
                    desk.add_track(client, URI_C, allow_repeat=allow_repeat, request_id='version-request')
                self.assertFalse(client.steps)
                self.assertEqual(sum(path == '/api/admin/request' for path, body in client.calls), 1)

    def test_missing_repeat_candidate_metadata_stops_before_enqueue(self):
        recent = observation()
        recent['recentTracks'] = [track('prior', URI_A)]
        for candidate in (None, {}, {'title': 'Song'}, {'artist': 'Artist'}):
            with self.subTest(candidate=candidate):
                client = ScriptedClient(('/api/admin/sync', recent),
                    ('/api/track?' + desk.urllib.parse.urlencode({'uri': URI_C}), {'track': candidate}))
                with self.assertRaises(desk.DeskError):
                    desk.add_track(client, URI_C, request_id='missing-metadata')
                self.assertEqual(len(client.calls), 2)

    def test_repeat_history_requires_explicit_override_before_a_write(self):
        recent = observation()
        recent['recentTracks'] = [track('older-occurrence')]
        client = ScriptedClient(('/api/admin/sync', recent))
        with self.assertRaisesRegex(desk.DeskError, 'allow-repeat'):
            desk.add_track(client, URI_A, request_id='intentional-request')
        self.assertEqual(len(client.calls), 1)
        client = ScriptedClient(('/api/admin/sync', recent),
            ('/api/admin/request', {'request': {'id': 'intentional-request', 'status': 'pending'}}))
        with contextlib.redirect_stderr(io.StringIO()):
            desk.add_track(client, URI_A, allow_repeat=True, request_id='intentional-request')
        self.assertEqual(len(client.calls), 2)

    def test_lost_add_response_preserves_request_id_and_does_not_promote_or_resubmit(self):
        client = ScriptedClient(('/api/admin/sync', observation()),
            ('/api/admin/request', desk.DeskError({'error': 'Response lost.', 'responseLost': True})))
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), self.assertRaises(desk.DeskError):
            desk.add_track(client, URI_A, front=True, request_id='recoverable-request')
        self.assertEqual(json.loads(stderr.getvalue())['requestId'], 'recoverable-request')
        self.assertEqual(len(client.calls), 2)

    def test_http_errors_and_lost_write_responses_keep_uncertainty(self):
        client = desk.ServiceClient('http://fixture.invalid', self.data)
        cases = [
            (HTTPError('fixture', 503, 'Failed', {},
                       io.BytesIO(b'{"error":"Unconfirmed","dispatched":true}')),
             {'error': 'Unconfirmed', 'dispatched': True}),
            (URLError('Fixture disconnected'), None),
        ]
        for failure, expected in cases:
            with self.subTest(failure=str(failure)):
                with patch.object(desk.urllib.request, 'urlopen', side_effect=failure) as urlopen:
                    with self.assertRaises(desk.DeskError) as caught:
                        client.call('/api/admin/reorder', {'fixture': True})
                self.assertEqual(urlopen.call_count, 1)
                if expected:
                    self.assertEqual(caught.exception.detail, expected)
                else:
                    self.assertTrue(caught.exception.detail['responseLost'])

    def test_truncated_write_json_is_an_uncertain_response(self):
        client = desk.ServiceClient('http://fixture.invalid', self.data)
        with patch.object(desk.urllib.request, 'urlopen', return_value=io.BytesIO(b'{"ok":')) as urlopen:
            with self.assertRaises(desk.DeskError) as caught:
                client.call('/api/admin/reorder', {'fixture': True})
        self.assertTrue(caught.exception.detail['responseLost'])
        self.assertEqual(urlopen.call_count, 1)

    def test_malformed_or_truncated_http_error_bodies_keep_write_uncertainty(self):
        class IncompleteBody(io.BytesIO):
            def read(self, *unused):
                raise desk.http.client.IncompleteRead(b'{"error":', 30)

        for label, body in (('invalid-json', io.BytesIO(b'{"error":')),
                            ('invalid-object', io.BytesIO(b'[]')),
                            ('truncated-stream', IncompleteBody())):
            with self.subTest(case=label):
                client = desk.ServiceClient('http://fixture.invalid', self.data)
                error = HTTPError('fixture', 503, 'Service failed', {}, body)
                with patch.object(desk.urllib.request, 'urlopen', side_effect=error) as urlopen:
                    with self.assertRaises(desk.DeskError) as caught:
                        client.call('/api/admin/reorder', {'fixture': True})
                self.assertTrue(caught.exception.detail['responseLost'])
                self.assertEqual(urlopen.call_count, 1)

    def test_watch_resumes_cursor_and_suppresses_recalled_events_in_both_formats(self):
        for raw in (False, True):
            with self.subTest(raw=raw):
                self.cursor.write_text('{"lastId":10}')
                state = observation()['state']
                state['withdrawnMessageIds'] = [71]
                events = [{'id': 11, 'kind': 'chat', 'payload': {'id': 71, 'text': 'Recalled'}},
                          {'id': 12, 'kind': 'chat', 'payload': {'id': 72, 'text': 'Keep this'}},
                          {'id': 13, 'kind': 'message_recalled', 'payload': {'messageId': 71}}]
                client = ScriptedClient(('/api/admin/events?after=10&wait=0',
                    {'state': state, 'events': events, 'lastId': 13}))
                stdout = io.StringIO()
                with contextlib.redirect_stdout(stdout):
                    desk.watch(client, cursor=self.cursor, wait=0, once=True, raw=raw)
                result = json.loads(stdout.getvalue())
                self.assertEqual([event['id'] for event in result['events']], [12, 13])
                self.assertEqual(json.loads(self.cursor.read_text()), {'lastId': 13})
                self.assertEqual(len(client.calls), 1)

    def test_watch_cursor_advances_only_after_successful_output(self):
        self.cursor.write_text('{"lastId":10}')
        client = ScriptedClient(('/api/admin/events?after=10&wait=0',
            {'state': observation()['state'],
             'events': [{'id': 11, 'kind': 'chat', 'payload': {'id': 71, 'text': 'Delivered before cursor'}}],
             'lastId': 11}))
        with patch.object(desk, 'write_json', side_effect=BrokenPipeError('Consumer closed')):
            with self.assertRaises(BrokenPipeError):
                desk.watch(client, cursor=self.cursor, wait=0, once=True)
        self.assertEqual(json.loads(self.cursor.read_text()), {'lastId': 10})

    def test_watch_persists_initial_seed_before_waiting_for_the_next_batch(self):
        client = ScriptedClient(
            ('/api/admin/events?after=-1&wait=0',
             {'state': observation()['state'], 'events': [], 'lastId': 40}),
            ('/api/admin/events?after=40&wait=0', desk.DeskError('Disconnected before first batch.')))
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout), self.assertRaises(desk.DeskError):
            desk.watch(client, cursor=self.cursor, wait=0, once=True)
        self.assertEqual(json.loads(stdout.getvalue())['lastId'], 40)
        self.assertTrue(self.cursor.exists(), 'The delivered initial resume point was not persisted.')
        self.assertEqual(json.loads(self.cursor.read_text()), {'lastId': 40})

    def test_watch_compact_queue_events_keep_count_and_limit_preview(self):
        items = [track(f'queued-{number}') for number in range(12)]
        client = ScriptedClient(('/api/admin/events?after=10&wait=0',
            {'state': observation(items)['state'],
             'events': [{'id': 11, 'kind': 'queue_changed', 'payload': {'queue': items}}], 'lastId': 11}))
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            desk.watch(client, after=10, wait=0, once=True)
        payload = json.loads(stdout.getvalue())['events'][0]['payload']
        self.assertEqual(payload['count'], 12)
        self.assertEqual(len(payload['queue']), 6)

    def test_watch_retries_transient_reads_with_the_same_cursor_and_backoff(self):
        self.cursor.write_text('{"lastId":10}')
        route = '/api/admin/events?after=10&wait=0'
        batch = {'state': observation()['state'], 'events': [
            {'id': 11, 'kind': 'chat', 'payload': {'id': 71, 'text': 'Still pending'}}], 'lastId': 11}
        client = ScriptedClient((route, desk.DeskError('Connection interrupted', transient=True)),
                                (route, desk.DeskError('Temporarily unavailable', transient=True)),
                                (route, batch))
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr), \
                patch.object(desk.time, 'sleep') as sleep:
            desk.watch(client, cursor=self.cursor, wait=0, once=True)
        self.assertEqual([path for path, body in client.calls], [route] * 3)
        self.assertTrue(all(body is None for path, body in client.calls))
        self.assertEqual([call.args[0] for call in sleep.call_args_list], [1, 2])
        self.assertEqual([json.loads(line)['after'] for line in stderr.getvalue().splitlines()], [10, 10])
        self.assertEqual(json.loads(stdout.getvalue())['events'], batch['events'])
        self.assertEqual(json.loads(self.cursor.read_text()), {'lastId': 11})

    def test_watch_retries_initial_seed_and_persists_it_before_later_read_retries(self):
        initial_route, following_route = '/api/admin/events?after=-1&wait=0', '/api/admin/events?after=40&wait=0'
        client = ScriptedClient(
            (initial_route, desk.DeskError('Seed temporarily unavailable', transient=True)),
            (initial_route, {'state': observation()['state'], 'events': [], 'lastId': 40}),
            (following_route, desk.DeskError('Next read interrupted', transient=True)),
            (following_route, {'state': observation()['state'], 'events': [
                {'id': 41, 'kind': 'chat', 'payload': {'id': 72, 'text': 'New request'}}], 'lastId': 41}))
        def while_retrying(unused):
            if len(client.calls) == 3:
                self.assertEqual(json.loads(self.cursor.read_text()), {'lastId': 40})
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()), \
                patch.object(desk.time, 'sleep', side_effect=while_retrying):
            desk.watch(client, cursor=self.cursor, wait=0, once=True)
        self.assertEqual([path for path, body in client.calls], [initial_route, initial_route, following_route, following_route])
        self.assertEqual(json.loads(self.cursor.read_text()), {'lastId': 41})

    def test_watch_auth_and_schema_failures_leave_the_cursor_and_do_not_retry(self):
        valid = {'state': observation()['state'], 'events': [], 'lastId': 11}
        responses = [desk.DeskError('Authentication required'),
                     {**valid, 'lastId': 9}, {**valid, 'events': None}, {**valid, 'state': None},
                     {**valid, 'events': [{'kind': 'chat', 'payload': {'id': 71}}]},
                     {**valid, 'events': [{'id': 11, 'kind': 'queue_changed', 'payload': {'queue': None}}]}]
        for response in responses:
            with self.subTest(response=response):
                self.cursor.write_text('{"lastId":10}')
                client = ScriptedClient(('/api/admin/events?after=10&wait=0', response))
                stdout = io.StringIO()
                with contextlib.redirect_stdout(stdout), patch.object(desk.time, 'sleep') as sleep:
                    with self.assertRaises(desk.DeskError):
                        desk.watch(client, cursor=self.cursor, wait=0, once=True)
                sleep.assert_not_called()
                self.assertEqual(len(client.calls), 1)
                self.assertEqual(stdout.getvalue(), '')
                self.assertEqual(json.loads(self.cursor.read_text()), {'lastId': 10})

    def test_http_read_retry_classification_distinguishes_transient_and_schema_errors(self):
        client = desk.ServiceClient('http://fixture.invalid', self.data)
        for code in (400, 401, 403, 404, 408, 429, 500, 503):
            with self.subTest(code=code):
                error = HTTPError('fixture', code, 'Read failed', {}, io.BytesIO(b'{"error":"Read failed"}'))
                with patch.object(desk.urllib.request, 'urlopen', side_effect=error):
                    with self.assertRaises(desk.DeskError) as caught:
                        client.call('/api/admin/events?after=10&wait=0')
                self.assertEqual(caught.exception.transient, code in (408, 429, 500, 503))
        with patch.object(desk.urllib.request, 'urlopen', side_effect=URLError('Disconnected')):
            with self.assertRaises(desk.DeskError) as caught:
                client.call('/api/admin/events?after=10&wait=0')
        self.assertTrue(caught.exception.transient)
        for body in (b'[]', b'{"events":'):
            with self.subTest(schema=body), patch.object(desk.urllib.request, 'urlopen', return_value=io.BytesIO(body)):
                with self.assertRaises(desk.DeskError) as caught:
                    client.call('/api/admin/events?after=10&wait=0')
                self.assertFalse(caught.exception.transient)

    def test_queue_command_reads_fresh_full_metadata_and_pending_requests(self):
        explicit = [track(f'q-{index}', album=f'Album {index}', durationMs=180000 + index) for index in range(12)]
        context = [track(f'context-{index}', source='context', album=f'Context album {index}') for index in range(4)]
        synced = observation(explicit + context)
        pending = [{'id': 'pending-request', 'uri': URI_C, 'status': 'pending', 'requestedBy': 'Guest'}]
        synced['state']['queue'].extend(pending)
        client = ScriptedClient(('/api/admin/sync', synced))
        stdout = io.StringIO()
        with patch.object(desk, 'ServiceClient', return_value=client), contextlib.redirect_stdout(stdout):
            result = desk.main(['queue'])
        self.assertEqual(result, 0)
        output = json.loads(stdout.getvalue())
        self.assertEqual(output['explicitQueue'], explicit)
        self.assertEqual(output['context'], context)
        self.assertEqual(output['pending'], pending)
        self.assertEqual(output['nowPlaying']['uid'], 'current')
        self.assertEqual(output['device']['id'], 'native-device')
        self.assertEqual(client.calls, [('/api/admin/sync', {})])


if __name__ == '__main__':
    unittest.main()
