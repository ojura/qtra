#!/usr/bin/env python3
"""Local song requests, live DJ chat and state for qtra's party stage."""
from __future__ import annotations

import argparse
import collections
import hmac
import json
import math
import mimetypes
import os
from pathlib import Path
import re
import secrets
import signal
import socket
import sqlite3
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit, unquote

from spotify_native import SpotifyClient, SpotifyError
from speech import DedicationAudio
from karaoke import Karaoke

ROOT = Path(__file__).resolve().parent
DEFAULT_DATA = Path.home() / '.local/state/larics-dj'
MOODS = {'chill': ('Lagano', .22), 'groove': ('Groove', .48),
         'dance': ('Plešemo', .74), 'chaos': ('Kaos', .95),
         'singalong': ('Pjevamo', .64)}
TRACK = re.compile(r'^spotify:track:[A-Za-z0-9]{22}$')
AUDIO_STYLES = ('ring', 'city', 'waterfall')
AUDIO_ACT_MS = 10 * 60 * 1000
PROCESSING_LEASE_MS = 15 * 60 * 1000
PROCESSING_LIMIT = 50
OBSERVATION_TTL_MS = 10000
MESSAGE_SOURCES = ('native', 'web', 'api', 'test')
MATCH_BASES = ('title', 'literal_lyrics', 'semantic')
COMMAND_ACTIONS = ('next', 'pause', 'resume', 'move', 'reorder', 'playback')
WORK_STATES = ('active', 'waiting', 'blocked', 'completed', 'cancelled')
SONG_THEMES = ('record', 'speaker', 'headphones', 'microphone', 'bottles',
               'cocktail', 'compass', 'dandelion', 'embers')


def now_ms():
    return int(time.time() * 1000)


def clean(value, maximum=500):
    if not isinstance(value, str):
        raise ValueError('Expected text.')
    return ''.join(c for c in value if c in '\n\t' or ord(c) >= 32).strip()[:maximum]


def bounded_text(value, field, maximum, *, required=False):
    if not isinstance(value, str) or len(value) > maximum:
        raise ValueError(f'{field} must be text of at most {maximum} characters.')
    result = clean(value, maximum)
    if required and not result:
        raise ValueError(f'{field} must not be empty.')
    return result


def prepare_matches(body, uris):
    """Validate supplied evidence without fetching or independently verifying links."""
    if 'matches' not in body:
        return {}
    matches = body['matches']
    if not isinstance(matches, list) or len(matches) > 8:
        raise ValueError('matches must contain at most eight candidate descriptions.')
    prepared = {}
    for match in matches:
        if (not isinstance(match, dict)
                or set(match) - {'uri', 'basis', 'rationale', 'confidence', 'sources'}):
            raise ValueError('Each match must contain uri, basis, rationale and optional confidence/sources.')
        uri, basis = match.get('uri'), match.get('basis')
        if not isinstance(uri, str) or uri not in uris or uri in prepared:
            raise ValueError('Each match URI must occur once in the selected uris.')
        if basis not in MATCH_BASES:
            raise ValueError('Match basis must be title, literal_lyrics or semantic.')
        evidence = {'basis': basis,
                    'rationale': bounded_text(match.get('rationale'), 'rationale', 500, required=True)}
        if 'confidence' in match:
            confidence = match['confidence']
            if (type(confidence) not in (int, float) or not 0 <= confidence <= 1
                    or not math.isfinite(confidence)):
                raise ValueError('confidence must be a finite number from 0 to 1.')
            evidence['confidence'] = confidence
        sources = match.get('sources', [])
        if not isinstance(sources, list) or len(sources) > 8:
            raise ValueError('sources must contain at most eight HTTP(S) links.')
        evidence['sources'] = []
        for source in sources:
            if not isinstance(source, dict) or set(source) != {'url', 'label'}:
                raise ValueError('Each source must contain url and label.')
            url = bounded_text(source['url'], 'source URL', 2048, required=True)
            parsed = urlsplit(url)
            if (parsed.scheme not in ('http', 'https') or not parsed.hostname
                    or parsed.username is not None or parsed.password is not None
                    or any(c.isspace() or ord(c) < 32 for c in url)):
                raise ValueError('Source links must be HTTP(S) URLs with a host and no credentials.')
            # Accessing port validates a supplied port without opening the link.
            parsed.port
            evidence['sources'].append({'url': url,
                'label': bounded_text(source['label'], 'source label', 120, required=True)})
        prepared[uri] = evidence
    return prepared


def atomic_json(filename, value):
    temporary = filename.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, ensure_ascii=False), encoding='utf-8')
    os.replace(temporary, filename)


class Party:
    def __init__(self, data=DEFAULT_DATA, port=8787, client=None, audio=None):
        self.data = Path(data)
        self.data.mkdir(parents=True, exist_ok=True)
        self.port = port
        self.lock = threading.RLock()
        self.changed = threading.Condition(self.lock)
        self.stop = threading.Event()
        self.client = client or SpotifyClient()
        self.audio = audio
        self.db = sqlite3.connect(self.data / 'party.sqlite3', check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.db.executescript('''
            PRAGMA journal_mode=WAL;
            CREATE TABLE IF NOT EXISTS requests(
              id TEXT PRIMARY KEY, uri TEXT NOT NULL, requested_by TEXT,
              dedication TEXT, track TEXT NOT NULL, status TEXT NOT NULL,
              created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL, error TEXT);
            CREATE TABLE IF NOT EXISTS chat(
              id INTEGER PRIMARY KEY AUTOINCREMENT, request_id TEXT UNIQUE,
              role TEXT NOT NULL, name TEXT NOT NULL, text TEXT NOT NULL,
              created_at INTEGER NOT NULL, read_at INTEGER);
            CREATE TABLE IF NOT EXISTS events(
              id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT NOT NULL,
              payload TEXT NOT NULL, created_at INTEGER NOT NULL);
            CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY, value TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS search_descriptions(
              id TEXT PRIMARY KEY, guest_id TEXT NOT NULL, text TEXT NOT NULL,
              status TEXT NOT NULL, tracks TEXT NOT NULL, note TEXT NOT NULL,
              created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL);
            CREATE TABLE IF NOT EXISTS message_work(
              message_id INTEGER PRIMARY KEY, state TEXT NOT NULL, summary TEXT NOT NULL,
              outcome TEXT NOT NULL, evidence TEXT NOT NULL, updated_at INTEGER NOT NULL,
              active_until INTEGER);
        ''')
        chat_columns = {row[1] for row in self.db.execute('PRAGMA table_info(chat)')}
        if 'guest_id' not in chat_columns:
            self.db.execute("ALTER TABLE chat ADD COLUMN guest_id TEXT NOT NULL DEFAULT ''")
        if 'withdrawn_at' not in chat_columns:
            self.db.execute('ALTER TABLE chat ADD COLUMN withdrawn_at INTEGER')
        if 'accepted_at' not in chat_columns:
            self.db.execute('ALTER TABLE chat ADD COLUMN accepted_at INTEGER')
        if 'origin' not in chat_columns:
            self.db.execute('ALTER TABLE chat ADD COLUMN origin TEXT')
        request_columns = {row[1] for row in self.db.execute('PRAGMA table_info(requests)')}
        if 'origin' not in request_columns:
            self.db.execute('ALTER TABLE requests ADD COLUMN origin TEXT')
        self.db.commit()
        token_file = self.data / 'admin-token'
        if not token_file.exists():
            token_file.write_text(secrets.token_urlsafe(32), encoding='ascii')
            token_file.chmod(0o600)
        self.admin_token = token_file.read_text().strip()
        self.guests = {}
        self.rate = collections.defaultdict(collections.deque)
        self.search_cache = {}
        self.track_cache = {}
        self.playback = {'nowPlaying': None, 'queue': []}
        self._control_generation = 0
        self.spotify = {'connected': False, 'error': 'Connecting to Spotify.'}
        self.revision = 0
        self.last_seen = 0
        self.processing_messages = self.setting('processingMessages', {})
        self.last_track = ''
        self._queue_signature = None
        self.missing_requests = {}
        self.next_dispatch_at = 0
        self.last_auto_caption = 0
        self.last_auto_scene = 0
        self.scene_index = 0
        self.last_book = 0
        self.book_index = -1
        self.book_moment = None
        self.book_enabled = self.setting('bookEnabled', True)
        self.caption = None
        self.caption_queue = collections.deque()
        self.scene = self.setting('scene', {'mode': 'auto', 'energy': .65, 'palette': 'orange', 'effect': 5})
        self.scene.setdefault('audioStyle', 'ring')
        self.scene.setdefault('audioActStartedAt', now_ms())
        if self.scene['audioStyle'] in ('city', 'waterfall'):
            self.scene['effect'] = 0
        self.scene_manual_until = 0
        self.karaoke = Karaoke(self.setting('karaoke'))
        self.mood_votes = {}
        self.captions = self._load_captions()
        self.caption_index = 0
        # A previous process may have stopped after dispatch. Reconcile these
        # with the native queue before considering another dispatch.
        self.db.execute("UPDATE requests SET status='uncertain' WHERE status='sending'")
        self.db.commit()
        self.request_url = self.setting('requestUrl', self._lan_url())
        if self.audio:
            self.audio.set_enabled(self.setting('speechEnabled', False))
        self._write_qr()
        self._migrate_work()
        self._publish()

    def _lan_url(self):
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                s.connect(('1.1.1.1', 80))
                return f'http://{s.getsockname()[0]}:{self.port}'
        except OSError:
            return f'http://{socket.gethostname()}.local:{self.port}'

    def _load_captions(self):
        source = ROOT / 'captions.json'
        return json.loads(source.read_text()) if source.exists() else []

    def _write_qr(self):
        try:
            import qrcode
            target = self.data / 'request-qr.png'
            temporary = target.with_suffix('.tmp')
            qrcode.make(self.request_url, border=3, box_size=8).save(temporary, format='PNG')
            os.replace(temporary, target)
        except ImportError:
            print('QR generation needs qrcode and Pillow; use tools/party_dj/launch.py with the documented dependencies.', flush=True)

    def _book_cards(self):
        source = self.data / 'book/cards.json'
        if not source.exists():
            return []
        raw = json.loads(source.read_text())
        return raw.get('cards', []) if isinstance(raw, dict) else raw

    def _show_book(self, card, seconds=35):
        if not self.book_enabled:
            return False
        candidate = card.get('imagePath') or card.get('path') or card.get('image')
        if not candidate:
            return False
        image = Path(candidate)
        if not image.is_absolute():
            image = self.data / 'book' / candidate
        if not image.is_file():
            return False
        stamp = now_ms()
        self.book_moment = {**card, 'imagePath': str(image),
            'imageUrl': '/book/' + image.name, 'createdAt': stamp,
            'expiresAt': stamp + max(10, min(90, float(seconds))) * 1000}
        self.last_book = stamp
        return True

    def setting(self, key, default=None):
        row = self.db.execute('SELECT value FROM settings WHERE key=?', (key,)).fetchone()
        return json.loads(row[0]) if row else default

    def set_setting(self, key, value):
        self.db.execute('INSERT OR REPLACE INTO settings VALUES(?,?)', (key, json.dumps(value)))
        self.db.commit()

    def _event(self, kind, payload):
        self.db.execute('INSERT INTO events(kind,payload,created_at) VALUES(?,?,?)',
                        (kind, json.dumps(payload, ensure_ascii=False), now_ms()))
        self.db.commit()
        self.changed.notify_all()

    def _request_dict(self, row):
        track = json.loads(row['track'])
        return {**track, 'id': row['id'], 'requestedBy': row['requested_by'],
                'dedication': row['dedication'], 'status': row['status'],
                'createdAt': row['created_at'], 'error': row['error']}

    def _origin(self, body, context=None):
        """HTTP context is supplied by Handler; all labels remain caller declarations."""
        declared = 'source' in body
        source = body.get('source', 'unknown')
        if declared and source not in MESSAGE_SOURCES:
            raise ValueError('source must be native, web, api or test.')
        context = context or {}
        return {'guestId': clean(body.get('guestId', ''), 100) or None,
                'source': source, 'sourceDeclared': declared,
                'operatorLabel': bounded_text(context.get('operatorLabel', ''), 'operator label', 120) or None,
                'httpPeer': dict(context['httpPeer']) if context.get('httpPeer') else None}

    def _stored_origin(self, row):
        if row['origin']:
            return json.loads(row['origin'])
        return {'guestId': (row['guest_id'] or None) if 'guest_id' in row.keys() else None,
                'source': 'unknown', 'sourceDeclared': False, 'operatorLabel': None, 'httpPeer': None}

    def _chat_dict(self, row):
        origin = self._stored_origin(row)
        return {'id': row['id'], 'role': row['role'], 'name': row['name'],
                'text': row['text'], 'createdAt': row['created_at'],
                'acceptedAt': row['accepted_at'], 'readAt': row['read_at'],
                'source': origin['source'], 'sourceDeclared': origin['sourceDeclared'],
                'isTest': row['role'] == 'test',
                'work': self._message_work(row['id']),
                'status': 'withdrawn' if row['withdrawn_at'] else 'read' if row['read_at'] else 'delivered'}

    def message_info(self, identifier):
        if type(identifier) is not int or not 0 < identifier <= 2**53 - 1:
            raise ValueError('Choose a positive message ID.')
        with self.lock:
            row = self.db.execute('SELECT * FROM chat WHERE id=?', (identifier,)).fetchone()
            if not row:
                raise ValueError('Unknown message ID.')
            return {'message': self._chat_dict(row), 'attribution': self._stored_origin(row),
                    'requestId': row['request_id'], 'withdrawnAt': row['withdrawn_at']}

    def _withdrawn_ids(self):
        return [row[0] for row in self.db.execute(
            'SELECT id FROM chat WHERE withdrawn_at IS NOT NULL ORDER BY id DESC LIMIT 500')]

    def _dj(self):
        stamp = now_ms()
        age = stamp - self.last_seen
        return {'status': 'listening' if age < 90000 else 'away', 'lastSeen': self.last_seen,
                'processingMessageIds': sorted(int(key) for key in self._active_processing(stamp)),
                'messageWork': self._work_map(stamp)}

    def _work_dict(self, row, stamp=None):
        stamp = now_ms() if stamp is None else stamp
        effective = row['state']
        if effective == 'active' and (row['active_until'] is None or stamp >= row['active_until']):
            effective = 'unconfirmed'
        return {'state': row['state'], 'effectiveState': effective, 'summary': row['summary'],
                'outcome': row['outcome'], 'evidence': json.loads(row['evidence']),
                'updatedAt': row['updated_at'], 'activeUntil': row['active_until']}

    def _message_work(self, identifier):
        row = self.db.execute('SELECT * FROM message_work WHERE message_id=?', (identifier,)).fetchone()
        return self._work_dict(row) if row else None

    def _work_map(self, stamp=None):
        return {str(row['message_id']): self._work_dict(row, stamp) for row in self.db.execute(
            "SELECT w.* FROM message_work w JOIN chat c ON c.id=w.message_id WHERE c.role='user' AND c.withdrawn_at IS NULL")}

    def _sync_processing_work(self, stamp):
        self.processing_messages = {str(row[0]): row[1] for row in self.db.execute(
            "SELECT message_id,active_until FROM message_work WHERE state='active' AND active_until>?", (stamp,))}
        self.db.execute('INSERT OR REPLACE INTO settings VALUES(?,?)',
                        ('processingMessages', json.dumps(self.processing_messages)))

    def _migrate_work(self):
        with self.lock:
            stamp = now_ms()
            changed = []
            if not self.setting('messageWorkMigrated', False):
                legacy = self.processing_messages if isinstance(self.processing_messages, dict) else {}
                for key in legacy:
                    if not isinstance(key, str) or not key.isdecimal() or not 0 < int(key) <= 2**53 - 1:
                        continue
                    identifier = int(key)
                    if not self.db.execute("SELECT 1 FROM chat WHERE id=? AND role='user' AND withdrawn_at IS NULL", (identifier,)).fetchone():
                        continue
                    cursor = self.db.execute('INSERT OR IGNORE INTO message_work VALUES(?,?,?,?,?,?,?)',
                        (identifier, 'waiting', 'Raniji rad čeka zabilježen ishod', '', '[]', stamp, None))
                    if cursor.rowcount:
                        changed.append(identifier)
                self.db.execute('INSERT OR REPLACE INTO settings VALUES(?,?)', ('messageWorkMigrated', 'true'))
            self._sync_processing_work(stamp)
            self.db.commit()
            if changed:
                self._event('work_changed', {'messageIds': changed,
                    'messageWork': {str(identifier): self._message_work(identifier) for identifier in changed}})

    def _guest_message_ids(self, identifiers):
        if not isinstance(identifiers, list) or not 1 <= len(identifiers) <= PROCESSING_LIMIT:
            raise ValueError(f'messageIds must contain 1 to {PROCESSING_LIMIT} guest message IDs.')
        if any(type(value) is not int or not 0 < value <= 2**53 - 1 for value in identifiers):
            raise ValueError('messageIds must contain positive integer guest message IDs.')
        identifiers = sorted(set(identifiers))
        placeholders = ','.join('?' for unused in identifiers)
        found = {row[0] for row in self.db.execute(
            f"SELECT id FROM chat WHERE role='user' AND withdrawn_at IS NULL AND id IN ({placeholders})", identifiers)}
        if found != set(identifiers):
            raise ValueError('Choose existing guest message IDs; replies, tests and withdrawn messages cannot accept work.')
        return identifiers

    def _prepare_work(self, body):
        identifiers = self._guest_message_ids(body.get('messageIds'))
        state = body.get('state')
        if state not in WORK_STATES:
            raise ValueError('Work state must be active, waiting, blocked, completed or cancelled.')
        summary = bounded_text(body.get('summary', ''), 'summary', 500)
        outcome = bounded_text(body.get('outcome', ''), 'outcome', 1000)
        if state in ('active', 'waiting', 'blocked') and (len(summary) < 3 or not any(c.isalnum() for c in summary)):
            raise ValueError('Active, waiting and blocked work need a useful summary.')
        evidence = body.get('evidence', [])
        if not isinstance(evidence, list) or len(evidence) > 8:
            raise ValueError('evidence must contain at most eight text entries.')
        evidence = [bounded_text(value, 'evidence entry', 500, required=True) for value in evidence]
        if state == 'completed' and (not outcome or not evidence):
            raise ValueError('Completed work needs a nonempty outcome and evidence.')
        if state == 'cancelled' and not outcome:
            raise ValueError('Cancelled work needs an outcome explaining the cancellation.')
        lease = body.get('leaseSeconds', 300)
        if type(lease) is not int or not 30 <= lease <= 900:
            raise ValueError('leaseSeconds must be an integer from 30 to 900.')
        stamp = now_ms()
        if state == 'active' and len(set(self._active_processing(stamp)) | {str(value) for value in identifiers}) > PROCESSING_LIMIT:
            raise ValueError(f'At most {PROCESSING_LIMIT} guest messages can be active at once.')
        return {'messageIds': identifiers, 'state': state, 'summary': summary, 'outcome': outcome,
                'evidence': evidence, 'updatedAt': stamp,
                'activeUntil': stamp + lease * 1000 if state == 'active' else None}

    def _save_work(self, identifiers, work, *, accept=True):
        for identifier in identifiers:
            self.db.execute('''INSERT INTO message_work VALUES(?,?,?,?,?,?,?)
                ON CONFLICT(message_id) DO UPDATE SET state=excluded.state,summary=excluded.summary,
                outcome=excluded.outcome,evidence=excluded.evidence,updated_at=excluded.updated_at,
                active_until=excluded.active_until''',
                (identifier, work['state'], work['summary'], work['outcome'], json.dumps(work['evidence'], ensure_ascii=False),
                 work['updatedAt'], work['activeUntil']))
            if accept:
                self.db.execute('UPDATE chat SET accepted_at=COALESCE(accepted_at,?) WHERE id=?', (work['updatedAt'], identifier))

    def _work_changed(self, identifiers):
        self._sync_processing_work(now_ms())
        self._event('work_changed', {'messageIds': identifiers,
            'messageWork': {str(identifier): self._message_work(identifier) for identifier in identifiers}})

    def _legacy_processing_work(self, body):
        stamp = now_ms()
        changed = []
        for identifier in sorted(set(body['messageIds'])):
            previous = self._message_work(identifier)
            if body['active']:
                work = {'state': 'active', 'summary': (previous or {}).get('summary') or 'DJ obrađuje poruku; ishod još nije zabilježen.',
                        'outcome': (previous or {}).get('outcome', ''), 'evidence': (previous or {}).get('evidence', []),
                        'updatedAt': stamp, 'activeUntil': stamp + PROCESSING_LEASE_MS}
            elif previous and previous['state'] == 'active':
                work = {**previous, 'state': 'waiting', 'outcome': 'Ishod rada još nije potvrđen.',
                        'updatedAt': stamp, 'activeUntil': None}
            else:
                continue
            self._save_work([identifier], work)
            changed.append(identifier)
        if changed:
            self._work_changed(changed)
        else:
            self._sync_processing_work(stamp)
            self.db.commit()

    def _active_processing(self, stamp):
        return {key: expires for key, expires in self.processing_messages.items() if expires > stamp}

    def _prepare_processing(self, body):
        """Validate the complete batch before any admin side effect."""
        identifiers = self._guest_message_ids(body.get('messageIds'))
        active = body.get('active')
        if type(active) is not bool:
            raise ValueError('active must be true or false.')
        if active and any((self._message_work(identifier) or {}).get('state') in ('completed', 'cancelled') for identifier in identifiers):
            raise ValueError('Terminal work can be reopened only through the explicit work action.')
        stamp = now_ms()
        updated = self._active_processing(stamp)
        for identifier in identifiers:
            if active:
                updated[str(identifier)] = stamp + PROCESSING_LEASE_MS
            else:
                updated.pop(str(identifier), None)
        if len(updated) > PROCESSING_LIMIT:
            raise ValueError(f'At most {PROCESSING_LIMIT} guest messages can be processing at once.')
        return updated

    def _mood(self):
        active = [m for m, stamp in self.mood_votes.values() if now_ms() - stamp < 20 * 60000]
        counts = collections.Counter(active)
        choice = counts.most_common(1)[0][0] if counts else 'dance'
        energy = sum(MOODS[m][1] for m in active) / len(active) if active else self.scene['energy']
        return {'label': MOODS[choice][0], 'choice': choice, 'energy': round(energy, 3),
                'votes': dict(counts), 'count': len(active)}

    def snapshot(self):
        with self.lock:
            rows = self.db.execute("SELECT * FROM requests WHERE status NOT IN ('played','removed') ORDER BY created_at LIMIT 100").fetchall()
            requests = [self._request_dict(r) for r in rows]
            own = {item['uri']: item for item in requests}
            own_by_uid = {item['uid']: item for item in requests if item.get('uid')}
            queue = []
            seen = set()
            for item in self.playback.get('queue', []):
                merged = {**item, 'status': 'queued'}
                request = own_by_uid.get(item.get('uid')) or own.get(item.get('uri'))
                if request and request.get('uid') and item.get('uid') and request['uid'] != item['uid']:
                    request = None
                if request:
                    merged.update({k: v for k, v in request.items() if k in ('id', 'requestedBy', 'dedication', 'status')})
                    seen.add(request['id'])
                queue.append(merged)
            queue.extend(self._request_dict(r) for r in rows if r['id'] not in seen and r['status'] != 'playing')
            now_playing = self.playback.get('nowPlaying')
            playing = next((r for r in rows if r['status'] == 'playing'), None)
            if now_playing and playing:
                now_playing = {**now_playing, 'requestedUri': playing['uri'],
                    'requestId': playing['id'], 'requestedBy': playing['requested_by'],
                    'dedication': playing['dedication']}
            spotify = self.spotify.copy()
            observed = spotify.get('observedAt')
            valid_stamp = type(observed) in (int, float) and math.isfinite(observed)
            spotify['ageMs'] = max(0, now_ms() - observed) if valid_stamp else None
            spotify['stale'] = (not spotify.get('connected') or not valid_stamp
                                or observed > now_ms() + 1000 or spotify['ageMs'] > OBSERVATION_TTL_MS)
            result = {'revision': self.revision, 'nowPlaying': now_playing,
                'queue': queue, 'scene': self.scene.copy(), 'caption': self.caption,
                'mood': self._mood(), 'guestCount': sum(now_ms() - t < 90000 for t in self.guests.values()),
                'requestUrl': self.request_url, 'qrPath': str(self.data / 'request-qr.png'),
                'spotify': spotify, 'playbackState': self.playback.get('playbackState'), 'dj': self._dj(),
                'withdrawnMessageIds': self._withdrawn_ids(),
                'bookEnabled': self.book_enabled, 'bookMoment': self.book_moment,
                'speech': self.audio.snapshot() if self.audio else {'available': False, 'enabled': False},
                'karaoke': self.karaoke.snapshot(now_playing, self.spotify['connected'], now_ms()),
                'title': 'LARICS / LYRICS'}
            return result

    def _publish(self):
        self.revision += 1
        atomic_json(self.data / 'state.json', self.snapshot())

    def visit(self, guest):
        if guest:
            with self.lock:
                self.guests[clean(guest, 100)] = now_ms()
                if len(self.guests) > 500:
                    self.guests = {k: v for k, v in self.guests.items() if now_ms() - v < 3600000}

    def limit(self, identity, action, count, seconds):
        with self.lock:
            bucket = self.rate[(identity, action)]
            cutoff = time.monotonic() - seconds
            while bucket and bucket[0] < cutoff:
                bucket.popleft()
            if len(bucket) >= count:
                raise ValueError('A little breathing room, please. Try again in a moment.')
            bucket.append(time.monotonic())

    def search(self, query):
        query = clean(query, 180)
        if len(query) < 2:
            return {'tracks': []}
        key = query.casefold()
        with self.lock:
            cached = self.search_cache.get(key)
            if cached and time.monotonic() - cached[0] < 45:
                return {'tracks': cached[1]}
        tracks = self.client.search(query)
        with self.lock:
            for track in tracks:
                self.track_cache[track['uri']] = track
            self.search_cache[key] = (time.monotonic(), tracks)
            if len(self.search_cache) > 100:
                self.search_cache.pop(next(iter(self.search_cache)))
        return {'tracks': tracks}

    def lookup_track(self, uri):
        if not isinstance(uri, str) or not TRACK.fullmatch(uri):
            raise ValueError('Choose an exact Spotify track URI.')
        with self.lock:
            track = self.track_cache.get(uri)
        if track is None:
            track = self.client.track(uri)
            with self.lock:
                self.track_cache[uri] = track
        return {'track': track}

    def request(self, body, *, context=None):
        origin = self._origin(body, context)
        if origin['source'] == 'test':
            raise ValueError('Test submissions are messages; use an isolated fixture for music requests.')
        rid = clean(body.get('requestId', ''), 100)
        uri = clean(body.get('uri', ''), 100)
        if len(rid) < 8 or not TRACK.fullmatch(uri):
            raise ValueError('Choose a song from the search results.')
        name = clean(body.get('requestedBy', ''), 60) or 'Gost'
        dedication = clean(body.get('dedication', ''), 280)
        with self.lock:
            row = self.db.execute('SELECT * FROM requests WHERE id=?', (rid,)).fetchone()
            if row:
                if row['uri'] != uri:
                    raise ValueError('This request ID belongs to a different song.')
                return {'request': self._request_dict(row), 'status': row['status']}
            row = self.db.execute("SELECT * FROM requests WHERE uri=? AND status IN ('pending','sending','uncertain','queued','playing') ORDER BY created_at DESC LIMIT 1", (uri,)).fetchone()
            if row:
                return {'request': self._request_dict(row), 'status': row['status'], 'alreadyQueued': True}
            track = self.track_cache.get(uri)
        if track is None:
            track = self.client.track(uri)
        with self.lock:
            # Recheck after the metadata fetch, which runs outside the database lock.
            row = self.db.execute('SELECT * FROM requests WHERE id=?', (rid,)).fetchone()
            if row:
                if row['uri'] != uri:
                    raise ValueError('This request ID belongs to a different song.')
                return {'request': self._request_dict(row), 'status': row['status']}
            row = self.db.execute("SELECT * FROM requests WHERE uri=? AND status IN ('pending','sending','uncertain','queued','playing') LIMIT 1", (uri,)).fetchone()
            if row:
                return {'request': self._request_dict(row), 'status': row['status'], 'alreadyQueued': True}
            stamp = now_ms()
            self.db.execute('INSERT INTO requests(id,uri,requested_by,dedication,track,status,created_at,updated_at,error,origin) VALUES(?,?,?,?,?,?,?,?,?,?)',
                (rid, uri, name, dedication, json.dumps(track, ensure_ascii=False), 'pending', stamp, stamp, None,
                 json.dumps(origin, ensure_ascii=False)))
            self._event('request', {**track, 'id': rid, 'requestedBy': name, 'dedication': dedication})
            self._publish()
            return {'request': self._request_dict(self.db.execute('SELECT * FROM requests WHERE id=?', (rid,)).fetchone()), 'status': 'pending'}

    def chat(self, after=0):
        with self.lock:
            if after:
                rows = self.db.execute('SELECT * FROM chat WHERE id>? AND withdrawn_at IS NULL ORDER BY id LIMIT 100', (after,)).fetchall()
            else:
                rows = self.db.execute('SELECT * FROM (SELECT * FROM chat WHERE withdrawn_at IS NULL ORDER BY id DESC LIMIT 80) ORDER BY id').fetchall()
            return {'messages': [self._chat_dict(r) for r in rows], 'dj': self._dj(),
                    'withdrawnMessageIds': self._withdrawn_ids()}

    def message(self, body, role='user', *, context=None):
        origin = self._origin(body, context)
        text = clean(body.get('text', ''), 1501)
        if len(text) > 1500:
            raise ValueError('A message can contain at most 1500 characters.')
        if not text:
            raise ValueError('Write a message first.')
        rid = clean(body.get('requestId', '') or secrets.token_hex(16), 100)
        name = 'DJ' if role == 'assistant' else clean(body.get('name', ''), 60) or 'Gost'
        if origin['source'] == 'test':
            role, name, text = 'test', 'TEST / ' + name, '[TEST] ' + text
        with self.lock:
            row = self.db.execute('SELECT * FROM chat WHERE request_id=?', (rid,)).fetchone()
            if row:
                previous = self._stored_origin(row)
                if (row['role'] != role or row['text'] != text
                        or previous['guestId'] != origin['guestId']
                        or previous['source'] != origin['source']):
                    raise ValueError('This message ID was already used.')
                return {'message': self._chat_dict(row)}
            self.db.execute('INSERT INTO chat(request_id,role,name,text,created_at,guest_id,origin) VALUES(?,?,?,?,?,?,?)',
                (rid, role, name, text, now_ms(), origin['guestId'] or '', json.dumps(origin, ensure_ascii=False)))
            row = self.db.execute('SELECT * FROM chat WHERE request_id=?', (rid,)).fetchone()
            item = self._chat_dict(row)
            self._event('test_message' if role == 'test' else 'chat' if role == 'user' else 'reply', item)
            self._publish()
            return {'message': item}

    def recall_message(self, body):
        guest = clean(body.get('guestId', ''), 100)
        identifier = body.get('messageId')
        if len(guest) < 8 or type(identifier) is not int or identifier < 1:
            raise ValueError('Choose your last sent message to recall.')
        with self.lock:
            row = self.db.execute("SELECT * FROM chat WHERE id=? AND guest_id=? AND role='user'",
                                  (identifier, guest)).fetchone()
            if not row:
                raise ValueError('That message does not belong to this guest.')
            result = {'recalled': {'id': row['id'], 'text': row['text'], 'requestId': row['request_id']}}
            if row['withdrawn_at']:
                return result
            latest = self.db.execute("SELECT MAX(id) FROM chat WHERE guest_id=? AND role='user' AND withdrawn_at IS NULL",
                                     (guest,)).fetchone()[0]
            if identifier != latest:
                raise ValueError('Only your latest message can be recalled.')
            if (row['accepted_at'] is not None or self._message_work(identifier) is not None
                    or str(identifier) in self._active_processing(now_ms())):
                raise ValueError('The DJ has already accepted this message for processing.')
            self.db.execute('UPDATE chat SET withdrawn_at=? WHERE id=?', (now_ms(), identifier))
            self._event('message_recalled', {'messageId': identifier})
            self._publish()
            return result

    def _description_dict(self, row):
        return {'id': row['id'], 'text': row['text'], 'status': row['status'],
                'tracks': json.loads(row['tracks']), 'note': row['note']}

    def describe_search(self, body):
        identifier = clean(body.get('requestId', ''), 100)
        guest = clean(body.get('guestId', ''), 100)
        text = clean(body.get('text', ''), 1501)
        if len(identifier) < 8 or len(guest) < 8 or not 3 <= len(text) <= 1500:
            raise ValueError('Describe the song in 3 to 1500 characters.')
        with self.lock:
            row = self.db.execute('SELECT * FROM search_descriptions WHERE id=?', (identifier,)).fetchone()
            if row:
                if row['guest_id'] != guest or row['text'] != text:
                    raise ValueError('This search ID belongs to another description.')
                return {'search': self._description_dict(row)}
            stamp = now_ms()
            self.db.execute('INSERT INTO search_descriptions VALUES(?,?,?,?,?,?,?,?)',
                            (identifier, guest, text, 'pending', '[]', '', stamp, stamp))
            self._event('search_description', {'id': identifier, 'text': text})
            return self.description_search(identifier)

    def description_search(self, identifier):
        with self.lock:
            row = self.db.execute('SELECT * FROM search_descriptions WHERE id=?', (clean(identifier, 100),)).fetchone()
            if not row:
                raise ValueError('Unknown song description.')
            return {'search': self._description_dict(row)}

    def _observation_failed(self, error):
        message = str(error)[:300]
        changed = self.spotify['connected'] or self.spotify.get('error') != message
        self.spotify = {'connected': False, 'error': message}
        self.playback = {'nowPlaying': None, 'queue': []}
        if changed:
            self._event('spotify', self.spotify)

    def synchronize(self):
        """Read the actual player now; no transport, queue or reconnect command."""
        with self.lock:
            self._control_generation += 1
            try:
                observed = self.client.status()
                self._apply_observation(observed)
            except Exception as error:
                self._observation_failed(error)
                self._publish()
                raise SpotifyError(str(error), dispatched=False) from error
            self._publish()
            recent = [dict(json.loads(row['payload']), eventId=row['id'], seenAt=row['created_at'])
                      for row in self.db.execute("SELECT id,payload,created_at FROM events WHERE kind='track' ORDER BY id DESC LIMIT 200")]
            return {'state': self.snapshot(), 'observation': self.playback,
                    'recentTracks': recent, 'lastId': self.db.execute('SELECT COALESCE(MAX(id),0) FROM events').fetchone()[0]}

    def mood(self, body):
        choice = body.get('mood')
        if choice not in MOODS:
            raise ValueError('Choose one of the mood buttons.')
        guest = clean(body.get('guestId', ''), 100)
        if not guest:
            raise ValueError('A guest ID is required.')
        with self.lock:
            previous = self.mood_votes.get(guest)
            self.mood_votes[guest] = (choice, now_ms())
            if previous is None or previous[0] != choice:
                self._event('mood', {'guestId': guest, 'choice': choice, 'mood': self._mood()})
            if now_ms() > self.scene_manual_until:
                self.scene['energy'] = self._mood()['energy']
            self._publish()
            return {'mood': self._mood()}

    def cue(self, text, seconds=None):
        text = clean(text, 400)
        if not text:
            return
        seconds = max(len(text) / 17 + 1, float(seconds or 0), 6)
        cue = {'id': secrets.token_hex(6), 'text': text, 'seconds': min(seconds, 45)}
        self.caption_queue.append(cue)

    def _advance_captions(self):
        stamp = now_ms()
        current_done = not self.caption or stamp > self.caption['createdAt'] + self.caption['seconds'] * 1000
        if current_done and self.caption_queue:
            self.caption = {**self.caption_queue.popleft(), 'createdAt': stamp}
        elif current_done and self.caption:
            self.caption = None
        if stamp - self.last_auto_caption > 55000 and not self.caption_queue and self.captions:
            item = self.captions[self.caption_index % len(self.captions)]
            self.caption_index += 1
            self.last_auto_caption = stamp
            self.cue(item['text'] if isinstance(item, dict) else item)

    def _advance_show(self):
        stamp = now_ms()
        if self.scene['audioStyle'] in ('city', 'waterfall'):
            self.scene['effect'] = 0
            if stamp - self.scene['audioActStartedAt'] >= AUDIO_ACT_MS:
                self.scene['audioStyle'] = 'waterfall' if self.scene['audioStyle'] == 'city' else 'city'
                self.scene['audioActStartedAt'] = stamp
                self.set_setting('scene', self.scene)
                self._event('audio_act', {'style': self.scene['audioStyle'], 'startedAt': stamp})
        elif self.scene.get('mode') == 'auto' and stamp > self.scene_manual_until and stamp - self.last_auto_scene > 75000:
            effects = [1, 6, 3, 0, 7, 2, 5, 4]
            self.scene['effect'] = effects[self.scene_index % len(effects)]
            self.scene_index += 1
            self.last_auto_scene = stamp
        if self.book_enabled and stamp - self.last_book > 105000:
            try:
                cards = self._book_cards()
                if cards:
                    photos = [c for c in cards if c.get('kind') == 'photo']
                    pages = [c for c in cards if c.get('kind') != 'photo']
                    self.book_index += 1
                    # Three photographs, then one source page. Each collection
                    # advances independently so a long book never buries its photos.
                    page_turn = self.book_index % 4 == 3 and pages
                    pool = pages if page_turn else (photos or pages)
                    index = self.book_index // 4 if page_turn else self.book_index - self.book_index // 4
                    self._show_book(pool[index % len(pool)])
                self.last_book = stamp
            except (ValueError, OSError, TypeError):
                self.last_book = stamp - 90000
        if self.book_moment and stamp > self.book_moment.get('expiresAt', 0):
            self.book_moment = None

    def _command_observation(self):
        current = self.playback.get('nowPlaying') or {}
        playback = self.playback.get('playbackState') or {}
        device = playback.get('device') or {}
        return {'currentUid': current.get('uid'), 'currentUri': current.get('uri'),
                'deviceId': device.get('id'), 'observedAt': self.spotify.get('observedAt'),
                'connected': self.spotify.get('connected') is True,
                'paused': current.get('paused'), 'buffering': current.get('buffering')}

    def _new_command(self, action, body, origin):
        target = {}
        for key in ('uid', 'beforeUid', 'expectedCurrentUid', 'expectedDeviceId', 'requestId', 'uri'):
            if isinstance(body.get(key), str):
                target[key] = body[key][:512]
        for key in ('orderedQueuedUids', 'expectedQueuedUids'):
            if isinstance(body.get(key), list):
                target[key] = [value[:512] if isinstance(value, str) else None for value in body[key][:500]]
        if type(body.get('paused')) is bool:
            target['paused'] = body['paused']
        return {'started': False, 'record': {'commandId': secrets.token_hex(16), 'action': action,
            'attribution': origin, 'target': target, 'before': self._command_observation()}}

    def _command_event(self, command, phase, *, result=None, error=None):
        if phase == 'rejected':
            dispatched = False
        elif error is not None:
            value = error.dispatched if isinstance(error, SpotifyError) else None
            dispatched = value if type(value) is bool else None
        elif isinstance(result, dict):
            value = result.get('dispatched')
            if type(value) is bool:
                dispatched = value
            elif any(result.get(key) is True for key in ('changed', 'moved', 'queued')):
                dispatched = True
            elif any(result.get(key) is False for key in ('changed', 'moved')):
                dispatched = False
            else:
                dispatched = None
        else:
            dispatched = None
        record = {**command['record'], 'phase': phase, 'dispatched': dispatched,
                  'dispatchUncertain': dispatched is None or (phase == 'error' and dispatched is True),
                  'current': self._command_observation()}
        if error is not None:
            record.update(error=str(error)[:500], errorType=type(error).__name__)
        if isinstance(result, dict):
            record['returnedStatus'] = isinstance(result.get('status'), dict)
            record['result'] = {key: result[key] for key in ('ok', 'action', 'queued', 'moved', 'changed', 'paused', 'uid', 'uri', 'requestId')
                                if key in result and type(result[key]) in (str, bool, int, float, type(None))}
        self._event('command', record)

    def _start_command(self, command):
        self._command_event(command, 'dispatch')
        command['started'] = True

    def _verified_playback_status(self, result, paused, current_uid, device_id):
        status = result.get('status') if isinstance(result, dict) else None
        valid = isinstance(status, dict)
        current = status.get('nowPlaying') if valid else None
        playback = status.get('playbackState') if valid else None
        queue = status.get('queue') if valid else None
        device = playback.get('device') if isinstance(playback, dict) else None
        stamp = now_ms()
        def fresh(value):
            return (type(value) in (int, float) and stamp - OBSERVATION_TTL_MS <= value <= stamp + 1000
                    and math.isfinite(value))
        valid = (valid and isinstance(current, dict) and isinstance(playback, dict) and isinstance(device, dict)
                 and isinstance(queue, list) and all(isinstance(item, dict) and
                     all(item.get(key) is None or isinstance(item.get(key), str) for key in ('uid', 'uri', 'source'))
                     for item in queue)
                 and current.get('uid') == current_uid and device.get('id') == device_id
                 and isinstance(current.get('uri'), str) and bool(current['uri'].strip())
                 and current.get('paused') is paused and playback.get('paused') is paused
                 and fresh(status.get('observedAt')) and fresh(current.get('observedAt')))
        if not valid:
            raise SpotifyError('The playback command did not return a fresh verified target observation.', dispatched=True)
        return status

    def admin(self, action, body, *, context=None):
        origin = self._origin(body, context) if action in COMMAND_ACTIONS else None
        with self.lock:
            command = self._new_command(action, body, origin) if action in COMMAND_ACTIONS else None
            try:
                result = self._admin(action, body, context=context, command=command)
            except Exception as error:
                if command:
                    self._command_event(command, 'error' if command['started'] else 'rejected', error=error)
                raise
            if command and command['started']:
                self._command_event(command, 'returned', result=result)
            return result

    def _admin(self, action, body, *, context=None, command=None):
        with self.lock:
            processing = self._prepare_processing(body) if action == 'processing' else None
            work = self._prepare_work(body) if action == 'work' else None
            if action == 'configure' and 'bookEnabled' in body and not isinstance(body['bookEnabled'], bool):
                raise ValueError('bookEnabled must be true or false.')
            if action == 'move':
                uid, before_uid = body.get('uid'), body.get('beforeUid')
                expected = body.get('expectedQueuedUids')
                if not all(isinstance(value, str) and value for value in (uid, before_uid)) or uid == before_uid:
                    raise ValueError('Choose two distinct queue occurrence UIDs for the move.')
                if (not isinstance(expected, list) or len(expected) < 2
                        or not all(isinstance(value, str) and value for value in expected)
                        or len(set(expected)) != len(expected)
                        or uid not in expected or before_uid not in expected):
                    raise ValueError('Supply the complete unique explicit queue UIDs, including the moved occurrence and anchor.')
            if action == 'reorder':
                ordered, expected = body.get('orderedQueuedUids'), body.get('expectedQueuedUids')
                current_uid, device_id = body.get('expectedCurrentUid'), body.get('expectedDeviceId')
                if (not all(isinstance(value, list) for value in (ordered, expected))
                        or any(not isinstance(uid, str) or not uid or len(uid) > 512 for values in (ordered, expected) for uid in values)
                        or len(set(ordered)) != len(ordered) or len(set(expected)) != len(expected)
                        or set(ordered) != set(expected)
                        or not all(isinstance(value, str) and 0 < len(value) <= 512 for value in (current_uid, device_id))):
                    raise ValueError('Supply a unique permutation of the complete explicit queue and the expected current occurrence/device.')
            if action == 'playback':
                paused = body.get('paused')
                current_uid, device_id = body.get('expectedCurrentUid'), body.get('expectedDeviceId')
                if type(paused) is not bool or not all(isinstance(value, str) and value.strip() and len(value) <= 512
                                                       for value in (current_uid, device_id)):
                    raise ValueError('Supply paused as a boolean and the expected current occurrence/device IDs.')
            if action == 'scene' and 'songVisual' in body:
                visual = body['songVisual']
                if visual is not None and (not isinstance(visual, dict) or set(visual) != {'trackUid', 'theme'}
                        or not isinstance(visual.get('trackUid'), str) or not 0 < len(visual['trackUid']) <= 512
                        or visual.get('theme') not in SONG_THEMES):
                    raise ValueError('songVisual must be null or {trackUid, theme} with a supported motif.')
            if action == 'resolve-search':
                search_id = clean(body.get('id', ''), 100)
                uris = body.get('uris')
                note = clean(body.get('note', ''), 500)
                if (not isinstance(uris, list) or len(uris) > 8
                        or any(not isinstance(uri, str) or not TRACK.fullmatch(uri) for uri in uris)
                        or len(set(uris)) != len(uris) or (not uris and not note)):
                    raise ValueError('Resolve a description with up to eight exact Spotify tracks or a clarification note.')
                if not self.db.execute('SELECT 1 FROM search_descriptions WHERE id=?', (search_id,)).fetchone():
                    raise ValueError('Unknown song description.')
                matches = prepare_matches(body, uris)
            read_through = body.get('readThrough')
            if read_through is not None:
                read_through = int(read_through)
            self.last_seen = now_ms()
            if read_through is not None:
                self.db.execute('UPDATE chat SET read_at=COALESCE(read_at,?) WHERE role=? AND id<=? AND withdrawn_at IS NULL', (now_ms(), 'user', read_through))
                self.db.commit()
            if action == 'heartbeat':
                result = {'dj': self._dj()}
            elif action == 'sync':
                return self.synchronize()
            elif action == 'resolve-search':
                tracks = [{**(self.track_cache.get(uri) or self.client.track(uri)),
                           **({'match': matches[uri]} if uri in matches else {})}
                          for uri in uris]
                self.db.execute("UPDATE search_descriptions SET status='resolved',tracks=?,note=?,updated_at=? WHERE id=?",
                                (json.dumps(tracks, ensure_ascii=False), note, now_ms(), search_id))
                self._event('search_resolved', {'id': search_id, 'uris': uris})
                result = self.description_search(search_id)
            elif action == 'processing':
                self._legacy_processing_work(body)
                result = {'dj': self._dj()}
            elif action == 'work':
                self._save_work(work['messageIds'], work)
                self._work_changed(work['messageIds'])
                result = {'dj': self._dj()}
            elif action == 'reply':
                result = self.message(body, 'assistant', context=context)
                if body.get('caption'):
                    self.cue(body['text'], body.get('seconds'))
            elif action == 'cue':
                self.cue(body.get('text', ''), body.get('seconds'))
                result = {'queued': True}
            elif action == 'speak':
                text = clean(body.get('text', ''), 170)
                rid = clean(body.get('requestId', ''), 100)
                result = self.audio.submit(rid, text) if self.audio else {'queued': False, 'error': 'Speech is unavailable.'}
                if body.get('caption', True):
                    self.cue(text)
            elif action == 'scene':
                mode = body.get('mode', self.scene['mode'])
                if mode not in ('auto', 'orbit', 'laser', 'robots', 'quiet'):
                    raise ValueError('Unknown scene.')
                style = body.get('audioStyle', self.scene['audioStyle'])
                if style not in AUDIO_STYLES:
                    raise ValueError('Choose ring, city or waterfall for audioStyle.')
                self.scene.update({'mode': mode, 'energy': max(0, min(1, float(body.get('energy', self.scene['energy'])))),
                    'palette': clean(body.get('palette', self.scene['palette']), 30)})
                if 'effect' in body:
                    self.scene['effect'] = int(body['effect'])
                if 'graphicsScale' in body:
                    self.scene['graphicsScale'] = max(.75, min(2, float(body['graphicsScale'])))
                if 'songVisual' in body:
                    self.scene['songVisual'] = body['songVisual']
                if style != self.scene['audioStyle']:
                    self.scene['audioActStartedAt'] = now_ms()
                self.scene['audioStyle'] = style
                if style in ('city', 'waterfall'):
                    self.scene['effect'] = 0
                self.scene_manual_until = now_ms() + int(body.get('holdSeconds', 600)) * 1000
                self.set_setting('scene', self.scene)
                self._event('scene', self.scene)
                result = {'scene': self.scene}
            elif action == 'karaoke':
                self.karaoke.control(body, self.snapshot()['nowPlaying'], now_ms())
                self.set_setting('karaoke', self.karaoke.saved())
                self._event('karaoke', {'command': body.get('command', 'load'),
                                       'title': self.karaoke.sheet['title'] if self.karaoke.sheet else None})
                result = {'karaoke': self.snapshot()['karaoke']}
            elif action == 'configure':
                if 'speechEnabled' in body:
                    if not isinstance(body['speechEnabled'], bool):
                        raise ValueError('speechEnabled must be true or false.')
                    self.set_setting('speechEnabled', body['speechEnabled'])
                    if self.audio:
                        self.audio.set_enabled(body['speechEnabled'])
                if 'requestUrl' in body:
                    url = clean(body['requestUrl'], 300)
                    if not url.startswith(('http://', 'https://')):
                        raise ValueError('Expected an HTTP request URL.')
                    self.request_url = url
                    self.set_setting('requestUrl', url)
                    self._write_qr()
                if 'bookEnabled' in body:
                    self.set_setting('bookEnabled', body['bookEnabled'])
                    self.book_enabled = body['bookEnabled']
                    if not self.book_enabled:
                        self.book_moment = None
                result = {'requestUrl': self.request_url, 'bookEnabled': self.book_enabled,
                          'speech': self.audio.snapshot() if self.audio else {'available': False, 'enabled': False}}
            elif action == 'book':
                if not self.book_enabled:
                    raise ValueError('Book and photo visuals are disabled. Enable bookEnabled to display them.')
                filename = clean(body.get('filename', ''), 200)
                card = next((c for c in self._book_cards() if Path(c.get('imagePath') or c.get('path') or c.get('image') or '').name == filename), None)
                if not card or not self._show_book(card, body.get('seconds', 35)):
                    raise ValueError('Choose a filename from book/cards.json.')
                result = {'bookMoment': self.book_moment}
            elif action == 'request':
                result = self.request(body, context=context)
            elif action == 'move':
                # A pre-move poll must not overwrite the confirmed new order.
                # The bridge sends at most one revision-checked queue command.
                self._control_generation += 1
                self._start_command(command)
                result = self.client.move_queue(uid, before_uid, expected)
                if not isinstance(result, dict) or not isinstance(result.get('status'), dict):
                    raise SpotifyError('The queue move did not return a verified Spotify observation.')
                self._apply_observation(result['status'])
            elif action == 'reorder':
                self._control_generation += 1
                self._start_command(command)
                result = self.client.reorder_queue(ordered, expected, current_uid, device_id)
                if not isinstance(result, dict) or not isinstance(result.get('status'), dict):
                    raise SpotifyError('The queue reorder did not return a verified Spotify observation.', dispatched=True)
                self._apply_observation(result['status'])
            elif action == 'playback':
                if paused and self.audio:
                    self.audio.interrupt()
                self._control_generation += 1
                self._start_command(command)
                result = self.client.set_paused(paused, current_uid, device_id)
                observed = self._verified_playback_status(result, paused, current_uid, device_id)
                self._apply_observation(observed)
                self._event('playback', {'action': 'setPaused', 'paused': paused})
            elif action in ('next', 'pause', 'resume'):
                if self.audio and action in ('next', 'pause'):
                    self.audio.interrupt()
                # The client serializes playback operations with queue writes.
                # Older polls must not overwrite this command's observation,
                # including when dispatch fails with an uncertain outcome.
                self._control_generation += 1
                self._start_command(command)
                result = self.client.control(action)
                observed = result.get('status')
                if observed is not None:
                    self._apply_observation(observed)
                else:
                    # A fresh observation after next is not skip verification.
                    self._refresh()
                self._event('playback', {'action': action})
            else:
                raise ValueError('Unknown DJ action.')
            self._publish()
            return result

    def events(self, after=0, wait=0):
        deadline = time.monotonic() + min(max(wait, 0), 45)
        with self.changed:
            self.last_seen = now_ms()
            if after < 0:
                after = self.db.execute('SELECT COALESCE(MAX(id),0) FROM events').fetchone()[0]
                return {'events': [], 'lastId': after, 'state': self.snapshot()}
            while True:
                rows = self.db.execute('SELECT * FROM events WHERE id>? ORDER BY id LIMIT 100', (after,)).fetchall()
                if rows or time.monotonic() >= deadline or self.stop.is_set():
                    return {'events': [{'id': r['id'], 'kind': r['kind'],
                        'payload': json.loads(r['payload']), 'createdAt': r['created_at']} for r in rows],
                        'lastId': rows[-1]['id'] if rows else after, 'state': self.snapshot()}
                self.changed.wait(deadline - time.monotonic())

    def _apply_observation(self, status):
        """Reconcile one Spotify observation while the caller holds self.lock."""
        connected_before = self.spotify['connected']
        self.playback = status
        self.spotify = {'connected': True, 'error': None, 'mode': self.client.mode,
                        'observedAt': status.get('observedAt'),
                        'explicitQueueComplete': status.get('explicitQueueComplete') is True}
        current = status.get('nowPlaying') or {}
        uri = current.get('uri', '')
        queue = status.get('queue', [])
        by_uri = {t.get('uri'): t for t in queue}
        by_uid = {t['uid']: t for t in queue if t.get('uid')}
        current_key = current.get('uid') or uri
        if uri and current_key != self.last_track:
            self.last_track = current_key
            self._event('track', current)
        for pending in self.db.execute("SELECT * FROM requests WHERE status IN ('queued','uncertain','playing','removed')").fetchall():
            rid = pending['id']
            track = json.loads(pending['track'])
            uid = track.get('uid')
            restoring = pending['status'] == 'removed'
            # A temporary Connect queue omission can outlast the removal
            # grace. Positive evidence of the exact same occurrence
            # restores its request without dispatching another copy.
            if restoring and not (uid and (uid in by_uid or (uri and uid == current.get('uid')))):
                continue
            # Spotify may relink a catalogue URI when a track starts.
            # Its queue UID identifies the same requested occurrence.
            observed = by_uid.get(uid) if uid else by_uri.get(pending['uri'])
            if observed is None and uid:
                same_uri = by_uri.get(pending['uri'])
                if same_uri and not same_uri.get('uid'):
                    observed = same_uri
            if not uid and observed and observed.get('uid'):
                uid = track['uid'] = observed['uid']
                self.db.execute('UPDATE requests SET track=? WHERE id=?', (json.dumps(track, ensure_ascii=False), rid))
            if uid and current.get('uid'):
                is_current = bool(uri) and uid == current['uid']
            else:
                # An exact occurrence still in the queue outranks a
                # URI-only current-track observation of the same song.
                is_current = (bool(uri) and pending['uri'] == uri and not restoring
                              and not (uid and uid in by_uid))
            if is_current:
                if not uid and current.get('uid'):
                    track['uid'] = current['uid']
                    self.db.execute('UPDATE requests SET track=? WHERE id=?', (json.dumps(track, ensure_ascii=False), rid))
                if pending['status'] != 'playing':
                    self.db.execute("UPDATE requests SET status='playing',error=NULL,updated_at=? WHERE id=?", (now_ms(), rid))
                self.missing_requests.pop(rid, None)
            elif pending['status'] == 'playing' and uri:
                self.db.execute("UPDATE requests SET status='played',updated_at=? WHERE id=?", (now_ms(), rid))
            elif observed:
                if pending['status'] in ('uncertain', 'removed'):
                    self.db.execute("UPDATE requests SET status='queued',error=NULL,updated_at=? WHERE id=?", (now_ms(), rid))
                self.missing_requests.pop(rid, None)
            elif pending['status'] == 'queued' and status.get('explicitQueueComplete'):
                first, count = self.missing_requests.get(rid, (now_ms(), 0))
                self.missing_requests[rid] = (first, count + 1)
                if count >= 1 and now_ms() - max(first, pending['updated_at']) >= 10000:
                    self.db.execute("UPDATE requests SET status='removed',updated_at=? WHERE id=?", (now_ms(), rid))
                    self.missing_requests.pop(rid, None)
                    self._event('removed', {'id': rid, 'uri': pending['uri']})
            if restoring:
                self._event('queue_restored', {'id': rid, 'uri': pending['uri'], 'uid': uid,
                                              'status': 'playing' if is_current else 'queued'})
        self.db.commit()
        observed_current = status.get('nowPlaying')
        playing = self.db.execute("SELECT * FROM requests WHERE status='playing' ORDER BY created_at LIMIT 1").fetchone()
        if observed_current and playing:
            observed_current = {**observed_current, 'requestedUri': playing['uri'],
                'requestId': playing['id'], 'requestedBy': playing['requested_by'], 'dedication': playing['dedication']}
        if self.karaoke.observe(observed_current, self.spotify['connected'], now_ms()):
            self.set_setting('karaoke', self.karaoke.saved())
        if self.audio:
            requests = [self._request_dict(r) for r in self.db.execute(
                "SELECT * FROM requests WHERE status IN ('pending','queued','uncertain','playing') ORDER BY created_at").fetchall()]
            self.audio.observe(observed_current, requests, playback_state=status.get('playbackState'))
        if not connected_before:
            self._event('spotify', {'connected': True, 'mode': self.client.mode})
        # Compare observed occurrences, including their order and duplicates.
        # Keep this baseline across disconnects; clearing the public cache is
        # not an observation that Spotify removed its queue.
        occurrences = [{key: item.get(key) for key in ('uid', 'uri', 'source')}
                       for item in queue]
        signature = tuple((item['uid'], item['uri'], item['source']) for item in occurrences)
        if self._queue_signature is not None and signature != self._queue_signature:
            self._event('queue_changed', {'queue': occurrences})
        self._queue_signature = signature

    def _refresh(self):
        with self.lock:
            generation = self._control_generation
        try:
            status = self.client.status()
            with self.lock:
                if generation != self._control_generation:
                    return
                self._apply_observation(status)
        except Exception as error:
            with self.lock:
                if generation != self._control_generation:
                    return
                self._observation_failed(error)

    def _dispatch(self):
        with self.lock:
            if self.stop.is_set() or not self.spotify['connected'] or now_ms() < self.next_dispatch_at:
                return
            row = self.db.execute("SELECT * FROM requests WHERE status='pending' ORDER BY created_at LIMIT 1").fetchone()
            if not row:
                return
            self.db.execute("UPDATE requests SET status='sending',updated_at=? WHERE id=?", (now_ms(), row['id']))
            self.db.commit()
            command = self._new_command('enqueue', {'requestId': row['id'], 'uri': row['uri']}, self._stored_origin(row))
            self._start_command(command)
        try:
            result = self.client.enqueue(row['uri'], row['id'])
            if (not isinstance(result, dict) or result.get('queued') is not True
                    or result.get('uri') != row['uri'] or result.get('requestId') != row['id']
                    or not isinstance(result.get('uid'), str) or not result['uid'].strip() or len(result['uid']) > 512):
                raise SpotifyError('The queue add did not return a confirmed requested occurrence.', dispatched=True)
        except Exception as error:
            with self.lock:
                self._command_event(command, 'error', error=error)
                known_not_sent = isinstance(error, SpotifyError) and error.dispatched is False
                state = 'pending' if known_not_sent else 'uncertain'
                self.db.execute("UPDATE requests SET status=?,error=?,updated_at=? WHERE id=?",
                    (state, str(error)[:300], now_ms(), row['id']))
                self.next_dispatch_at = now_ms() + 5000 if known_not_sent else 0
                self._event('queue_retry' if known_not_sent else 'queue_uncertain',
                    {'id': row['id'], 'uri': row['uri'], 'error': str(error)[:300]})
        else:
            with self.lock:
                self._command_event(command, 'returned', result=result)
                if isinstance(result, dict) and result.get('uid'):
                    track = json.loads(row['track'])
                    track['uid'] = result['uid']
                    self.db.execute('UPDATE requests SET track=? WHERE id=?', (json.dumps(track, ensure_ascii=False), row['id']))
                self.db.execute("UPDATE requests SET status='queued',error=NULL,updated_at=? WHERE id=?", (now_ms(), row['id']))
                item = self._request_dict(self.db.execute('SELECT * FROM requests WHERE id=?', (row['id'],)).fetchone())
                self._event('queued', item)
                text = f"{row['requested_by']}: {item['title']}"
                if row['dedication']:
                    text += f". {row['dedication']}"
                self.cue(text)

    def run(self):
        while not self.stop.is_set():
            self._refresh()
            if self.stop.is_set():
                break
            self._dispatch()
            with self.lock:
                self._advance_captions()
                self._advance_show()
                self._publish()
            self.stop.wait(2.5)


class Handler(BaseHTTPRequestHandler):
    server_version = 'LaricsDJ/1.0'

    @property
    def party(self):
        return self.server.party

    def log_message(self, fmt, *args):
        if args and len(args) > 1 and str(args[1]).startswith(('4', '5')):
            super().log_message(fmt, *args)

    def send_json(self, value, status=200):
        data = json.dumps(value, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def authorized(self):
        token = self.headers.get('Authorization', '')
        return hmac.compare_digest(token, 'Bearer ' + self.party.admin_token)

    def request_context(self):
        return {'operatorLabel': self.headers.get('X-DJ-Operator', ''),
                'httpPeer': {'address': self.client_address[0], 'port': self.client_address[1]}}

    def file(self, base, relative):
        base = base.resolve()
        filename = (base / unquote(relative)).resolve()
        if not filename.is_relative_to(base) or not filename.is_file():
            self.send_json({'error': 'Not found.'}, 404)
            return
        data = filename.read_bytes()
        self.send_response(200)
        self.send_header('Content-Type', mimetypes.guess_type(filename.name)[0] or 'application/octet-stream')
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-cache' if base == (ROOT / 'web').resolve() else 'public, max-age=3600')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        url = urlsplit(self.path)
        query = parse_qs(url.query)
        try:
            if url.path.startswith('/api/admin/'):
                if not self.authorized():
                    self.send_json({'error': 'DJ access required.'}, 401)
                elif url.path == '/api/admin/events':
                    self.send_json(self.party.events(int(query.get('after', ['0'])[0]), float(query.get('wait', ['0'])[0])))
                elif url.path == '/api/admin/message-info':
                    self.send_json(self.party.message_info(int(query.get('id', ['0'])[0])))
                else:
                    self.send_json({'error': 'Unknown endpoint.'}, 404)
            elif url.path == '/api/state':
                self.party.visit(query.get('guestId', [''])[0])
                self.send_json(self.party.snapshot())
            elif url.path == '/api/search':
                self.party.limit(self.client_address[0], 'search', 30, 60)
                self.send_json(self.party.search(query.get('q', [''])[0]))
            elif url.path == '/api/search/describe':
                self.send_json(self.party.description_search(query.get('id', [''])[0]))
            elif url.path == '/api/track':
                self.party.limit(self.client_address[0], 'track', 30, 60)
                self.send_json(self.party.lookup_track(query.get('uri', [''])[0]))
            elif url.path == '/api/chat':
                self.send_json(self.party.chat(int(query.get('after', ['0'])[0])))
            elif url.path == '/health':
                self.send_json({'ok': True, 'spotify': self.party.spotify})
            elif url.path.startswith('/book/'):
                self.file(self.party.data / 'book', url.path[6:])
            elif url.path == '/request-qr.png':
                self.file(self.party.data, 'request-qr.png')
            else:
                self.file(ROOT / 'web', 'index.html' if url.path == '/' else url.path.lstrip('/'))
        except ValueError as error:
            self.send_json({'error': str(error)}, 400)
        except SpotifyError as error:
            self.send_json({'tracks': [], 'error': str(error)}, 503)
        except Exception as error:
            print(f'GET {url.path}: {type(error).__name__}: {error}', flush=True)
            self.send_json({'error': 'The DJ connection is recovering. Try again shortly.'}, 503)

    def do_POST(self):
        url = urlsplit(self.path)
        try:
            length = int(self.headers.get('Content-Length', '0'))
            maximum = 262144 if url.path == '/api/admin/karaoke' and self.authorized() else 16384
            if length <= 0 or length > maximum:
                raise ValueError('Invalid message size.')
            origin = self.headers.get('Origin')
            if origin and urlsplit(origin).netloc != self.headers.get('Host'):
                self.send_json({'error': 'Use the party page to send requests.'}, 403)
                return
            body = json.loads(self.rfile.read(length))
            if not isinstance(body, dict):
                raise ValueError('Expected a JSON object.')
            if url.path.startswith('/api/admin/'):
                if not self.authorized():
                    self.send_json({'error': 'DJ access required.'}, 401)
                    return
                self.send_json(self.party.admin(url.path.rsplit('/', 1)[-1], body, context=self.request_context()))
            elif url.path == '/api/request':
                self.party.limit(self.client_address[0], 'request', 15, 60)
                self.send_json(self.party.request(body, context=self.request_context()))
            elif url.path == '/api/message':
                self.party.limit(self.client_address[0], 'message', 20, 60)
                self.party.visit(body.get('guestId', ''))
                self.send_json(self.party.message(body, context=self.request_context()))
            elif url.path == '/api/message/recall':
                self.party.limit(self.client_address[0], 'recall', 30, 60)
                self.send_json(self.party.recall_message(body))
            elif url.path == '/api/search/describe':
                self.party.limit(self.client_address[0], 'describe', 15, 60)
                self.send_json(self.party.describe_search(body))
            elif url.path == '/api/mood':
                self.party.limit(self.client_address[0], 'mood', 20, 60)
                self.party.visit(body.get('guestId', ''))
                self.send_json(self.party.mood(body))
            else:
                self.send_json({'error': 'Unknown endpoint.'}, 404)
        except (ValueError, TypeError) as error:
            self.send_json({'error': str(error)}, 400)
        except SpotifyError as error:
            detail = {'error': str(error)}
            if url.path in ('/api/admin/move', '/api/admin/reorder', '/api/admin/sync', '/api/admin/playback',
                            '/api/admin/next', '/api/admin/pause', '/api/admin/resume'):
                detail['dispatched'] = error.dispatched
            if url.path == '/api/admin/sync':
                detail['state'] = self.party.snapshot()
            self.send_json(detail, 503)
        except Exception as error:
            print(f'POST {url.path}: {type(error).__name__}: {error}', flush=True)
            self.send_json({'error': 'The DJ connection is recovering. Try again shortly.'}, 503)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--bind', default='0.0.0.0')
    parser.add_argument('--port', type=int, default=8787)
    parser.add_argument('--data', type=Path, default=DEFAULT_DATA)
    args = parser.parse_args()
    audio = DedicationAudio(args.data)
    party = Party(args.data, args.port, audio=audio)
    server = ThreadingHTTPServer((args.bind, args.port), Handler)
    server.daemon_threads = True
    server.party = party
    audio.start()
    worker = threading.Thread(target=party.run, name='spotify-state', daemon=True)
    worker.start()
    print(f'LARICS / LYRICS is listening at {party.request_url}', flush=True)
    print(f'State: {party.data / "state.json"}', flush=True)
    try:
        server.serve_forever(poll_interval=.5)
    except KeyboardInterrupt:
        pass
    finally:
        party.stop.set()
        with party.changed:
            party.changed.notify_all()
        audio.close()
        worker.join()
        party.client.close()
        server.server_close()


if __name__ == '__main__':
    main()
