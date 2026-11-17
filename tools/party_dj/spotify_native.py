"""Control the user's Spotify client through its existing authenticated UI."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import threading
import time
import urllib.error
import urllib.request
import uuid

import websocket


class SpotifyError(RuntimeError):
    def __init__(self, message, dispatched=True):
        super().__init__(message)
        self.dispatched = dispatched


class SpotifyTransportError(SpotifyError):
    """The browser/native control connection needs to be established again."""


class SpotifySearchCancelled(SpotifyError):
    """An obsolete catalogue read ended without a playback or queue command."""

    def __init__(self, message='This catalogue search was cancelled.'):
        super().__init__(message, dispatched=False)


SEARCH_TIMEOUT = 16.0
SEARCH_POLL_INTERVAL = .18
SEARCH_LOCK_INTERVAL = .05


class _SearchCancellation:
    def __init__(self, client, event):
        self.client, self.event = client, event
        self.epoch = client._search_epoch

    def is_set(self):
        return self.epoch != self.client._search_epoch or (self.event is not None and self.event.is_set())

    def wait(self, timeout):
        deadline = time.monotonic() + timeout
        while not self.is_set():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return False
            delay = min(SEARCH_LOCK_INTERVAL, remaining)
            self.event.wait(delay) if self.event is not None else time.sleep(delay)
        return True


class SpotifyClient:
    def __init__(self, native_port=9224, daemon='http://127.0.0.1:7799'):
        self.native_port = native_port
        self.daemon = daemon
        self.lock = threading.RLock()
        self.search_lock = threading.Lock()
        self._search_epoch = 0
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
        if not candidates:
            raise SpotifyTransportError('No existing Spotify browser controller is available. '
                'Run spotify_reconnect.py ensure explicitly to prepare the connection.')
        target = candidates[0]['targetId']
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
            self._search_epoch += 1
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

    def _evaluate_connected(self, expression, timeout=None):
        """Evaluate under self.lock using only the caller's existing connection."""
        params = {'expression': expression, 'returnByValue': True, 'awaitPromise': True, 'userGesture': True}
        if self.ws:
            self.sequence += 1
            ident = self.sequence
            previous_timeout = self.ws.gettimeout() if timeout is not None else None
            if timeout is not None:
                self.ws.settimeout(timeout)
            try:
                self.ws.send(json.dumps({'id': ident, 'method': 'Runtime.evaluate', 'params': params}))
                deadline = time.monotonic() + (25 if timeout is None else timeout)
                while True:
                    if time.monotonic() > deadline:
                        raise SpotifyTransportError('The native Spotify control did not respond in time.')
                    event = json.loads(self.ws.recv())
                    if event.get('id') == ident:
                        if 'error' in event:
                            raise SpotifyTransportError(str(event['error']))
                        result = event['result']
                        break
            finally:
                if timeout is not None:
                    self.ws.settimeout(previous_timeout)
        else:
            result = self._http(self.daemon + '/eval', {'sessionId': self.session,
                'expression': expression, 'returnByValue': True, 'awaitPromise': True,
                'timeout_seconds': 20 if timeout is None else timeout},
                timeout=25 if timeout is None else timeout + 1)
        if 'exceptionDetails' in result:
            exception = result['exceptionDetails'].get('exception', {})
            description = exception.get('description') or result['exceptionDetails'].get('text', 'Spotify command failed.')
            raise SpotifyError(description.split('\n', 1)[0].removeprefix('Error: '))
        if 'error' in result:
            raise SpotifyTransportError(str(result['error']))
        return result.get('result', {}).get('value')

    def evaluate(self, expression):
        with self.lock:
            try:
                self._connect()
                return self._evaluate_connected(expression)
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

    @staticmethod
    def _check_search(cancel_event, deadline):
        if cancel_event is not None and cancel_event.is_set():
            raise SpotifySearchCancelled()
        if time.monotonic() >= deadline:
            raise SpotifyError('Spotify search is still loading. Try again in a moment.', dispatched=False)

    def _search_acquire(self, lock, cancel_event, deadline):
        while True:
            self._check_search(cancel_event, deadline)
            if lock.acquire(timeout=min(SEARCH_LOCK_INTERVAL, max(0, deadline - time.monotonic()))):
                try:
                    self._check_search(cancel_event, deadline)
                except Exception:
                    lock.release()
                    raise
                return

    def _connection_key(self):
        return (id(self.ws) if self.ws is not None else None, self.target, self.session)

    def _search_step(self, method, arguments, cancel_event, deadline, connection=None):
        self._search_acquire(self.lock, cancel_event, deadline)
        try:
            if connection is not None and connection != self._connection_key():
                raise SpotifySearchCancelled('The catalogue connection changed before its result arrived.')
            ready = self.evaluate((Path(__file__).parent / 'spotify_bridge.js').read_text())
            if not isinstance(ready, dict) or not ready.get('ready'):
                detail = ready.get('error') if isinstance(ready, dict) else None
                raise SpotifyError(detail or 'Spotify is still loading.', dispatched=False)
            # Cancellation may arrive during connection/bootstrap, before a
            # browser search has been admitted. Do not navigate after it.
            self._check_search(cancel_event, deadline)
            if connection is not None and connection != self._connection_key():
                raise SpotifySearchCancelled('The catalogue connection changed before its result arrived.')
            if method == 'beginSearch':
                # CDP response expiry does not revoke a queued evaluation.
                # A late begin must not navigate after this read's budget.
                expires_at = int(time.time() * 1000 + max(0, deadline - time.monotonic()) * 1000)
                arguments = [*arguments, expires_at]
            expression = f'window.__laricsDJ[{json.dumps(method)}](...{json.dumps(arguments, ensure_ascii=False)})'
            return self.evaluate(expression), self._connection_key()
        finally:
            self.lock.release()

    def _abandon_search(self, identifier, connection):
        # A search job contains no running JS loop. Cleanup is best-effort and
        # must neither wait behind a real command nor reconnect a lost session.
        if not self.lock.acquire(blocking=False):
            return
        try:
            if connection != self._connection_key() or not (self.ws or self.session):
                return
            self._evaluate_connected(f'window.__laricsDJ?.cancelSearch?.({json.dumps(identifier)})', timeout=1)
        except Exception:
            pass
        finally:
            self.lock.release()

    def search(self, query, *, cancel_event=None):
        """Keep one catalogue route, yielding the transport lock while it loads."""
        cancel_event = _SearchCancellation(self, cancel_event)
        deadline = time.monotonic() + SEARCH_TIMEOUT
        self._search_acquire(self.search_lock, cancel_event, deadline)
        identifier = uuid.uuid4().hex
        connection = None
        finished = False
        try:
            result, connection = self._search_step('beginSearch', [query, identifier], cancel_event, deadline)
            while True:
                self._check_search(cancel_event, deadline)
                if not isinstance(result, dict) or result.get('searchId') != identifier:
                    raise SpotifyError('Spotify returned a mismatched catalogue response.', dispatched=False)
                state = result.get('state')
                if state == 'cancelled':
                    raise SpotifySearchCancelled()
                if state == 'error':
                    raise SpotifyError(result.get('error') or 'Spotify search failed.', dispatched=False)
                if state == 'complete':
                    tracks = result.get('tracks')
                    if not isinstance(tracks, list) or any(not isinstance(track, dict) for track in tracks):
                        raise SpotifyError('Spotify returned an unreadable catalogue result.', dispatched=False)
                    finished = True
                    return tracks
                if state != 'pending':
                    raise SpotifyError('Spotify returned an unreadable catalogue state.', dispatched=False)
                delay = min(SEARCH_POLL_INTERVAL, max(0, deadline - time.monotonic()))
                if cancel_event is not None:
                    cancel_event.wait(delay)
                else:
                    time.sleep(delay)
                result, unused = self._search_step('pollSearch', [identifier], cancel_event, deadline, connection)
        finally:
            if connection is not None and not finished:
                self._abandon_search(identifier, connection)
            self.search_lock.release()

    def track(self, uri):
        cancelled = _SearchCancellation(self, None)
        known = self.call('trackInQueue', uri)
        if cancelled.is_set():
            raise SpotifySearchCancelled()
        if known is not None:
            if not isinstance(known, dict) or known.get('uri') != uri:
                raise SpotifyError('Spotify returned an unreadable track.', dispatched=False)
            return known
        found = next((track for track in self.search(uri, cancel_event=cancelled) if track.get('uri') == uri), None)
        if found is None:
            raise SpotifyError('Find this track in search again before requesting it.', dispatched=False)
        return found

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
            if (not isinstance(result, dict) or result.get('queued') is not True
                    or result.get('uri') != uri or result.get('requestId') != request_id
                    or not isinstance(result.get('uid'), str) or not result['uid'].strip()):
                raise SpotifyError('Spotify returned an incomplete queue-addition confirmation; inspect the actual queue.',
                                   dispatched=True)
            return result

    def control(self, action):
        return self.call('control', action)

    def _guarded_change(self, operation, *arguments):
        # Keep the same lock through preparation, dispatch and observation. A
        # bootstrap failure is definitely unsent; losing the method response is
        # uncertain and must never trigger a retry here.
        with self.lock:
            try:
                bootstrap = (Path(__file__).parent / 'spotify_bridge.js').read_text()
                ready = self.evaluate(bootstrap)
                if not isinstance(ready, dict) or not ready.get('ready'):
                    detail = ready.get('error') if isinstance(ready, dict) else None
                    raise SpotifyError(detail or 'Spotify is still loading.')
                expression = f'window.__laricsDJ[{json.dumps(operation)}](...{json.dumps(arguments, ensure_ascii=False)})'
            except Exception as error:
                raise SpotifyError(str(error), dispatched=False) from error
            result = self.evaluate(expression)
            if isinstance(result, dict) and result.get('error'):
                raise SpotifyError(result['error'], dispatched=result.get('dispatched', True))
            return result

    def move_queue(self, uid, before_uid, expected_queued_uids):
        return self._guarded_change('moveQueue', uid, before_uid, expected_queued_uids)

    def reorder_queue(self, ordered_queued_uids, expected_queued_uids, expected_current_uid, expected_device_id):
        return self._guarded_change('reorderQueue', ordered_queued_uids, expected_queued_uids,
                                    expected_current_uid, expected_device_id)

    def set_paused(self, paused, expected_current_uid, expected_device_id):
        return self._guarded_change('setPaused', paused, expected_current_uid, expected_device_id)


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
