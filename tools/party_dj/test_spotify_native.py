"""Connection recovery checks with an in-memory CDP daemon and no Spotify calls."""
from __future__ import annotations

import io
import json
import concurrent.futures
import threading
import time
import unittest
from unittest.mock import patch
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit

import spotify_native as native


class FakeDaemon:
    def __init__(self, failure):
        self.failure = failure
        self.failed = False
        self.calls = []

    def urlopen(self, request, timeout=None):
        address = urlsplit(request.full_url)
        body = json.loads(request.data) if request.data else None
        self.calls.append((address.path, body))
        if address.port == 9224:
            raise URLError('No native debugging endpoint in this test')
        if address.path == '/eval':
            if not self.failed:
                self.failed = True
                if self.failure == 'http':
                    raise HTTPError(request.full_url, 404, 'Not Found', {},
                                    io.BytesIO(b'{"error":"No session with given id"}'))
                if self.failure == 'protocol':
                    value = {'error': {'code': -32001, 'message': 'Session with given id not found'}}
                else:
                    value = {'exceptionDetails': {'text': 'Uncaught', 'exception': {
                        'description': 'Error: This recording is unavailable.\n    at Spotify.play'}}}
            else:
                value = {'result': {'type': 'number', 'value': 42}}
        elif address.path == '/targets':
            value = [{'type': 'page', 'url': 'https://open.spotify.com/', 'targetId': 'spotify-tab'}]
        elif address.path == '/attach':
            value = {'sessionId': 'fresh-session'}
        elif address.path == '/prepare':
            value = {'sessionId': body['sessionId'], 'readiness': 'dom'}
        elif address.path == '/release':
            value = {'released': True}
        elif address.path == '/cdp' and body.get('method') == 'Target.detachFromTarget':
            value = {'result': {}}
        else:
            raise AssertionError(f'Unexpected daemon request: {address.path} {body}')
        return io.BytesIO(json.dumps(value).encode())


class DiscoveryDaemon:
    def __init__(self):
        self.pages = []
        self.calls = []

    def urlopen(self, request, timeout=None):
        address = urlsplit(request.full_url)
        body = json.loads(request.data) if request.data else None
        self.calls.append((address.path, body))
        if address.port == 9224:
            raise URLError('No native endpoint in this fixture')
        if address.path == '/targets':
            value = self.pages
        elif address.path == '/attach':
            value = {'sessionId': 'existing-page-session'}
        elif address.path == '/prepare':
            value = {'sessionId': body['sessionId'], 'readiness': 'dom'}
        elif address.path == '/eval':
            result = {'ready': True} if body['expression'].startswith('(async') else {'nowPlaying': {'uid': 'fixture-current'}}
            value = {'result': {'type': 'object', 'value': result}}
        elif address.path == '/release':
            value = {'released': True}
        elif address.path == '/cdp' and body.get('method') == 'Target.detachFromTarget':
            value = {'result': {}}
        else:
            raise AssertionError(f'Unexpected fixture request: {address.path} {body}')
        return io.BytesIO(json.dumps(value).encode())


class SpotifyTransportTests(unittest.TestCase):
    def client_and_daemon(self, failure):
        daemon = FakeDaemon(failure)
        self.urlopen = patch.object(native.urllib.request, 'urlopen', daemon.urlopen)
        self.urlopen.start()
        self.addCleanup(self.urlopen.stop)
        sockets = patch.object(native.websocket, 'create_connection',
                               side_effect=AssertionError('Tests must not open a real Spotify connection'))
        sockets.start()
        self.addCleanup(sockets.stop)
        client = native.SpotifyClient()
        client.session = 'old-session'
        client.created_web_target = 'previous-tab'
        client.last_native_check = time.monotonic()
        self.addCleanup(client.close)
        return client, daemon

    def assert_transport_recovery(self, failure):
        client, daemon = self.client_and_daemon(failure)
        with self.assertRaises(native.SpotifyTransportError):
            client.evaluate('42')
        self.assertIsNone(client.session)
        self.assertIsNone(client.created_web_target)
        self.assertIn(('/release', {'sessionId': 'old-session', 'timeout_seconds': 5}), daemon.calls)
        detach = [body for path, body in daemon.calls if path == '/cdp']
        self.assertTrue(any(body.get('method') == 'Target.detachFromTarget'
                            and body['params']['sessionId'] == 'old-session' for body in detach))
        self.assertEqual(client.evaluate('42'), 42)
        self.assertEqual(client.session, 'fresh-session')
        self.assertEqual([body['sessionId'] for path, body in daemon.calls if path == '/eval'],
                         ['old-session', 'fresh-session'])
        self.assertEqual(sum(path == '/attach' for path, _ in daemon.calls), 1)

    def test_missing_session_http_error_releases_and_reattaches(self):
        self.assert_transport_recovery('http')

    def test_missing_session_protocol_error_releases_and_reattaches(self):
        self.assert_transport_recovery('protocol')

    def test_ordinary_operations_never_create_or_prepare_a_missing_controller(self):
        daemon = DiscoveryDaemon()
        client = native.SpotifyClient()
        client.created_web_target = 'no-longer-listed-controller'
        with patch.object(native.urllib.request, 'urlopen', daemon.urlopen), \
                patch.object(native.websocket, 'create_connection', side_effect=AssertionError('No real socket')):
            for name, arguments in (
                    ('status', ()), ('search', ('fixture song',)),
                    ('enqueue', ('spotify:track:' + 'a' * 22, 'fixture-request')),
                    ('move_queue', ('two', 'one', ['one', 'two'])),
                    ('reorder_queue', (['two', 'one'], ['one', 'two'], 'current', 'device')),
                    ('set_paused', (True, 'current', 'device'))):
                with self.subTest(operation=name):
                    with self.assertRaisesRegex(native.SpotifyError, 'spotify_reconnect.py ensure') as caught:
                        getattr(client, name)(*arguments)
                    if name in ('enqueue', 'move_queue', 'reorder_queue', 'set_paused'):
                        self.assertFalse(caught.exception.dispatched)
            client.close()
        self.assertTrue(any(path == '/targets' for path, body in daemon.calls))
        self.assertFalse(any(path in ('/attach', '/prepare', '/eval', '/cdp') for path, body in daemon.calls))

    def test_later_existing_controller_discovery_attaches_without_creating_a_target(self):
        daemon = DiscoveryDaemon()
        client = native.SpotifyClient()
        with patch.object(native.urllib.request, 'urlopen', daemon.urlopen), \
                patch.object(native.websocket, 'create_connection', side_effect=AssertionError('No real socket')):
            with self.assertRaises(native.SpotifyTransportError):
                client.status()
            daemon.pages = [{'type': 'page', 'url': 'https://open.spotify.com/', 'targetId': 'already-existing'}]
            self.assertEqual(client.status()['nowPlaying']['uid'], 'fixture-current')
            self.assertEqual(client.mode, 'native-via-web-controller')
            client.close()
        attaches = [body for path, body in daemon.calls if path == '/attach']
        self.assertEqual(attaches, [{'targetId': 'already-existing'}])
        self.assertEqual(sum(path == '/prepare' for path, body in daemon.calls), 1)
        self.assertFalse(any(path == '/cdp' and body.get('method') == 'Target.createTarget' for path, body in daemon.calls))

    def test_native_debugging_path_does_not_require_or_create_a_browser_controller(self):
        client = native.SpotifyClient()
        target = {'id': 'native-xpui', 'type': 'page', 'url': 'spotify:app:xpui',
                  'webSocketDebuggerUrl': 'ws://fixture.invalid/native'}
        with patch.object(client, '_http', return_value=[target]) as http, \
                patch.object(native.websocket, 'create_connection') as connect:
            client._connect()
            self.assertEqual(client.mode, 'native')
            self.assertEqual(client.target, 'native-xpui')
            http.assert_called_once_with('http://127.0.0.1:9224/json/list', timeout=1)
            connect.assert_called_once_with('ws://fixture.invalid/native', timeout=25, suppress_origin=True)
            client.close()

    def test_spotify_application_exception_preserves_session_and_dispatch_status(self):
        client, daemon = self.client_and_daemon('application')
        with self.assertRaises(native.SpotifyError) as caught:
            client.evaluate('42')
        self.assertNotIsInstance(caught.exception, native.SpotifyTransportError)
        self.assertTrue(caught.exception.dispatched)
        self.assertEqual(str(caught.exception), 'This recording is unavailable.')
        self.assertEqual(client.session, 'old-session')
        self.assertFalse(any(path in ('/release', '/attach') for path, _ in daemon.calls))
        self.assertEqual(client.evaluate('42'), 42)
        self.assertEqual([body['sessionId'] for path, body in daemon.calls if path == '/eval'],
                         ['old-session', 'old-session'])

    def test_enqueue_requires_exact_complete_confirmation_and_never_replays_uncertainty(self):
        uri, request_id = 'spotify:track:' + 'a' * 22, 'fixture-request'
        valid = {'queued': True, 'uri': uri, 'requestId': request_id, 'uid': 'observed-new-uid'}
        for result in (None, False, [], {}, {'queued': True}, {**valid, 'queued': 1},
                       {**valid, 'uid': None}, {**valid, 'uid': ' '},
                       {**valid, 'uri': 'other'}, {**valid, 'requestId': 'other'}):
            with self.subTest(result=result):
                client = native.SpotifyClient()
                with patch.object(client, 'evaluate', side_effect=[{'ready': True}, result]) as evaluate:
                    with self.assertRaises(native.SpotifyError) as caught:
                        client.enqueue(uri, request_id)
                self.assertTrue(caught.exception.dispatched)
                self.assertEqual(evaluate.call_count, 2)
        client = native.SpotifyClient()
        with patch.object(client, 'evaluate', side_effect=[{'ready': True}, valid]) as evaluate:
            self.assertEqual(client.enqueue(uri, request_id), valid)
        self.assertEqual(evaluate.call_count, 2)

    def test_enqueue_preserves_explicit_preflight_and_uncertain_errors_once(self):
        for dispatched in (False, True, None):
            client = native.SpotifyClient()
            with patch.object(client, 'evaluate', side_effect=[{'ready': True},
                    {'error': 'Queue observation unavailable', 'dispatched': dispatched}]) as evaluate:
                with self.assertRaises(native.SpotifyError) as caught:
                    client.enqueue('spotify:track:' + 'a' * 22, 'stable-request')
            self.assertIs(caught.exception.dispatched, dispatched)
            self.assertEqual(evaluate.call_count, 2)
        client = native.SpotifyClient()
        with patch.object(client, 'evaluate', side_effect=[{'ready': True},
                native.SpotifyTransportError('Enqueue response lost')]) as evaluate:
            with self.assertRaises(native.SpotifyError) as caught:
                client.enqueue('spotify:track:' + 'a' * 22, 'stable-request')
        self.assertTrue(caught.exception.dispatched)
        self.assertEqual(evaluate.call_count, 2)

    def test_reorder_wrapper_preserves_all_preconditions_and_never_retries_errors(self):
        arguments = (['second', 'first'], ['first', 'second'], 'current-uid', 'device-id')
        for dispatched in (False, True):
            with self.subTest(dispatched=dispatched):
                client = native.SpotifyClient()
                with patch.object(client, 'evaluate', side_effect=[{'ready': True},
                        {'error': 'Queue changed', 'dispatched': dispatched}]) as evaluate:
                    with self.assertRaises(native.SpotifyError) as caught:
                        client.reorder_queue(*arguments)
                self.assertEqual(caught.exception.dispatched, dispatched)
                self.assertEqual(evaluate.call_count, 2)
                expression = evaluate.call_args_list[1].args[0]
                self.assertTrue(expression.startswith('window.__laricsDJ["reorderQueue"]('))
                sent = json.loads(expression.split('...', 1)[1][:-1])
                self.assertEqual(sent, list(arguments))

    def test_queue_bootstrap_failures_are_definitely_undispatched(self):
        for operation, arguments in (
                ('move_queue', ('second', 'first', ['first', 'second'])),
                ('reorder_queue', (['second', 'first'], ['first', 'second'], 'current', 'device')),
                ('set_paused', (True, 'current', 'device')),
                ('skip', ('next', 'current', 'device'))):
            for failure in ({'ready': False, 'error': 'Player loading'}, None, 'loading',
                            native.SpotifyTransportError('Bootstrap response lost')):
                with self.subTest(operation=operation, failure=str(failure)):
                    client = native.SpotifyClient()
                    with patch.object(client, 'evaluate', side_effect=[failure]) as evaluate:
                        with self.assertRaises(native.SpotifyError) as caught:
                            getattr(client, operation)(*arguments)
                    self.assertFalse(caught.exception.dispatched)
                    self.assertEqual(evaluate.call_count, 1)
            client = native.SpotifyClient()
            with patch.object(native.Path, 'read_text', side_effect=OSError('Missing bootstrap')):
                with patch.object(client, 'evaluate') as evaluate:
                    with self.assertRaises(native.SpotifyError) as caught:
                        getattr(client, operation)(*arguments)
            self.assertFalse(caught.exception.dispatched)
            evaluate.assert_not_called()

    def test_lost_queue_method_response_stays_uncertain_and_is_not_retried(self):
        client = native.SpotifyClient()
        with patch.object(client, 'evaluate', side_effect=[{'ready': True},
                native.SpotifyTransportError('Method response lost')]) as evaluate:
            with self.assertRaises(native.SpotifyTransportError) as caught:
                client.reorder_queue(['second', 'first'], ['first', 'second'], 'current', 'device')
        self.assertTrue(caught.exception.dispatched)
        self.assertEqual(evaluate.call_count, 2)

    def test_set_paused_preserves_explicit_arguments_and_fresh_success_or_error(self):
        for paused in (True, False):
            for result in (
                    {'ok': True, 'changed': False, 'paused': paused, 'status': {'nowPlaying': {'paused': paused}}},
                    {'error': 'Current occurrence changed', 'dispatched': False},
                    {'error': 'Confirmation timed out', 'dispatched': True}):
                with self.subTest(paused=paused, result=result):
                    client = native.SpotifyClient()
                    def evaluate(expression):
                        self.assertTrue(client.lock._is_owned(), 'Explicit playback left the shared operation lock.')
                        if expression.startswith('(async'):
                            return {'ready': True}
                        return result
                    with patch.object(client, 'evaluate', side_effect=evaluate) as called:
                        if 'error' in result:
                            with self.assertRaises(native.SpotifyError) as caught:
                                client.set_paused(paused, 'current', 'device')
                            self.assertEqual(caught.exception.dispatched, result['dispatched'])
                        else:
                            self.assertEqual(client.set_paused(paused, 'current', 'device'), result)
                    self.assertEqual(called.call_count, 2)
                    expression = called.call_args_list[-1].args[0]
                    self.assertTrue(expression.startswith('window.__laricsDJ["setPaused"]('))
                    self.assertEqual(json.loads(expression.split('...', 1)[1][:-1]), [paused, 'current', 'device'])

    def test_lost_set_paused_response_stays_uncertain_without_retry(self):
        client = native.SpotifyClient()
        with patch.object(client, 'evaluate', side_effect=[{'ready': True},
                native.SpotifyTransportError('Playback response lost')]) as evaluate:
            with self.assertRaises(native.SpotifyTransportError) as caught:
                client.set_paused(True, 'current', 'device')
        self.assertTrue(caught.exception.dispatched)
        self.assertEqual(evaluate.call_count, 2)

    def test_skip_preserves_arguments_confirmation_and_single_dispatch_errors(self):
        for direction in ('next', 'previous'):
            success = {'ok': True, 'action': 'skip', 'direction': direction, 'changed': True,
                       'outcome': direction, 'fromUid': 'current', 'toUid': 'new',
                       'device': {'id': 'device'}, 'before': {'currentUid': 'current'},
                       'status': {'nowPlaying': {'uid': 'new'}}}
            for result in (success, {'error': 'Current changed', 'dispatched': False},
                           {'error': 'Unconfirmed', 'dispatched': True}):
                with self.subTest(direction=direction, result=result):
                    client = native.SpotifyClient()
                    def evaluate(expression):
                        self.assertTrue(client.lock._is_owned(), 'Skip left the shared operation lock.')
                        return {'ready': True} if expression.startswith('(async') else result
                    with patch.object(client, 'evaluate', side_effect=evaluate) as called:
                        if 'error' in result:
                            with self.assertRaises(native.SpotifyError) as caught:
                                client.skip(direction, 'current', 'device')
                            self.assertEqual(caught.exception.dispatched, result['dispatched'])
                        else:
                            self.assertEqual(client.skip(direction, 'current', 'device'), result)
                    self.assertEqual(called.call_count, 2)
                    expression = called.call_args_list[-1].args[0]
                    self.assertTrue(expression.startswith('window.__laricsDJ["skip"]('))
                    self.assertEqual(json.loads(expression.split('...', 1)[1][:-1]), [direction, 'current', 'device'])

    def test_skip_rejects_invalid_input_before_bootstrap_and_malformed_or_lost_success_once(self):
        for args in (('forward', 'current', 'device'), ('next', '', 'device'), ('previous', 'current', None)):
            client = native.SpotifyClient()
            with patch.object(client, 'evaluate') as evaluate, self.assertRaises(native.SpotifyError) as caught:
                client.skip(*args)
            self.assertFalse(caught.exception.dispatched); evaluate.assert_not_called()
        for result in (None, {'ok': True}, native.SpotifyTransportError('Skip response lost')):
            client = native.SpotifyClient()
            with patch.object(client, 'evaluate', side_effect=[{'ready': True}, result]) as evaluate:
                with self.assertRaises(native.SpotifyError) as caught:
                    client.skip('next', 'current', 'device')
            self.assertTrue(caught.exception.dispatched); self.assertEqual(evaluate.call_count, 2)

    def test_reorder_holds_the_shared_client_lock_through_confirmation(self):
        client = native.SpotifyClient()
        entered, release, observer_started = threading.Event(), threading.Event(), threading.Event()
        operations = []
        fresh = {'nowPlaying': {'uid': 'current'}, 'queue': [{'uid': 'second'}, {'uid': 'first'}]}

        def evaluate(expression):
            if expression.startswith('(async'):
                return {'ready': True}
            if '["reorderQueue"]' in expression:
                operations.append('reorder-dispatched')
                entered.set()
                if not release.wait(5):
                    raise AssertionError('Test did not release reorder confirmation.')
                operations.append('reorder-confirmed')
                return {'ok': True, 'moved': True, 'status': fresh}
            if '["status"]' in expression:
                operations.append('status-observed')
                return fresh
            raise AssertionError('Unexpected fixture operation: ' + expression[:80])

        def observe():
            free = client.lock.acquire(blocking=False)
            if free:
                client.lock.release()
            observer_started.set()
            self.assertFalse(free, 'Reorder did not hold the shared Spotify client lock.')
            return client.status()

        with patch.object(client, 'evaluate', side_effect=evaluate):
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                reorder = pool.submit(client.reorder_queue, ['second', 'first'], ['first', 'second'], 'current', 'device')
                try:
                    self.assertTrue(entered.wait(5))
                    observed = pool.submit(observe)
                    self.assertTrue(observer_started.wait(5))
                finally:
                    release.set()
                self.assertEqual(reorder.result(timeout=5)['status'], fresh)
                self.assertEqual(observed.result(timeout=5), fresh)
        self.assertEqual(operations, ['reorder-dispatched', 'reorder-confirmed', 'status-observed'])


class CatalogueFixture:
    def __init__(self, client):
        self.client = client
        self.calls = []
        self.http_paths = []
        self.started = threading.Event()
        self.ready = threading.Event()
        self.active = None
        self.on_bootstrap = None
        self.on_method = None
        self.known = None
        self.tracks = [{'uri': 'spotify:track:' + 'b' * 22, 'title': 'Fixture song'}]
        self.status = {'nowPlaying': {'uid': 'current', 'paused': False}, 'queue': []}

    def http(self, url, body=None, timeout=25):
        path = urlsplit(url).path
        self.http_paths.append(path)
        if path in ('/release', '/cdp'):
            return {}
        if path != '/eval':
            raise AssertionError('Unexpected connection operation: ' + path)
        if not self.client.lock._is_owned():
            raise AssertionError('A search evaluation escaped the shared transport lock.')
        expression = body['expression']
        if expression.startswith('(async'):
            if self.on_bootstrap:
                self.on_bootstrap()
            return {'result': {'value': {'ready': True}}}
        cleanup = 'window.__laricsDJ?.cancelSearch?.('
        if expression.startswith(cleanup):
            method, arguments = 'cancelSearch', [json.loads(expression[len(cleanup):-1])]
        else:
            name, raw = expression[len('window.__laricsDJ['):].split('](...', 1)
            method, arguments = json.loads(name), json.loads(raw[:-1])
        self.calls.append((method, arguments))
        if self.on_method:
            overridden = self.on_method(method, arguments)
            if overridden is not NotImplemented:
                return {'result': {'value': overridden}}
        if method == 'beginSearch':
            query, identifier, expires_at = arguments
            if not time.time() * 1000 - 1000 <= expires_at <= time.time() * 1000 + 17000:
                raise AssertionError('Search admission has no bounded absolute deadline.')
            self.active = (query, identifier)
            self.started.set()
            result = {'searchId': identifier, 'state': 'pending'}
        elif method == 'pollSearch':
            identifier = arguments[0]
            if not self.active or self.active[1] != identifier:
                result = {'searchId': identifier, 'state': 'cancelled'}
            elif self.ready.is_set():
                result = {'searchId': identifier, 'state': 'complete', 'tracks': self.tracks}
                self.active = None
            else:
                result = {'searchId': identifier, 'state': 'pending'}
        elif method == 'cancelSearch':
            identifier = arguments[0]
            if self.active and self.active[1] == identifier:
                self.active = None
            result = {'searchId': identifier, 'state': 'cancelled'}
        elif method == 'status':
            result = self.status
        elif method == 'setPaused':
            result = {'ok': True, 'changed': True, 'status': self.status}
        elif method == 'trackInQueue':
            result = self.known
        else:
            raise AssertionError('Unexpected fixture method: ' + method)
        return {'result': {'value': result}}


class SpotifyCatalogueTests(unittest.TestCase):
    def setUp(self):
        for target, attribute in ((native.urllib.request, 'urlopen'), (native.websocket, 'create_connection')):
            guard = patch.object(target, attribute, side_effect=AssertionError('No real connection in catalogue fixtures'))
            guard.start()
            self.addCleanup(guard.stop)
        for name, value in (('SEARCH_POLL_INTERVAL', .01), ('SEARCH_LOCK_INTERVAL', .01)):
            setting = patch.object(native, name, value)
            setting.start()
            self.addCleanup(setting.stop)

    def fixture(self):
        client = native.SpotifyClient()
        client.session = 'fixture-existing-session'
        client.last_native_check = time.monotonic()
        daemon = CatalogueFixture(client)
        transport = patch.object(client, '_http', side_effect=daemon.http)
        transport.start()
        self.addCleanup(transport.stop)
        self.addCleanup(client.close)
        return client, daemon

    def test_cancel_before_admission_or_during_bootstrap_never_navigates(self):
        for during_bootstrap in (False, True):
            with self.subTest(during_bootstrap=during_bootstrap):
                client, daemon = self.fixture()
                cancelled = threading.Event()
                if during_bootstrap:
                    daemon.on_bootstrap = cancelled.set
                else:
                    cancelled.set()
                with self.assertRaises(native.SpotifySearchCancelled) as caught:
                    client.search('obsolete query', cancel_event=cancelled)
                self.assertFalse(caught.exception.dispatched)
                self.assertEqual(daemon.calls, [])
                self.assertFalse(any(path in ('/attach', '/prepare', '/release') for path in daemon.http_paths))
                self.assertEqual(client.session, 'fixture-existing-session')

    def test_pending_catalogue_yields_to_fresh_status_and_an_explicit_write(self):
        client, daemon = self.fixture()
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            search = pool.submit(client.search, 'slow catalogue')
            try:
                self.assertTrue(daemon.started.wait(2))
                self.assertEqual(pool.submit(client.status).result(timeout=1), daemon.status)
                self.assertTrue(pool.submit(client.set_paused, True, 'current', 'device').result(timeout=1)['ok'])
                self.assertFalse(search.done(), 'Search completed before its result was available.')
            finally:
                daemon.ready.set()
            self.assertEqual(search.result(timeout=2), daemon.tracks)
        methods = [method for method, unused in daemon.calls]
        self.assertLess(methods.index('beginSearch'), methods.index('status'))
        self.assertEqual(methods.count('setPaused'), 1)

    def test_superseded_waiter_never_starts_and_newest_search_can_follow(self):
        client, daemon = self.fixture()
        first_cancel, old_cancel, old_entered = threading.Event(), threading.Event(), threading.Event()
        def old_search():
            old_entered.set()
            return client.search('obsolete waiter', cancel_event=old_cancel)
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
            first = pool.submit(client.search, 'active old query', cancel_event=first_cancel)
            try:
                self.assertTrue(daemon.started.wait(2))
                old = pool.submit(old_search)
                self.assertTrue(old_entered.wait(2))
                old_cancel.set()
                with self.assertRaises(native.SpotifySearchCancelled):
                    old.result(timeout=1)
                newest = pool.submit(client.search, 'newest query')
                first_cancel.set()
                with self.assertRaises(native.SpotifySearchCancelled):
                    first.result(timeout=1)
                daemon.ready.set()
                self.assertEqual(newest.result(timeout=2), daemon.tracks)
            finally:
                first_cancel.set(); old_cancel.set(); daemon.ready.set()
        self.assertEqual([args[0] for method, args in daemon.calls if method == 'beginSearch'],
                         ['active old query', 'newest query'])
        self.assertEqual(client.session, 'fixture-existing-session')
        self.assertNotIn('/release', daemon.http_paths)

    def test_waiting_for_transport_can_be_cancelled_without_acquiring_it(self):
        client, daemon = self.fixture()
        cancelled, entered = threading.Event(), threading.Event()
        def search():
            entered.set()
            return client.search('obsolete', cancel_event=cancelled)
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            with client.lock:
                pending = pool.submit(search)
                self.assertTrue(entered.wait(2))
                cancelled.set()
                with self.assertRaises(native.SpotifySearchCancelled):
                    pending.result(timeout=1)
        self.assertEqual(daemon.http_paths, [])
        self.assertFalse(client.search_lock.locked())

    def test_search_cancel_does_not_interrupt_or_wait_behind_a_real_write(self):
        client, daemon = self.fixture()
        cancelled, write_entered, release_write = threading.Event(), threading.Event(), threading.Event()
        def handle(method, arguments):
            if method == 'setPaused':
                write_entered.set()
                if not release_write.wait(2):
                    raise AssertionError('The fixture did not release its explicit write.')
                return {'ok': True, 'status': daemon.status}
            return NotImplemented
        daemon.on_method = handle
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            searching = pool.submit(client.search, 'slow result', cancel_event=cancelled)
            try:
                self.assertTrue(daemon.started.wait(2))
                writing = pool.submit(client.set_paused, True, 'current', 'device')
                self.assertTrue(write_entered.wait(2))
                count = len(daemon.calls)
                cancelled.set()
                with self.assertRaises(native.SpotifySearchCancelled):
                    searching.result(timeout=1)
                self.assertFalse(writing.done())
                self.assertEqual(len(daemon.calls), count, 'Cancelled search interleaved an evaluation with the write.')
            finally:
                release_write.set(); cancelled.set(); daemon.ready.set()
            self.assertTrue(writing.result(timeout=2)['ok'])
        self.assertEqual(sum(method == 'setPaused' for method, unused in daemon.calls), 1)

    def test_close_cancels_active_and_waiting_reads_without_new_attachment(self):
        client, daemon = self.fixture()
        waiter_entered = threading.Event()
        acquire = client._search_acquire
        def observed_acquire(lock, *arguments):
            if lock is client.search_lock:
                waiter_entered.set()
            return acquire(lock, *arguments)
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            active = pool.submit(client.search, 'active before close')
            self.assertTrue(daemon.started.wait(2))
            with patch.object(client, '_search_acquire', side_effect=observed_acquire):
                waiting = pool.submit(client.search, 'queued before close')
                self.assertTrue(waiter_entered.wait(2))
                client.close()
                for future in (active, waiting):
                    with self.assertRaises(native.SpotifySearchCancelled):
                        future.result(timeout=1)
        self.assertEqual([args[0] for method, args in daemon.calls if method == 'beginSearch'], ['active before close'])
        self.assertFalse(any(path in ('/targets', '/attach', '/prepare') for path in daemon.http_paths))
        self.assertIsNone(client.session)

    def test_connection_change_abandons_the_old_job_without_rebinding(self):
        client, daemon = self.fixture()
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            search = pool.submit(client.search, 'old connection')
            self.assertTrue(daemon.started.wait(2))
            with client.lock:
                before = len(daemon.calls)
                client.session = 'replacement-owned-session'
            with self.assertRaises(native.SpotifySearchCancelled):
                search.result(timeout=1)
        self.assertFalse(any(method in ('pollSearch', 'cancelSearch') for method, unused in daemon.calls[before:]))
        self.assertFalse(any(path in ('/targets', '/attach', '/prepare', '/release') for path in daemon.http_paths))

    def test_unreadable_or_wrong_id_results_fail_once_and_release_the_catalogue_gate(self):
        for malformed in (None, {}, {'searchId': 'wrong', 'state': 'complete', 'tracks': []},
                          {'state': 'unknown'}, {'state': 'complete', 'tracks': None},
                          {'state': 'complete', 'tracks': ['not a track']}):
            with self.subTest(result=malformed):
                client, daemon = self.fixture()
                def reply(method, arguments):
                    if method != 'pollSearch':
                        return NotImplemented
                    return {'searchId': arguments[0], **malformed} if isinstance(malformed, dict) else malformed
                daemon.on_method = reply
                with self.assertRaises(native.SpotifyError):
                    client.search('fixture')
                self.assertEqual(sum(method == 'beginSearch' for method, unused in daemon.calls), 1)
                self.assertFalse(client.search_lock.locked())
                self.assertNotIn('/release', daemon.http_paths)

    def test_timeout_cleanup_does_not_reconnect_or_replay_search(self):
        client, daemon = self.fixture()
        with patch.object(native, 'SEARCH_TIMEOUT', .04):
            with self.assertRaisesRegex(native.SpotifyError, 'still loading'):
                client.search('never ready')
        self.assertEqual(sum(method == 'beginSearch' for method, unused in daemon.calls), 1)
        self.assertEqual(sum(method == 'cancelSearch' for method, unused in daemon.calls), 1)
        self.assertEqual(client.session, 'fixture-existing-session')
        self.assertNotIn('/release', daemon.http_paths)

    def test_track_lookup_keeps_queue_fast_path_and_yields_during_catalogue_fallback(self):
        client, daemon = self.fixture()
        uri = daemon.tracks[0]['uri']
        daemon.known = daemon.tracks[0]
        self.assertEqual(client.track(uri), daemon.known)
        self.assertEqual([method for method, unused in daemon.calls], ['trackInQueue'])
        daemon.known = None
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            lookup = pool.submit(client.track, uri)
            try:
                self.assertTrue(daemon.started.wait(2))
                self.assertEqual(pool.submit(client.status).result(timeout=1), daemon.status)
            finally:
                daemon.ready.set()
            self.assertEqual(lookup.result(timeout=2), daemon.tracks[0])
        self.assertEqual([args[0] for method, args in daemon.calls if method == 'beginSearch'], [uri])


if __name__ == '__main__':
    unittest.main()
