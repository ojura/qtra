"""Validated syllable cues and a playback clock for the native karaoke stage."""
from __future__ import annotations

import copy
import math
import re


TRACK = re.compile(r'^spotify:track:[A-Za-z0-9]{22}$')
MAX_TIME = 24 * 60 * 60 * 1000


def milliseconds(value, name, minimum=0, maximum=MAX_TIME):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise ValueError(f'{name} must be a finite number of milliseconds.')
    if not minimum <= value <= maximum:
        raise ValueError(f'{name} must be between {minimum} and {maximum}.')
    return int(round(value))


def label(value, name, maximum=180):
    if not isinstance(value, str) or not value.strip() or len(value) > maximum:
        raise ValueError(f'{name} must contain 1 to {maximum} characters.')
    if any(ord(c) < 32 for c in value):
        raise ValueError(f'{name} must be a single line.')
    return value


def normalize(body, current, stamp):
    source = body.get('source', 'spotify')
    if source not in ('spotify', 'manual'):
        raise ValueError('Karaoke source must be spotify or manual.')
    title = label(body.get('title', 'Karaoke'), 'title')
    uri = body.get('trackUri', '')
    if source == 'spotify' and (not isinstance(uri, str) or not TRACK.fullmatch(uri)):
        raise ValueError('Spotify karaoke needs the selected trackUri.')
    uid = body.get('trackUid', '')
    if not isinstance(uid, str) or len(uid) > 180:
        raise ValueError('trackUid must be text.')
    if source == 'spotify' and not uid and uri in (current.get('uri'), current.get('requestedUri')):
        uid = current.get('uid', '')
    rows = body.get('lines')
    if not isinstance(rows, list) or not 1 <= len(rows) <= 300:
        raise ValueError('Provide 1 to 300 timed lyric lines.')
    lines, previous_end, count = [], 0, 0
    for row in rows:
        if not isinstance(row, dict) or not isinstance(row.get('syllables'), list) or not row['syllables']:
            raise ValueError('Every lyric line needs timed syllables.')
        syllables, syllable_end = [], previous_end
        for raw in row['syllables']:
            if not isinstance(raw, dict):
                raise ValueError('Each syllable must be an object.')
            text = label(raw.get('text'), 'syllable text', 80)
            start = milliseconds(raw.get('startMs'), 'startMs')
            end = milliseconds(raw.get('endMs'), 'endMs')
            if start < syllable_end or end <= start:
                raise ValueError('Syllable intervals must be ordered and must not overlap.')
            syllables.append({'text': text, 'startMs': start, 'endMs': end})
            syllable_end = end
            count += 1
            if count > 4000:
                raise ValueError('A karaoke sheet can contain at most 4000 syllables.')
        text = ''.join(item['text'] for item in syllables)
        label(text, 'lyric line', 240)
        start = milliseconds(row.get('startMs', syllables[0]['startMs']), 'line startMs')
        end = milliseconds(row.get('endMs', syllables[-1]['endMs']), 'line endMs')
        if start < previous_end or start > syllables[0]['startMs'] or end < syllables[-1]['endMs']:
            raise ValueError('Line intervals must contain their syllables and must not overlap.')
        lines.append({'text': text, 'startMs': start, 'endMs': end, 'syllables': syllables})
        previous_end = end
    duration = milliseconds(body.get('durationMs', previous_end), 'durationMs', 1)
    if duration < previous_end:
        raise ValueError('The last cue exceeds durationMs.')
    offset = milliseconds(body.get('offsetMs', 0), 'offsetMs', -60000, 60000)
    autoplay = body.get('autoplay', False)
    if not isinstance(autoplay, bool):
        raise ValueError('autoplay must be true or false.')
    return {'enabled': True, 'title': title, 'source': source, 'trackUri': uri,
            'trackUid': uid, 'durationMs': duration, 'offsetMs': offset, 'lines': lines,
            '_positionMs': 0, '_sampledAt': stamp, '_playing': source == 'manual' and autoplay}


class Karaoke:
    def __init__(self, saved=None):
        self.sheet = copy.deepcopy(saved)

    def saved(self):
        return copy.deepcopy(self.sheet)

    def _manual_position(self, stamp):
        sheet = self.sheet
        elapsed = max(0, stamp - sheet['_sampledAt']) if sheet['_playing'] else 0
        # Keep the raw clock running through a negative cue offset so delayed
        # final syllables can finish after the unadjusted duration.
        return sheet['_positionMs'] + elapsed

    def control(self, body, current, stamp):
        command = body.get('command', 'load')
        if command == 'load':
            # Validate the entire sheet before replacing the one on screen.
            self.sheet = normalize(body, current or {}, stamp)
            return
        if command == 'stop':
            self.sheet = None
            return
        if not self.sheet:
            raise ValueError('Load a karaoke sheet first.')
        if command == 'offset':
            offset = milliseconds(body.get('offsetMs'), 'offsetMs', -60000, 60000)
            self.sheet['offsetMs'] = offset
            return
        if command not in ('start', 'pause', 'resume', 'seek'):
            raise ValueError('Unknown karaoke command.')
        if self.sheet['source'] != 'manual':
            raise ValueError('Spotify karaoke follows Spotify transport; use offset to adjust lyric timing.')
        position = self._manual_position(stamp)
        if command == 'start':
            position = 0
        elif command == 'seek':
            position = milliseconds(body.get('positionMs'), 'positionMs', 0, self.sheet['durationMs'])
        self.sheet['_positionMs'] = position
        self.sheet['_sampledAt'] = stamp
        if command in ('start', 'resume'):
            self.sheet['_playing'] = True
        elif command == 'pause':
            self.sheet['_playing'] = False

    def snapshot(self, current, connected, stamp):
        if not self.sheet:
            return None
        sheet = self.sheet
        result = {key: value for key, value in sheet.items() if not key.startswith('_')}
        current = current or {}
        if sheet['source'] == 'manual':
            position = self._manual_position(stamp)
            playing = sheet['_playing']
            matched = True
        else:
            matched = connected and (sheet['trackUid'] == current.get('uid') if sheet['trackUid']
                else sheet['trackUri'] in (current.get('uri'), current.get('requestedUri')))
            playing = bool(current.get('playing')) and matched
            elapsed = max(0, stamp - current.get('observedAt', stamp)) if playing else 0
            position = current.get('progressMs', 0) + elapsed if matched else 0
        # A negative anchor preserves a future lyric start during interpolation.
        # The renderer clamps only after adding elapsed time to this anchor.
        adjusted = min(sheet['durationMs'], position + sheet['offsetMs']) if matched else 0
        status = ('waiting' if not matched else 'finished' if adjusted >= sheet['durationMs']
                  else 'playing' if playing else 'paused')
        result.update({'status': status, 'positionMs': int(adjusted), 'sampledAt': stamp})
        return result
