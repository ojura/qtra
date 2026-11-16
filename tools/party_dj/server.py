#!/usr/bin/env python3
"""Local song requests, live DJ chat and state for qtra's party stage."""
from __future__ import annotations

import argparse
import collections
import hmac
import json
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

ROOT = Path(__file__).resolve().parent
DEFAULT_DATA = Path.home() / '.local/state/larics-dj'
MOODS = {'chill': ('Lagano', .22), 'groove': ('Groove', .48),
         'dance': ('Plešemo', .74), 'chaos': ('Kaos', .95),
         'singalong': ('Pjevamo', .64)}
TRACK = re.compile(r'^spotify:track:[A-Za-z0-9]{22}$')


def now_ms():
    return int(time.time() * 1000)


def clean(value, maximum=500):
    if not isinstance(value, str):
        raise ValueError('Expected text.')
    return ''.join(c for c in value if c in '\n\t' or ord(c) >= 32).strip()[:maximum]


def atomic_json(filename, value):
    temporary = filename.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, ensure_ascii=False), encoding='utf-8')
    os.replace(temporary, filename)


class Party:
    def __init__(self, data=DEFAULT_DATA, port=8787, client=None):
        self.data = Path(data)
        self.data.mkdir(parents=True, exist_ok=True)
        self.port = port
        self.lock = threading.RLock()
        self.changed = threading.Condition(self.lock)
        self.stop = threading.Event()
        self.client = client or SpotifyClient()
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
        ''')
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
        self.spotify = {'connected': False, 'error': 'Connecting to Spotify.'}
        self.revision = 0
        self.last_seen = 0
        self.last_track = ''
        self.missing_requests = {}
        self.next_dispatch_at = 0
        self.last_auto_caption = 0
        self.last_auto_scene = 0
        self.scene_index = 0
        self.last_book = 0
        self.book_index = -1
        self.book_moment = None
        self.caption = None
        self.caption_queue = collections.deque()
        self.scene = self.setting('scene', {'mode': 'auto', 'energy': .65, 'palette': 'orange', 'effect': 5})
        self.scene_manual_until = 0
        self.mood_votes = {}
        self.captions = self._load_captions()
        self.caption_index = 0
        # A previous process may have stopped after dispatch. Reconcile these
        # with the native queue before considering another dispatch.
        self.db.execute("UPDATE requests SET status='uncertain' WHERE status='sending'")
        self.db.commit()
        self.request_url = self.setting('requestUrl', self._lan_url())
        self._write_qr()
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

    def _chat_dict(self, row):
        return {'id': row['id'], 'role': row['role'], 'name': row['name'],
                'text': row['text'], 'createdAt': row['created_at'],
                'status': 'read' if row['read_at'] else 'delivered'}

    def _dj(self):
        age = now_ms() - self.last_seen
        return {'status': 'listening' if age < 90000 else 'away', 'lastSeen': self.last_seen}

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
            own = {r['uri']: self._request_dict(r) for r in rows}
            queue = []
            seen = set()
            for item in self.playback.get('queue', []):
                merged = {**item, 'status': 'queued'}
                if item.get('uri') in own:
                    merged.update({k: v for k, v in own[item['uri']].items() if k in ('id', 'requestedBy', 'dedication', 'status')})
                queue.append(merged)
                seen.add(item.get('uri'))
            queue.extend(self._request_dict(r) for r in rows if r['uri'] not in seen and r['status'] != 'playing')
            result = {'revision': self.revision, 'nowPlaying': self.playback.get('nowPlaying'),
                'queue': queue, 'scene': self.scene.copy(), 'caption': self.caption,
                'mood': self._mood(), 'guestCount': sum(now_ms() - t < 90000 for t in self.guests.values()),
                'requestUrl': self.request_url, 'qrPath': str(self.data / 'request-qr.png'),
                'spotify': self.spotify.copy(), 'dj': self._dj(), 'bookMoment': self.book_moment,
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

    def request(self, body):
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
            self.db.execute('INSERT INTO requests VALUES(?,?,?,?,?,?,?,?,?)',
                (rid, uri, name, dedication, json.dumps(track, ensure_ascii=False), 'pending', stamp, stamp, None))
            self._event('request', {**track, 'id': rid, 'requestedBy': name, 'dedication': dedication})
            self._publish()
            return {'request': self._request_dict(self.db.execute('SELECT * FROM requests WHERE id=?', (rid,)).fetchone()), 'status': 'pending'}

    def chat(self, after=0):
        with self.lock:
            if after:
                rows = self.db.execute('SELECT * FROM chat WHERE id>? ORDER BY id LIMIT 100', (after,)).fetchall()
            else:
                rows = self.db.execute('SELECT * FROM (SELECT * FROM chat ORDER BY id DESC LIMIT 80) ORDER BY id').fetchall()
            return {'messages': [self._chat_dict(r) for r in rows], 'dj': self._dj()}

    def message(self, body, role='user'):
        text = clean(body.get('text', ''), 1200)
        if not text:
            raise ValueError('Write a message first.')
        rid = clean(body.get('requestId', '') or secrets.token_hex(16), 100)
        name = 'DJ' if role == 'assistant' else clean(body.get('name', ''), 60) or 'Gost'
        with self.lock:
            row = self.db.execute('SELECT * FROM chat WHERE request_id=?', (rid,)).fetchone()
            if row:
                if row['role'] != role or row['text'] != text:
                    raise ValueError('This message ID was already used.')
                return {'message': self._chat_dict(row)}
            self.db.execute('INSERT INTO chat(request_id,role,name,text,created_at) VALUES(?,?,?,?,?)',
                (rid, role, name, text, now_ms()))
            row = self.db.execute('SELECT * FROM chat WHERE request_id=?', (rid,)).fetchone()
            item = self._chat_dict(row)
            self._event('chat' if role == 'user' else 'reply', item)
            self._publish()
            return {'message': item}

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
        if self.scene.get('mode') == 'auto' and stamp > self.scene_manual_until and stamp - self.last_auto_scene > 75000:
            effects = [1, 6, 3, 0, 7, 2, 5, 4]
            self.scene['effect'] = effects[self.scene_index % len(effects)]
            self.scene_index += 1
            self.last_auto_scene = stamp
        if stamp - self.last_book > 105000:
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

    def admin(self, action, body):
        with self.lock:
            self.last_seen = now_ms()
            read_through = body.get('readThrough')
            if read_through is not None:
                self.db.execute('UPDATE chat SET read_at=? WHERE role=? AND id<=?', (now_ms(), 'user', int(read_through)))
                self.db.commit()
            if action == 'heartbeat':
                result = {'dj': self._dj()}
            elif action == 'reply':
                result = self.message(body, 'assistant')
                if body.get('caption'):
                    self.cue(body['text'], body.get('seconds'))
            elif action == 'cue':
                self.cue(body.get('text', ''), body.get('seconds'))
                result = {'queued': True}
            elif action == 'scene':
                mode = body.get('mode', self.scene['mode'])
                if mode not in ('auto', 'orbit', 'laser', 'robots', 'quiet'):
                    raise ValueError('Unknown scene.')
                self.scene.update({'mode': mode, 'energy': max(0, min(1, float(body.get('energy', self.scene['energy'])))),
                    'palette': clean(body.get('palette', self.scene['palette']), 30)})
                if 'effect' in body:
                    self.scene['effect'] = int(body['effect'])
                self.scene_manual_until = now_ms() + int(body.get('holdSeconds', 600)) * 1000
                self.set_setting('scene', self.scene)
                self._event('scene', self.scene)
                result = {'scene': self.scene}
            elif action == 'configure':
                if 'requestUrl' in body:
                    url = clean(body['requestUrl'], 300)
                    if not url.startswith(('http://', 'https://')):
                        raise ValueError('Expected an HTTP request URL.')
                    self.request_url = url
                    self.set_setting('requestUrl', url)
                    self._write_qr()
                result = {'requestUrl': self.request_url}
            elif action == 'book':
                filename = clean(body.get('filename', ''), 200)
                card = next((c for c in self._book_cards() if Path(c.get('imagePath') or c.get('path') or c.get('image') or '').name == filename), None)
                if not card or not self._show_book(card, body.get('seconds', 35)):
                    raise ValueError('Choose a filename from book/cards.json.')
                result = {'bookMoment': self.book_moment}
            elif action == 'request':
                result = self.request(body)
            elif action in ('next', 'pause', 'resume'):
                # The client serializes playback operations with queue writes.
                result = self.client.control(action)
                self._event('playback', {'action': action})
            else:
                raise ValueError('Unknown DJ action.')
            self._publish()
            return result

    def events(self, after=0, wait=0):
        deadline = time.monotonic() + min(max(wait, 0), 45)
        with self.changed:
            self.last_seen = now_ms()
            while True:
                rows = self.db.execute('SELECT * FROM events WHERE id>? ORDER BY id LIMIT 100', (after,)).fetchall()
                if rows or time.monotonic() >= deadline or self.stop.is_set():
                    return {'events': [{'id': r['id'], 'kind': r['kind'],
                        'payload': json.loads(r['payload']), 'createdAt': r['created_at']} for r in rows],
                        'lastId': rows[-1]['id'] if rows else after, 'state': self.snapshot()}
                self.changed.wait(deadline - time.monotonic())

    def _refresh(self):
        try:
            status = self.client.status()
            with self.lock:
                connected_before = self.spotify['connected']
                self.playback = status
                self.spotify = {'connected': True, 'error': None, 'mode': self.client.mode}
                current = status.get('nowPlaying') or {}
                uri = current.get('uri', '')
                queue_uris = {t.get('uri') for t in status.get('queue', [])}
                if uri and uri != self.last_track:
                    if self.last_track:
                        self.db.execute("UPDATE requests SET status='played',updated_at=? WHERE uri=? AND status='playing'", (now_ms(), self.last_track))
                    self.db.execute("UPDATE requests SET status='playing',updated_at=? WHERE uri=? AND status IN ('queued','uncertain','pending')", (now_ms(), uri))
                    self.last_track = uri
                    self._event('track', current)
                for pending in self.db.execute("SELECT id,uri FROM requests WHERE status='uncertain'").fetchall():
                    if pending['uri'] in queue_uris or pending['uri'] == uri:
                        self.db.execute("UPDATE requests SET status=?,error=NULL,updated_at=? WHERE id=?",
                            ('playing' if pending['uri'] == uri else 'queued', now_ms(), pending['id']))
                if status.get('explicitQueueComplete'):
                    for pending in self.db.execute("SELECT id,uri,updated_at FROM requests WHERE status='queued'").fetchall():
                        rid = pending['id']
                        if pending['uri'] in queue_uris or pending['uri'] == uri:
                            self.missing_requests.pop(rid, None)
                            continue
                        first, count = self.missing_requests.get(rid, (now_ms(), 0))
                        self.missing_requests[rid] = (first, count + 1)
                        if count >= 1 and now_ms() - max(first, pending['updated_at']) >= 10000:
                            self.db.execute("UPDATE requests SET status='removed',updated_at=? WHERE id=?", (now_ms(), rid))
                            self.missing_requests.pop(rid, None)
                            self._event('removed', {'id': rid, 'uri': pending['uri']})
                self.db.commit()
                if not connected_before:
                    self._event('spotify', {'connected': True, 'mode': self.client.mode})
        except Exception as error:
            with self.lock:
                message = str(error)[:300]
                changed = self.spotify['connected'] or self.spotify.get('error') != message
                self.spotify = {'connected': False, 'error': message}
                if changed:
                    self._event('spotify', self.spotify)

    def _dispatch(self):
        with self.lock:
            if self.stop.is_set() or not self.spotify['connected'] or now_ms() < self.next_dispatch_at:
                return
            row = self.db.execute("SELECT * FROM requests WHERE status='pending' ORDER BY created_at LIMIT 1").fetchone()
            if not row:
                return
            self.db.execute("UPDATE requests SET status='sending',updated_at=? WHERE id=?", (now_ms(), row['id']))
            self.db.commit()
        try:
            self.client.enqueue(row['uri'], row['id'])
        except Exception as error:
            with self.lock:
                known_not_sent = isinstance(error, SpotifyError) and not error.dispatched
                state = 'pending' if known_not_sent else 'uncertain'
                self.db.execute("UPDATE requests SET status=?,error=?,updated_at=? WHERE id=?",
                    (state, str(error)[:300], now_ms(), row['id']))
                self.next_dispatch_at = now_ms() + 5000 if known_not_sent else 0
                self._event('queue_retry' if known_not_sent else 'queue_uncertain',
                    {'id': row['id'], 'uri': row['uri'], 'error': str(error)[:300]})
        else:
            with self.lock:
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
                else:
                    self.send_json({'error': 'Unknown endpoint.'}, 404)
            elif url.path == '/api/state':
                self.party.visit(query.get('guestId', [''])[0])
                self.send_json(self.party.snapshot())
            elif url.path == '/api/search':
                self.party.limit(self.client_address[0], 'search', 30, 60)
                self.send_json(self.party.search(query.get('q', [''])[0]))
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
            if length <= 0 or length > 16384:
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
                self.send_json(self.party.admin(url.path.rsplit('/', 1)[-1], body))
            elif url.path == '/api/request':
                self.party.limit(self.client_address[0], 'request', 15, 60)
                self.send_json(self.party.request(body))
            elif url.path == '/api/message':
                self.party.limit(self.client_address[0], 'message', 20, 60)
                self.party.visit(body.get('guestId', ''))
                self.send_json(self.party.message(body))
            elif url.path == '/api/mood':
                self.party.limit(self.client_address[0], 'mood', 20, 60)
                self.party.visit(body.get('guestId', ''))
                self.send_json(self.party.mood(body))
            else:
                self.send_json({'error': 'Unknown endpoint.'}, 404)
        except (ValueError, TypeError) as error:
            self.send_json({'error': str(error)}, 400)
        except SpotifyError as error:
            self.send_json({'error': str(error)}, 503)
        except Exception as error:
            print(f'POST {url.path}: {type(error).__name__}: {error}', flush=True)
            self.send_json({'error': 'The DJ connection is recovering. Try again shortly.'}, 503)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--bind', default='0.0.0.0')
    parser.add_argument('--port', type=int, default=8787)
    parser.add_argument('--data', type=Path, default=DEFAULT_DATA)
    args = parser.parse_args()
    party = Party(args.data, args.port)
    server = ThreadingHTTPServer((args.bind, args.port), Handler)
    server.daemon_threads = True
    server.party = party
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
        worker.join()
        party.client.close()
        server.server_close()


if __name__ == '__main__':
    main()
