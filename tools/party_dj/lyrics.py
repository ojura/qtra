"""Bounded current-recording lyrics through the already attached Spotify client."""
from __future__ import annotations

import json
import math
from pathlib import Path
import re
import threading
import time
from urllib.parse import urlsplit
import uuid


READ_TIMEOUT = 8.0
POLL_INTERVAL = .12
LOCK_INTERVAL = .05
STEP_TIMEOUT = 1.0
TRACK = re.compile(r'^spotify:track:[A-Za-z0-9]{22}$')
MAX_LINES = 1000
MAX_TEXT = 100000


class LyricsReadError(RuntimeError):
    def __init__(self, message, *, status=503, code='lyrics_read_failed'):
        super().__init__(message)
        self.status, self.code = status, code


def _target(value):
    if (not isinstance(value, dict) or set(value) != {'uid', 'uri', 'deviceId'}
            or not all(isinstance(value.get(key), str) and value[key].strip()
                       and len(value[key]) <= 512 for key in ('uid', 'deviceId'))
            or not isinstance(value.get('uri'), str) or not TRACK.fullmatch(value['uri'])):
        raise LyricsReadError('Lyrics require an exact current recording and device.')
    return value.copy()


def _unavailable(target, reason):
    return {'target': target.copy(), 'availability': 'unavailable', 'timing': 'none',
            'provider': None, 'lines': [], 'reason': reason}


def validate_result(value, target):
    """Copy only the public cue schema; never publish raw provider data or errors."""
    target = _target(target)
    invalid = LyricsReadError('The lyrics source returned an invalid response.')
    if not isinstance(value, dict) or value.get('target') != target:
        raise invalid
    availability, timing = value.get('availability'), value.get('timing')
    if availability not in ('ready', 'unavailable', 'instrumental') or timing not in ('line', 'none'):
        raise invalid
    provider = value.get('provider')
    if provider is not None:
        if not isinstance(provider, dict) or set(provider) != {'name', 'attribution', 'sourceUrl'}:
            raise invalid
        if not all(isinstance(provider[key], str) and provider[key].strip()
                   and len(provider[key]) <= 200 for key in ('name', 'attribution')):
            raise invalid
        source = provider['sourceUrl']
        if source is not None:
            if not isinstance(source, str) or len(source) > 2048 or any(c.isspace() or ord(c) < 32 for c in source):
                raise invalid
            try:
                parsed = urlsplit(source)
                if parsed.scheme != 'https' or not parsed.hostname or parsed.username is not None or parsed.password is not None:
                    raise invalid
                parsed.port
            except ValueError:
                raise invalid from None
        provider = provider.copy()
    raw_lines = value.get('lines')
    if not isinstance(raw_lines, list) or len(raw_lines) > MAX_LINES:
        raise invalid
    lines, previous, characters = [], -1, 0
    for line in raw_lines:
        if not isinstance(line, dict) or set(line) != {'text', 'startMs'}:
            raise invalid
        text, start = line['text'], line['startMs']
        if (not isinstance(text, str) or len(text) > 2000
                or any(ord(c) < 32 and c not in '\n\t' for c in text)):
            raise invalid
        characters += len(text)
        if characters > MAX_TEXT:
            raise invalid
        if timing == 'line':
            if (type(start) not in (int, float) or not math.isfinite(start)
                    or start < previous or start < 0 or start > 86400000):
                raise invalid
            previous = start
        elif start is not None:
            raise invalid
        lines.append({'text': text, 'startMs': start})
    if availability == 'ready':
        if provider is None or not any(line['text'].strip() for line in lines):
            raise invalid
    elif lines or timing != 'none':
        raise invalid
    result = {'target': target, 'availability': availability, 'timing': timing,
              'provider': provider, 'lines': lines}
    if 'reason' in value:
        reason = value['reason']
        if not isinstance(reason, str) or not re.fullmatch(r'[a-z][a-z0-9_]{0,79}', reason):
            raise invalid
        result['reason'] = reason
    return result


class LyricsReader:
    """Use short connected evaluations, releasing the shared lock between polls."""
    def __init__(self, client):
        self.client = client

    @staticmethod
    def _remaining(deadline):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise LyricsReadError('The lyrics read timed out.')
        return remaining

    def _step(self, expression, deadline, connection=None):
        client = self.client
        while not client.lock.acquire(timeout=min(LOCK_INTERVAL, self._remaining(deadline))):
            pass
        try:
            self._remaining(deadline)
            if not (client.ws or client.session):
                raise LyricsReadError('The existing Spotify connection is unavailable.')
            current = client._connection_key()
            if connection is not None and current != connection:
                raise LyricsReadError('The Spotify connection changed during the lyrics read.')
            result = client._evaluate_connected(expression, timeout=min(STEP_TIMEOUT, self._remaining(deadline)))
            self._remaining(deadline)
            if client._connection_key() != current:
                raise LyricsReadError('The Spotify connection changed during the lyrics read.')
            return result, current
        except LyricsReadError:
            raise
        except Exception:
            # A CDP/provider exception may contain source data; keep it private.
            raise LyricsReadError('The existing Spotify connection could not read lyrics.') from None
        finally:
            client.lock.release()

    def _abandon(self, identifier, connection):
        client = self.client
        if connection is None or not client.lock.acquire(blocking=False):
            return
        try:
            if connection == client._connection_key() and (client.ws or client.session):
                client._evaluate_connected(
                    f'window.__laricsLyrics?.cancel?.({json.dumps(identifier)})', timeout=.2)
        except Exception:
            pass
        finally:
            client.lock.release()

    def read(self, target, recording):
        target = _target(target)
        if not isinstance(recording, dict) or recording.get('uri') != target['uri'] or recording.get('uid') != target['uid']:
            raise LyricsReadError('The current recording changed before the lyrics read.',
                                  status=409, code='stale_target')
        art = recording.get('artUrl')
        if (not isinstance(art, str) or not art.startswith('https://') or len(art) > 2048
                or any(ord(c) < 32 for c in art)):
            return _unavailable(target, 'missing_artwork')
        duration = recording.get('durationMs')
        if type(duration) not in (int, float) or not math.isfinite(duration) or not 0 < duration <= 86400000:
            return _unavailable(target, 'missing_metadata')
        deadline = time.monotonic() + READ_TIMEOUT
        identifier, connection, started, finished = uuid.uuid4().hex, None, False, False
        try:
            source = Path(__file__).with_name('spotify_lyrics.js').read_text(encoding='utf-8')
            ready, connection = self._step(source, deadline)
            if not isinstance(ready, dict):
                raise LyricsReadError('The lyrics helper could not be loaded.')
            if ready.get('ready') is not True:
                return _unavailable(target, 'provider_unavailable')
            expires = int(time.time() * 1000 + self._remaining(deadline) * 1000)
            arguments = [identifier, target, {'artUrl': art, 'durationMs': duration}, expires]
            started = True  # A timed-out evaluation may still have begun the read.
            result, _ = self._step('window.__laricsLyrics.begin(...' + json.dumps(arguments) + ')', deadline, connection)
            while True:
                if not isinstance(result, dict) or result.get('id') != identifier:
                    raise LyricsReadError('The lyrics helper returned a mismatched response.')
                state = result.get('state')
                if state == 'done':
                    checked = validate_result(result.get('result'), target)
                    finished = True
                    return checked
                if state == 'stale':
                    raise LyricsReadError('The current recording or device changed during the lyrics read.',
                                          status=409, code='stale_target')
                if state != 'pending':
                    raise LyricsReadError('The current lyrics are temporarily unavailable.')
                threading.Event().wait(min(POLL_INTERVAL, self._remaining(deadline)))
                result, _ = self._step(f'window.__laricsLyrics.poll({json.dumps(identifier)})', deadline, connection)
        except LyricsReadError:
            raise
        except Exception:
            raise LyricsReadError('The current lyrics could not be read.') from None
        finally:
            if started and not finished:
                self._abandon(identifier, connection)
