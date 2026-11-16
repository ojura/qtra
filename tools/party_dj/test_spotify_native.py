"""Connection recovery checks with an in-memory CDP daemon and no Spotify calls."""
from __future__ import annotations

import io
import json
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


if __name__ == '__main__':
    unittest.main()
