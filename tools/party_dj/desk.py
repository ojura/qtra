"""Reusable DJ operations over the existing party service; no player internals here."""
from __future__ import annotations

import argparse
import http.client
import json
import math
import os
from pathlib import Path
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import unicodedata
import uuid


REPO = Path(__file__).resolve().parents[2]
DEFAULT_DATA = Path.home() / '.local/state/larics-dj'
REORDER_FIELDS = ('orderedQueuedUids', 'expectedQueuedUids', 'expectedCurrentUid', 'expectedDeviceId')
COMPOSER_FIELDS = ('guestId', 'composerId', 'draftRevision', 'text', 'editorMode')
sys.path.insert(0, str(REPO / 'tools'))
from agentctl import AgentClient
from launch_party_stage import invoke


class DeskError(Exception):
    def __init__(self, detail, transient=False):
        self.detail = detail if isinstance(detail, dict) else {'error': str(detail)}
        self.transient = transient
        super().__init__(self.detail.get('error', 'DJ operation failed.'))


def now_ms():
    return int(time.time() * 1000)


class ServiceClient:
    def __init__(self, url='http://127.0.0.1:8787', data=DEFAULT_DATA, operator=None):
        self.url = url.rstrip('/')
        self.data = Path(data)
        self.operator = operator or os.environ.get('QTRA_DJ_OPERATOR', 'virtual-dj')

    def call(self, path, body=None, timeout=30):
        headers = {'Content-Type': 'application/json'}
        if path.startswith('/api/admin/'):
            headers['Authorization'] = 'Bearer ' + (self.data / 'admin-token').read_text().strip()
            headers['X-DJ-Operator'] = self.operator
        request = urllib.request.Request(self.url + path,
            data=json.dumps(body, ensure_ascii=False).encode() if body is not None else None,
            headers=headers)
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                result = json.load(response)
                if not isinstance(result, dict):
                    raise ValueError('The service returned an invalid response object.')
                return result
        except urllib.error.HTTPError as error:
            try:
                raw = error.read().decode('utf-8', errors='replace')
                detail = json.loads(raw)
                if not isinstance(detail, dict):
                    raise ValueError('The service returned an invalid error object.')
                if error.code >= 500 and body is not None and 'dispatched' not in detail:
                    detail = {**detail, 'responseLost': True}
            except (ValueError, OSError, http.client.HTTPException) as body_error:
                detail = {'error': f'HTTP {error.code}: unreadable response: {body_error}',
                          'responseLost': body is not None}
            raise DeskError(detail, transient=error.code in (408, 425, 429) or error.code >= 500) from error
        except (urllib.error.URLError, TimeoutError, OSError, ValueError, http.client.HTTPException) as error:
            # A lost response never authorizes a replay of a write.
            raise DeskError({'error': str(error), 'responseLost': body is not None},
                            transient=isinstance(error, (OSError, TimeoutError, urllib.error.URLError,
                                                         http.client.HTTPException))) from error

    def sync(self):
        return self.call('/api/admin/sync', {})


def actual_queue(sync):
    """Reject stale, partial or unidentified observations before constructing a plan."""
    if not isinstance(sync, dict) or not isinstance(sync.get('state'), dict) or not isinstance(sync.get('observation'), dict):
        raise DeskError('The service has not returned a complete observation object.')
    state, observed = sync.get('state', {}), sync.get('observation', {})
    if not isinstance(state.get('spotify'), dict) or not state['spotify'].get('connected'):
        raise DeskError('Spotify is disconnected; inspect the current player before changing its queue.')
    stamp = observed.get('observedAt')
    if (type(stamp) not in (int, float) or not math.isfinite(stamp)
            or not -1000 <= now_ms() - stamp <= 10000):
        raise DeskError('The Spotify observation is stale or has no valid read time.')
    if observed.get('explicitQueueComplete') is not True:
        raise DeskError('Spotify has not supplied its complete explicit queue.')
    current = observed.get('nowPlaying') or {}
    playback = observed.get('playbackState') or {}
    device = playback.get('device') or {} if isinstance(playback, dict) else {}
    if (not isinstance(current, dict) or not isinstance(device, dict)
            or not isinstance(current.get('uid'), str) or not current['uid']
            or not isinstance(device.get('id'), str) or not device['id']):
        raise DeskError('The current occurrence and active device must both be known.')
    if current.get('clockValid') is False or playback.get('clockValid') is False:
        raise DeskError('Spotify has no valid current playback clock; wait for a fresh observation.')
    raw_queue = observed.get('queue')
    if not isinstance(raw_queue, list) or any(not isinstance(item, dict) or item.get('source') not in ('queue', 'context') for item in raw_queue):
        raise DeskError('Spotify has not returned a valid complete queue list.')
    queue = [item for item in raw_queue if item['source'] == 'queue']
    uids = [item.get('uid') for item in queue]
    if any(not isinstance(uid, str) or not uid for uid in uids) or len(set(uids)) != len(uids):
        raise DeskError('The explicit queue has missing or ambiguous occurrence UIDs.')
    return queue, current, device


def plan_order(sync, selected, front=True):
    queue, current, device = actual_queue(sync)
    expected = [item['uid'] for item in queue]
    if len(set(selected)) != len(selected) or any(uid not in expected for uid in selected):
        raise DeskError('Select each existing queue occurrence once, by its exact UID.')
    ordered = list(selected) + [uid for uid in expected if uid not in selected] if front else list(selected)
    if set(ordered) != set(expected) or len(ordered) != len(expected):
        raise DeskError('A full order must contain every explicit queue UID exactly once.')
    by_uid = {item['uid']: item for item in queue}
    seen = {item.get('uri') for item in sync.get('recentTracks', [])}
    return {'operation': 'reorder', 'createdAt': now_ms(),
            'expectedCurrentUid': current['uid'], 'expectedDeviceId': device['id'],
            'expectedQueuedUids': expected, 'orderedQueuedUids': ordered,
            'current': current, 'device': device, 'changed': ordered != expected,
            'front': [by_uid[uid] for uid in ordered[:3]],
            'previouslyObserved': [uid for uid in ordered[:3] if by_uid[uid].get('uri') in seen]}


def apply_order(client, plan):
    if not isinstance(plan, dict) or plan.get('operation') != 'reorder' or any(key not in plan for key in REORDER_FIELDS):
        raise DeskError('Choose a queue plan produced by front or order.')
    return client.call('/api/admin/reorder', {key: plan[key] for key in REORDER_FIELDS})


def concise_state(state):
    keys = ('uid', 'uri', 'title', 'artist', 'album', 'playing', 'paused', 'buffering',
            'progressMs', 'durationMs', 'observedAt', 'requestedBy')
    current = state.get('nowPlaying')
    dj = state.get('dj') or {}
    presence = {key: dj[key] for key in ('status', 'lastSeen', 'llm', 'watcher', 'processingMessageIds') if key in dj}
    work_counts = {}
    for work in (dj.get('messageWork') or {}).values():
        if isinstance(work, dict):
            status = work.get('effectiveState', work.get('state', 'unconfirmed'))
            work_counts[status] = work_counts.get(status, 0) + 1
    if work_counts:
        presence['workCounts'] = work_counts
    return {'spotify': state.get('spotify'),
            'nowPlaying': {key: current[key] for key in keys if key in current} if current else None,
            'device': (state.get('playbackState') or {}).get('device'),
            'front': [{key: item[key] for key in keys if key in item}
                      for item in state.get('queue', []) if item.get('source') == 'queue'][:3],
            'pending': [item for item in state.get('queue', []) if item.get('status') in ('pending', 'sending', 'uncertain')],
            'dj': presence}


def write_json(value, path=None):
    text = json.dumps(value, ensure_ascii=False, indent=2)
    if path:
        path = Path(path)
        temporary = path.with_name(path.name + '.tmp')
        temporary.write_text(text + '\n', encoding='utf-8')
        os.replace(temporary, path)
    else:
        print(text, flush=True)


def set_presence(client, state, agent_session, sequence, lease_seconds=60):
    """One explicit agent assertion; event polling never calls this function."""
    if (state not in ('online', 'offline') or not isinstance(agent_session, str)
            or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._:-]{0,127}', agent_session)
            or type(sequence) is not int or not 1 <= sequence <= 2**53 - 1
            or type(lease_seconds) is not int or not 30 <= lease_seconds <= 120):
        raise DeskError('Use a session identifier, positive sequence and a 30-120 second presence lease.')
    result = client.call('/api/admin/presence', {'agentSessionId': agent_session,
        'sequence': sequence, 'state': state, 'leaseSeconds': lease_seconds})
    if isinstance(result.get('dj'), dict):
        result = {**result, 'dj': concise_state({'dj': result['dj']})['dj']}
    return result


def native_stage(data, request):
    """Address only the existing stage recorded by its launcher; never start it."""
    metadata = json.loads((Path(data) / 'qtra-modules.json').read_text(encoding='utf-8'))
    if not isinstance(metadata, dict) or not isinstance(metadata.get('modules'), dict):
        raise DeskError('The existing native stage metadata is unavailable.')
    socket_path, pid = metadata.get('socket'), metadata.get('pid')
    module = metadata['modules'].get('party_stage')
    if (not isinstance(socket_path, str) or not Path(socket_path).is_absolute()
            or type(pid) is not int or pid <= 0 or not isinstance(module, str) or not module.isdigit()):
        raise DeskError('The existing native stage metadata is incomplete.')
    try:
        with AgentClient(socket_path, timeout=10) as client:
            hello = client.request('hello', {})
            if (not isinstance(hello, dict) or hello.get('name') != 'qt-runtime-cube' or hello.get('uid') != os.getuid()
                    or hello.get('pid') != pid):
                raise DeskError('The native stage process changed; inspect its current launcher metadata.')
            modules = client.request('module.list', {})
            if not isinstance(modules, list) or not any(isinstance(item, dict)
                    and str(item.get('id')) == module and item.get('name') == 'LARICS / LYRICS party stage'
                    for item in modules):
                raise DeskError('The recorded native stage module is no longer loaded.')
            if any(isinstance(item, dict) and item.get('name') == 'LARICS / LYRICS party stage'
                    and str(item.get('id', '')).isdigit() and int(item['id']) > int(module)
                    for item in modules):
                raise DeskError('A newer native stage generation is loaded; refresh launcher metadata before composer control.')
            result = invoke(client, module, 'gui', request)
            if not isinstance(result, dict):
                raise DeskError('The native stage returned an invalid response.')
            return result
    except (RuntimeError, TimeoutError, KeyError, TypeError) as error:
        raise DeskError(str(error)) from error


def composer_binding(draft):
    if (not isinstance(draft, dict) or any(key not in draft for key in COMPOSER_FIELDS)
            or any(not isinstance(draft[key], str) or not draft[key] for key in ('guestId', 'composerId'))
            or type(draft['draftRevision']) is not int or not 0 <= draft['draftRevision'] <= 2**53 - 1
            or not isinstance(draft['text'], str) or draft['editorMode'] not in ('message', 'dedication')):
        raise DeskError('Read the current native composer before applying an intent cue.')
    return {key: draft[key] for key in COMPOSER_FIELDS}


def read_composer(data):
    draft = native_stage(data, {'getComposerDraft': True}).get('composerDraft')
    composer_binding(draft)
    return draft


def set_search_intent(data, draft, intent):
    if intent not in ('chat', 'music', 'auto'):
        raise DeskError('Choose chat, music or auto search intent.')
    cue = {**composer_binding(draft), 'intent': intent}
    result = native_stage(data, {'setSearchIntent': cue}).get('searchIntentResult')
    if not isinstance(result, dict) or result.get('applied') is not True:
        raise DeskError(result if isinstance(result, dict) and result.get('error')
                        else 'The native stage did not confirm this exact draft intent.')
    return result


def _read_events(client, after, wait):
    delay = 1
    while True:
        try:
            batch = client.call('/api/admin/events?' + urllib.parse.urlencode({'after': after, 'wait': wait}),
                                timeout=max(30, wait + 10))
        except DeskError as error:
            if not error.transient:
                raise
            print(json.dumps({'connection': 'retrying', 'after': after, 'retryInSeconds': delay,
                              'error': str(error)}, ensure_ascii=False), file=sys.stderr, flush=True)
            time.sleep(delay)
            delay = min(10, delay * 2)
            continue
        if (not isinstance(batch, dict) or type(batch.get('lastId')) is not int or batch['lastId'] < max(0, after)
                or not isinstance(batch.get('events'), list) or not isinstance(batch.get('state'), dict)
                or any(not isinstance(event, dict) or not isinstance(event.get('kind'), str)
                       or not isinstance(event.get('payload'), dict) for event in batch['events'])):
            raise DeskError('The service returned an invalid event batch; the saved cursor is unchanged.')
        ids = [event.get('id') for event in batch['events']]
        if (any(type(identifier) is not int or identifier <= after for identifier in ids)
                or ids != sorted(set(ids)) or (ids and batch['lastId'] != ids[-1])
                or (not ids and after >= 0 and batch['lastId'] != after)):
            raise DeskError('The event IDs do not match the returned cursor; the saved cursor is unchanged.')
        for event in batch['events']:
            if event['kind'] == 'queue_changed':
                items = event['payload'].get('queue')
                if not isinstance(items, list) or any(not isinstance(item, dict) for item in items):
                    raise DeskError('The queue event is malformed; the saved cursor is unchanged.')
        return batch


def watch(client, after=None, cursor=None, wait=25, once=False, raw=False):
    if cursor and Path(cursor).exists():
        after = json.loads(Path(cursor).read_text())['lastId']
    if after is None:
        seed = _read_events(client, -1, 0)
        after = seed['lastId']
        write_json({'lastId': after, 'state': concise_state(seed['state'])})
        if cursor:
            write_json({'lastId': after}, cursor)
    while True:
        batch = _read_events(client, after, wait)
        withdrawn = set(batch['state'].get('withdrawnMessageIds', []))
        events = []
        for event in batch['events']:
            if event['kind'] == 'chat' and event['payload'].get('id') in withdrawn:
                continue
            if event['kind'] == 'queue_changed' and not raw:
                items = event['payload']['queue']
                event = {**event, 'payload': {'count': len(items), 'queue': items[:6]}}
            events.append(event)
        # Commit the cursor only after successful output, so a failed delivery can be replayed.
        write_json({**batch, 'events': events} if raw else {'events': events, 'lastId': batch['lastId'], 'state': concise_state(batch['state'])})
        after = batch['lastId']
        if cursor:
            write_json({'lastId': after}, cursor)
        if once:
            return


def song_identity(track):
    if not isinstance(track, dict) or not track.get('title') or not track.get('artist'):
        return None
    title = re.sub(r'\s*(?:[-–—]\s*|[([])(?:\d{4}\s+)?(?:remaster(?:ed)?|live|radio edit|album version|re-recorded)\b.*$',
                   '', track['title'], flags=re.IGNORECASE)
    def letters(value):
        value = ''.join(char for char in unicodedata.normalize('NFKD', value.casefold()) if not unicodedata.combining(char))
        return ''.join(char for char in value if char.isalnum())
    return letters(track['artist']), letters(title)


def add_track(client, uri, name='DJ', dedication='', front=False, allow_repeat=False, request_id=None):
    sync = client.sync()
    initial_queue, initial_current, initial_device = actual_queue(sync)
    if not allow_repeat and any(item.get('uri') == uri for item in sync.get('recentTracks', [])):
        raise DeskError('This recording already occurred in the observed set. Use --allow-repeat for an intentional repeat.')
    if not allow_repeat and sync.get('recentTracks'):
        candidate = client.call('/api/track?' + urllib.parse.urlencode({'uri': uri})).get('track')
        identity = song_identity(candidate)
        if identity is None:
            raise DeskError('Confirm the selected track artist and title before adding it.')
        if any(song_identity(item) == identity for item in sync['recentTracks']):
            raise DeskError('This song title and artist already occurred under another recording/version. Use --allow-repeat intentionally.')
    request_id = request_id or 'dj-' + uuid.uuid4().hex
    print(json.dumps({'requestId': request_id, 'uri': uri}), file=sys.stderr, flush=True)
    result = client.call('/api/admin/request', {'requestId': request_id, 'uri': uri,
                         'requestedBy': name, 'dedication': dedication})
    if not front:
        return result
    identifier = result['request']['id']
    deadline = time.monotonic() + 15
    initial_uids = [item['uid'] for item in initial_queue]
    changed_while_pending = False
    observed_new_uids = set()
    while time.monotonic() < deadline:
        synced = client.sync()
        queue, current, device = actual_queue(synced)
        remaining_initial = [item['uid'] for item in queue if item['uid'] in initial_uids]
        new_items = [item for item in queue if item['uid'] not in initial_uids]
        observed_new_uids.update(item['uid'] for item in new_items)
        if (remaining_initial != initial_uids or current['uid'] != initial_current['uid']
                or device['id'] != initial_device['id']
                or any(item.get('uri') != uri for item in new_items)
                or len(new_items) > 1
                or (len(new_items) == 1 and [item['uid'] for item in queue] != initial_uids + [new_items[0]['uid']])):
            changed_while_pending = True
        if current.get('requestId') == identifier or (synced['state'].get('nowPlaying') or {}).get('requestId') == identifier:
            return {**result, 'alreadyPlaying': True}
        requested = next((item for item in synced['state']['queue'] if item.get('id') == identifier), None)
        if requested and requested.get('uid') in {item['uid'] for item in queue}:
            requested_uid = requested['uid']
            original_rest = [item['uid'] for item in initial_queue if item['uid'] != requested_uid]
            observed_rest = [item['uid'] for item in queue if item['uid'] != requested_uid]
            if (changed_while_pending or observed_new_uids - {requested_uid}
                    or original_rest != observed_rest or current['uid'] != initial_current['uid']
                    or device['id'] != initial_device['id']):
                return {'request': requested, 'frontPending': True, 'queueChanged': True,
                        'note': 'The queue or current player changed while this request was pending. Inspect the new front before promoting it.'}
            plan = plan_order(synced, [requested['uid']])
            return {'request': requested, 'reorder': apply_order(client, plan)}
        if requested and requested.get('status') == 'uncertain':
            return {'request': requested, 'frontPending': True, 'note': 'Inspect the observed queue before any further write.'}
        time.sleep(.5)
    return {**result, 'frontPending': True, 'note': 'The request is saved; promote its actual UID once observed. Do not resubmit it.'}


def main(argv=None):
    parser = argparse.ArgumentParser(description='Audience-led DJ tools for the existing Spotify party service.')
    parser.add_argument('--url', default='http://127.0.0.1:8787')
    parser.add_argument('--data', type=Path, default=DEFAULT_DATA)
    parser.add_argument('--operator', help='Operator label recorded with control commands; defaults to QTRA_DJ_OPERATOR or virtual-dj.')
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('status').add_argument('--cached', action='store_true')
    sub.add_parser('history')
    sub.add_parser('inbox')
    sub.add_parser('queue')
    sub.add_parser('message-info').add_argument('id', type=int)
    p = sub.add_parser('presence', help='Assert actual agent availability once; never runs a heartbeat loop.')
    p.add_argument('state', choices=('online', 'offline'))
    p.add_argument('--agent-session', required=True); p.add_argument('--sequence', type=int, required=True)
    p.add_argument('--lease-seconds', type=int, default=60)
    sub.add_parser('composer', help='Read the existing native editor without changing its text.').add_argument('--save', type=Path)
    p = sub.add_parser('intent', help='Apply a search cue only to the exact saved native draft.')
    p.add_argument('intent', choices=('chat', 'music', 'auto')); p.add_argument('--draft', type=Path, required=True)
    p = sub.add_parser('work')
    p.add_argument('state', choices=('active', 'waiting', 'blocked', 'completed', 'cancelled'))
    p.add_argument('message_ids', nargs='+', type=int)
    p.add_argument('--summary', default=''); p.add_argument('--outcome', default='')
    p.add_argument('--evidence', action='append', default=[])
    p.add_argument('--lease-seconds', type=int, default=300)
    for command in ('front', 'order'):
        p = sub.add_parser(command)
        p.add_argument('uids', nargs='*')
        p.add_argument('--apply', action='store_true')
        p.add_argument('--save', type=Path)
    sub.add_parser('apply').add_argument('plan', type=Path)
    for command in ('search', 'describe', 'cue'):
        sub.add_parser(command).add_argument('text', nargs='+')
    p = sub.add_parser('resolve')
    p.add_argument('id'); p.add_argument('uris', nargs='*'); p.add_argument('--note', default='')
    p.add_argument('--matches', type=Path, help='JSON list of per-track match basis, rationale and source links.')
    p = sub.add_parser('add')
    p.add_argument('uri'); p.add_argument('--name', default='DJ'); p.add_argument('--dedication', default='')
    p.add_argument('--front', action='store_true'); p.add_argument('--allow-repeat', action='store_true')
    p.add_argument('--request-id')
    p = sub.add_parser('watch')
    p.add_argument('--after', type=int); p.add_argument('--cursor', type=Path)
    p.add_argument('--wait', type=int, default=25); p.add_argument('--once', action='store_true'); p.add_argument('--raw', action='store_true')
    p = sub.add_parser('visual')
    p.add_argument('theme', nargs='?'); p.add_argument('--uid'); p.add_argument('--clear', action='store_true')
    sub.add_parser('directions').add_argument('query', nargs='?', default='')
    args = parser.parse_args(argv)
    client = ServiceClient(args.url, args.data, args.operator)
    try:
        if args.command == 'directions':
            catalogue = json.loads((REPO / 'virtual-dj/seeds/music-directions.json').read_text())
            result = [item for item in catalogue['directions'] if args.query.casefold() in json.dumps(item, ensure_ascii=False).casefold()]
        elif args.command == 'status':
            state = client.call('/api/state') if args.cached else client.sync()['state']
            result = concise_state(state)
        elif args.command == 'history':
            result = client.sync()['recentTracks']
        elif args.command == 'queue':
            synced = client.sync()
            explicit, current, device = actual_queue(synced)
            result = {'spotify': synced['state']['spotify'], 'nowPlaying': current, 'device': device,
                      'explicitQueue': explicit,
                      'context': [item for item in synced['observation']['queue'] if item['source'] == 'context'],
                      'pending': concise_state(synced['state'])['pending']}
        elif args.command == 'inbox':
            result = client.call('/api/chat')
        elif args.command == 'message-info':
            result = client.call('/api/admin/message-info?' + urllib.parse.urlencode({'id': args.id}))
        elif args.command == 'presence':
            result = set_presence(client, args.state, args.agent_session, args.sequence, args.lease_seconds)
        elif args.command == 'composer':
            result = read_composer(args.data)
            if args.save:
                write_json(result, args.save)
        elif args.command == 'intent':
            result = set_search_intent(args.data, json.loads(args.draft.read_text(encoding='utf-8')), args.intent)
        elif args.command == 'work':
            result = client.call('/api/admin/work', {'messageIds': args.message_ids,
                'state': args.state, 'summary': args.summary, 'outcome': args.outcome,
                'evidence': args.evidence, 'leaseSeconds': args.lease_seconds})
        elif args.command in ('front', 'order'):
            result = plan_order(client.sync(), args.uids, front=args.command == 'front')
            if args.save:
                write_json(result, args.save)
            if args.apply:
                result = {'plan': result, 'result': apply_order(client, result)}
        elif args.command == 'apply':
            result = apply_order(client, json.loads(args.plan.read_text()))
        elif args.command == 'search':
            result = client.call('/api/search?' + urllib.parse.urlencode({'q': ' '.join(args.text)}))
        elif args.command == 'describe':
            result = client.call('/api/search/describe', {'guestId': 'dj-' + uuid.uuid4().hex,
                'requestId': uuid.uuid4().hex, 'text': ' '.join(args.text)})
        elif args.command == 'resolve':
            body = {'id': args.id, 'uris': args.uris, 'note': args.note}
            if args.matches:
                body['matches'] = json.loads(args.matches.read_text(encoding='utf-8'))
            result = client.call('/api/admin/resolve-search', body)
        elif args.command == 'cue':
            result = client.call('/api/admin/cue', {'text': ' '.join(args.text)})
        elif args.command == 'add':
            result = add_track(client, args.uri, args.name, args.dedication, args.front, args.allow_repeat, args.request_id)
        elif args.command == 'visual':
            if args.clear:
                visual = None
            else:
                if not args.theme:
                    raise DeskError('Choose a theme, or use --clear.')
                uid = args.uid or actual_queue(client.sync())[1]['uid']
                visual = {'trackUid': uid, 'theme': args.theme}
            result = client.call('/api/admin/scene', {'songVisual': visual})
        elif args.command == 'watch':
            watch(client, args.after, args.cursor, max(0, min(45, args.wait)), args.once, args.raw)
            return 0
        write_json(result)
        return 0
    except (DeskError, ValueError, OSError) as error:
        write_json(error.detail if isinstance(error, DeskError) else {'error': str(error)})
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == '__main__':
    raise SystemExit(main())
