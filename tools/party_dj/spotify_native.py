"""Control the user's Spotify client through its existing authenticated UI."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import threading
import time
import urllib.error
import urllib.request

import websocket


class SpotifyError(RuntimeError):
    def __init__(self, message, dispatched=True):
        super().__init__(message)
        self.dispatched = dispatched


class SpotifyTransportError(SpotifyError):
    """The browser/native control connection needs to be established again."""


class SpotifyClient:
    def __init__(self, native_port=9224, daemon='http://127.0.0.1:7799'):
        self.native_port = native_port
        self.daemon = daemon
        self.lock = threading.RLock()
        self.ws = None
        self.target = None
        self.session = None
        self.mode = 'connecting'
        self.sequence = 0
        self.last_native_check = -100
        self.native_targets = []
        self.created_web_target = None

    def _http(self, url, body=None, timeout=25):
        data = json.dumps(body).encode() if body is not None else None
        request = urllib.request.Request(url, data=data, headers={'Content-Type': 'application/json'})
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                return json.load(response)
        except urllib.error.HTTPError as error:
            detail = error.read().decode(errors='replace')[:300]
            raise SpotifyTransportError(f'Spotify control returned HTTP {error.code}: {detail}') from error

    def _connect(self):
        if time.monotonic() - self.last_native_check > 10:
            self.last_native_check = time.monotonic()
            try:
                self.native_targets = self._http(f'http://127.0.0.1:{self.native_port}/json/list', timeout=1)
            except (OSError, ValueError, SpotifyError):
                self.native_targets = []
        candidates = [x for x in self.native_targets if x.get('type') == 'page' and x.get('webSocketDebuggerUrl')]
        candidates.sort(key=lambda x: ('xpui' not in x.get('url', ''), 'spotify' not in x.get('url', '')))
        if candidates:
            candidate = candidates[0]
            if self.ws is None or self.target != candidate['id']:
                self._release_browser()
                if self.ws:
                    self.ws.close()
                self.ws = websocket.create_connection(candidate['webSocketDebuggerUrl'], timeout=25, suppress_origin=True)
                self.target = candidate['id']
                self.mode = 'native'
            return
        if self.ws:
            self.ws.close()
            self.ws = None
        if self.session:
            return
        targets = self._http(self.daemon + '/targets')
        candidates = [x for x in targets if x.get('type') == 'page' and x.get('url', '').startswith('https://open.spotify.com/')]
        if candidates:
            target = candidates[0]['targetId']
        elif self.created_web_target:
            target = self.created_web_target
        else:
            result = self._http(self.daemon + '/cdp', {'method': 'Target.createTarget',
                'params': {'url': 'https://open.spotify.com/', 'background': True}})
            target = result['result']['targetId']
            self.created_web_target = target
        self.session = self._http(self.daemon + '/attach', {'targetId': target})['sessionId']
        self._http(self.daemon + '/prepare', {'sessionId': self.session, 'readiness': 'dom', 'timeout_seconds': 15})
        self.mode = 'native-via-web-controller'

    def _release_browser(self):
        if self.session:
            sid, self.session = self.session, None
            for endpoint, body in (
                ('/release', {'sessionId': sid, 'timeout_seconds': 5}),
                ('/cdp', {'method': 'Target.detachFromTarget', 'params': {'sessionId': sid}, 'timeout_seconds': 5}),
            ):
                try:
                    self._http(self.daemon + endpoint, body, timeout=7)
                except Exception:
                    pass

    def close(self):
        with self.lock:
            self._release_browser()
            if self.ws:
                self.ws.close()
                self.ws = None

    def _reset_transport(self):
        self._release_browser()
        if self.ws:
            try:
                self.ws.close()
            except Exception:
                pass
        self.ws = None
        self.target = None
        self.created_web_target = None
        self.last_native_check = -100
        self.mode = 'connecting'

    def evaluate(self, expression):
        with self.lock:
            try:
                self._connect()
                params = {'expression': expression, 'returnByValue': True, 'awaitPromise': True, 'userGesture': True}
                if self.ws:
                    self.sequence += 1
                    ident = self.sequence
                    self.ws.send(json.dumps({'id': ident, 'method': 'Runtime.evaluate', 'params': params}))
                    deadline = time.monotonic() + 25
                    while True:
                        if time.monotonic() > deadline:
                            raise SpotifyTransportError('The native Spotify control did not respond in time.')
                        event = json.loads(self.ws.recv())
                        if event.get('id') == ident:
                            if 'error' in event:
                                raise SpotifyTransportError(str(event['error']))
                            result = event['result']
                            break
                else:
                    result = self._http(self.daemon + '/eval', {'sessionId': self.session,
                        'expression': expression, 'returnByValue': True, 'awaitPromise': True, 'timeout_seconds': 20})
                if 'exceptionDetails' in result:
                    exception = result['exceptionDetails'].get('exception', {})
                    description = exception.get('description') or result['exceptionDetails'].get('text', 'Spotify command failed.')
                    raise SpotifyError(description.split('\n', 1)[0].removeprefix('Error: '))
                if 'error' in result:
                    raise SpotifyTransportError(str(result['error']))
                return result.get('result', {}).get('value')
            except SpotifyTransportError:
                self._reset_transport()
                raise
            except SpotifyError:
                raise
            except Exception as error:
                self._reset_transport()
                raise SpotifyError(f'Spotify connection is recovering: {type(error).__name__}: {error}') from error

    def call(self, method, *args):
        with self.lock:
            bootstrap = (Path(__file__).parent / 'spotify_bridge.js').read_text()
            ready = self.evaluate(bootstrap)
            if not isinstance(ready, dict) or not ready.get('ready'):
                raise SpotifyError((ready or {}).get('error', 'Spotify is still loading.'))
            expression = f'window.__laricsDJ[{json.dumps(method)}](...{json.dumps(args, ensure_ascii=False)})'
            return self.evaluate(expression)

    def status(self):
        return self.call('status')

    def search(self, query):
        return self.call('search', query)

    def track(self, uri):
        return self.call('track', uri)

    def enqueue(self, uri, request_id):
        with self.lock:
            try:
                bootstrap = (Path(__file__).parent / 'spotify_bridge.js').read_text()
                ready = self.evaluate(bootstrap)
                if not isinstance(ready, dict) or not ready.get('ready'):
                    raise SpotifyError((ready or {}).get('error', 'Spotify is still loading.'), dispatched=False)
            except Exception as error:
                raise SpotifyError(str(error), dispatched=False) from error
            result = self.evaluate(f'window.__laricsDJ.enqueue({json.dumps(uri)}, {json.dumps(request_id)})')
            if isinstance(result, dict) and result.get('error'):
                raise SpotifyError(result['error'], dispatched=result.get('dispatched', True))
            return result

    def control(self, action):
        return self.call('control', action)

    def move_queue(self, uid, before_uid, expected_queued_uids):
        # call() holds the same RLock as status/search/enqueue through bootstrap,
        # dispatch and observation. A failed move is never retried here.
        result = self.call('moveQueue', uid, before_uid, expected_queued_uids)
        if isinstance(result, dict) and result.get('error'):
            raise SpotifyError(result['error'], dispatched=result.get('dispatched', True))
        return result


def main():
    p = argparse.ArgumentParser()
    p.add_argument('action', choices=['eval', 'status', 'search', 'track'])
    p.add_argument('value', nargs='?', default='')
    p.add_argument('--file', type=Path)
    args = p.parse_args()
    client = SpotifyClient()
    try:
        if args.action == 'eval':
            result = client.evaluate(args.file.read_text() if args.file else args.value)
        else:
            result = getattr(client, args.action)(*([args.value] if args.action != 'status' else []))
        print(json.dumps(result, ensure_ascii=False, indent=2))
    finally:
        client.close()


if __name__ == '__main__':
    main()
