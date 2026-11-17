"""Run the real bridge against isolated Spotify/Connect fixtures in Node."""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import unittest


BRIDGE = Path(os.environ.get('SPOTIFY_BRIDGE_PATH', Path(__file__).with_name('spotify_bridge.js'))).resolve()
NODE = os.environ.get('QTRA_TEST_NODE', 'node')
HARNESS = r'''
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const bridgeSource = fs.readFileSync(process.argv[1], 'utf8');
const plain = value => JSON.parse(JSON.stringify(value));
const queuedTrack = (uid, letter = 'b') => ({uid, uri: 'spotify:track:' + letter.repeat(22),
  provider: 'queue', type: 'track', metadata: {title: uid, is_queued: 'true'}});
async function fixture(options = {}) {
  let clock = 1000000;
  const calls = [];
  const track = {uri: 'spotify:track:' + 'a'.repeat(22), uid: 'occurrence-a',
    metadata: {title: 'Current track', artist_name: 'Current artist', duration: '127050'}};
  const state = {item: track, isPaused: false, isBuffering: false,
    timestamp: clock - 2000, positionAsOfTimestamp: 10000, duration: 127050, speed: 1, ...options.state};
  const raw = {track_window: {current_track: track}, paused: false, loading: false, disallows: {},
    timestamp: clock - 2000, position: 10000, duration: 127050, playback_speed: 1, ...options.raw};
  const devices = options.devices || [{id: 'standalone', name: 'Native Spotify', type: 'computer',
    local: false, is_active: true, is_controllable: true, is_observable: true}];
  const queue = {current: track, queued: options.queued || [], nextUp: options.context || []};
  const internal = {revision: 'revision-before', current_track: track,
    next_items: [...queue.queued, ...queue.nextUp], previous_items: [queuedTrack('previous', 'z')], ...options.internal};
  const syncQueue = () => {
    queue.current = internal.current_track;
    queue.queued = internal.next_items.filter(item => item.provider === 'queue' && !item.hidden_in_queue);
    queue.nextUp = internal.next_items.filter(item => item.provider !== 'queue' && !item.hidden_in_queue);
  };
  syncQueue();
  let rawReads = 0, deviceReads = 0, queueReads = 0, internalReads = 0, lastQueuePayload;
  const manager = {_currentPlayerState: {queue_revision: internal.revision, restrictions: options.restrictions || {}},
    setInternalPlayerQueue: async (payload, id, parameters) => {
      calls.push({action: 'setInternalPlayerQueue', id, parameters: plain(parameters), payload: plain(payload)});
      lastQueuePayload = payload;
      if (options.onQueueCommand) return options.onQueueCommand({payload, id, parameters, internal, queue, state, raw, devices, manager, syncQueue});
      assert.equal(payload.revision, internal.revision);
      internal.next_items = payload.next_items;
      internal.previous_items = payload.previous_items;
      internal.revision = 'revision-after';
      manager._currentPlayerState.queue_revision = internal.revision;
      syncQueue();
      return true;
    }};
  let api, harmony;
  const skipCalls = [];
  const advanceTo = item => {
    internal.current_track = item; state.item = item; raw.track_window.current_track = item;
    raw.position = 0; raw.timestamp = clock; raw.loading = false; syncQueue();
  };
  const executeSkip = async (action, id, parameters) => {
    calls.push({action, id, parameters: plain(parameters)});
    if (options.onSkipCommand) return options.onSkipCommand({action, id, parameters, state, raw,
      devices, harmony, internal, queue, syncQueue, advanceTo, clock});
    if (action === 'seek') { raw.position = 0; raw.timestamp = clock; }
    else advanceTo(action === 'nextTrack' ? internal.next_items.find(item => !item.hidden_in_queue)
      || queuedTrack('after-next') : internal.previous_items.at(-1));
    return true;
  };
  const execute = async (action, id, parameters) => {
    calls.push({action, id, parameters: plain(parameters)});
    if (options.onCommand) return options.onCommand({action, id, parameters, state, raw, devices, harmony});
    raw.paused = action === 'pause';
    raw.loading = false;
    raw.timestamp = clock;
    return true;
  };
  harmony = {_hasFakeState: !!options.fakeState, getDevices: async () => {
      if (options.onDevicesRead) options.onDevicesRead({harmony, state, raw, devices, internal, queue, read: ++deviceReads});
      return devices;
    },
    getCurrentState: async () => {
      if (options.onRawRead) options.onRawRead({harmony, state, raw, devices, internal, queue, syncQueue, read: ++rawReads});
      return structuredClone(raw);
    }, pause: (id, parameters) => execute('pause', id, parameters),
    resume: (id, parameters) => execute('resume', id, parameters),
    nextTrack: (id, parameters) => { skipCalls.push('next'); return executeSkip('nextTrack', id, parameters); },
    smartPreviousTrack: async (id, parameters) => {
      skipCalls.push('previous');
      // Match the inspected native Harmony decision, including its raw-position
      // threshold rather than a UI-extrapolated clock or an invented policy.
      const current = await harmony.getCurrentState();
      return current.disallows.seeking || current.position < 3000
        ? executeSkip('previousTrack', id, parameters) : executeSkip('seek', id, {position: 0});
    }};
  api = {_harmony: options.withHarmony === false ? undefined : harmony,
    _queue: {getInternalQueue: () => {
      if (options.onInternalRead) options.onInternalRead({internal, queue, state, raw, read: ++internalReads});
      return internal;
    }, _queueManager: manager},
    getState: () => structuredClone(state), getQueue: async () => {
      if (options.onQueueRead) options.onQueueRead({queue, internal, calls, harmony, raw, state, devices, syncQueue, read: ++queueReads});
      return structuredClone(queue);
    },
    addToQueue: async items => {
      calls.push({action: 'addToQueue'});
      if (options.onAddCommand) return options.onAddCommand({items, queue, internal, raw, state, harmony, syncQueue});
      return true;
    },
    skipToNext: async () => calls.push({action: 'next'}),
    pause: async () => { calls.push({action: 'wrapperPause'}); state.isPaused = true; },
    resume: async () => { calls.push({action: 'wrapperResume'}); state.isPaused = false; }};
  const connect = {getActiveDevice: () => options.observedDevice || devices.find(device => device.is_active)};
  const map = new Map([[Symbol('PlayerAPI'), api], [Symbol('ConnectDevicesAPI'), connect]]);
  const registry = {_map: map, resolve: symbol => map.get(symbol)};
  const location = {hostname: options.hostname || 'open.spotify.com', pathname: options.path || '/search/previous/tracks'};
  const catalogue = {query: 'previous', requestId: 'previous-response', tracks: [], mainText: '', visible: true, pushes: []};
  const showSearch = (query, tracks, requestId = 'response-' + query) => {
    catalogue.query = query; catalogue.tracks = tracks; catalogue.requestId = requestId; catalogue.mainText = '';
  };
  const platform = {getRegistry: () => registry, getSession: async () => ({isAnonymous: false}),
    getHistory: () => ({push: destination => {
      catalogue.pushes.push(destination); location.pathname = destination;
      if (options.onNavigate) options.onNavigate({destination, catalogue, showSearch, location});
    }})};
  const element = {'__reactFiber$fixture': {memoizedProps: {value: platform}}};
  const page = {'__reactFiber$search': {get memoizedProps() {
    return {results: {requestId: catalogue.requestId, tracks: {items: catalogue.tracks}}};
  }}};
  const row = {'__reactFiber$row': {get memoizedProps() { return {tracks: catalogue.tracks}; }}};
  const list = {getAttribute: () => `Search results for “${catalogue.query}”`,
    querySelector: () => catalogue.tracks.length ? row : null};
  const window = {__laricsDJ: options.previousBridge};
  const timerCalls = [];
  const context = {window, document: {querySelectorAll: () => [element],
      getElementById: id => id === 'searchPage' && catalogue.visible ? page : null,
      querySelector: selector => selector === '[data-testid="track-list"]' ? (catalogue.visible ? list : null)
        : selector === 'main' ? {innerText: catalogue.mainText} : null},
    location, Date: {now: () => clock}, setTimeout: (callback, ms) => {
      timerCalls.push(ms); clock += ms;
      if (options.onTimer) options.onTimer({catalogue, showSearch, location, clock});
      callback();
    }};
  const ready = await vm.runInNewContext(bridgeSource, context);
  assert.equal(ready.ready, true);
  return {bridge: window.__laricsDJ, calls, raw, state, devices, harmony, api, manager, queue, internal, syncQueue,
    lastQueuePayload: () => lastQueuePayload,
    advance: milliseconds => { clock += milliseconds; }, catalogue, showSearch, location, timerCalls, skipCalls};
}
'''


class BridgeTransportTests(unittest.TestCase):
    def run_js(self, script):
        source = HARNESS + '\n(async()=>{' + script + '\n})().catch(error=>{console.error(error.stack);process.exitCode=1;});'
        result = subprocess.run([NODE, '-e', source, str(BRIDGE)], capture_output=True,
                                text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_incremental_catalogue_steps_wait_without_timers_or_transport_commands(self):
        self.run_js(r'''
          const f = await fixture();
          const before = plain(await f.bridge.status());
          const id = 'catalogue-first';
          assert.deepEqual(plain(f.bridge.beginSearch('mirna pjesma', id)), {searchId: id, state: 'pending'});
          assert.equal(f.bridge.pollSearch(id).state, 'pending');
          assert.deepEqual(f.timerCalls, []);
          assert.deepEqual(plain(await f.bridge.status()), before);
          f.showSearch('mirna pjesma', [queuedTrack('one', 'b'), queuedTrack('two', 'b')]);
          const result = f.bridge.pollSearch(id);
          assert.equal(result.state, 'complete'); assert.equal(result.searchId, id);
          assert.deepEqual(plain(result.tracks.map(t => t.uid)), ['one', 'two']);
          assert.equal(f.bridge.pollSearch(id).state, 'cancelled');
          assert.deepEqual(f.catalogue.pushes, ['/search/mirna%20pjesma/tracks']);
          assert.deepEqual(f.calls, []);
        ''')

    def test_old_catalogue_result_or_cancel_cannot_replace_a_new_search(self):
        self.run_js(r'''
          const f = await fixture();
          f.bridge.beginSearch('first query', 'catalogue-old');
          f.bridge.beginSearch('second query', 'catalogue-new');
          assert.equal(f.bridge.cancelSearch('catalogue-old').cancelled, false);
          assert.equal(f.bridge.pollSearch('catalogue-old').state, 'cancelled');
          f.showSearch('first query', [queuedTrack('old', 'b')], 'response-old');
          assert.equal(f.bridge.pollSearch('catalogue-new').state, 'pending');
          f.showSearch('second query', [queuedTrack('new', 'c')], 'response-new');
          const current = f.bridge.pollSearch('catalogue-new');
          assert.equal(current.state, 'complete'); assert.equal(current.tracks[0].uid, 'new');
          assert.deepEqual(f.catalogue.pushes, ['/search/first%20query/tracks', '/search/second%20query/tracks']);
          assert.deepEqual(f.calls, []); assert.deepEqual(f.timerCalls, []);
        ''')

    def test_delayed_catalogue_begin_expires_before_navigation_and_preserves_newer_job(self):
        self.run_js(r'''
          const f = await fixture();
          f.bridge.beginSearch('current query', 'catalogue-current');
          const delayed = f.bridge.beginSearch('obsolete query', 'catalogue-delayed', 999999);
          assert.equal(delayed.state, 'cancelled');
          assert.equal(f.bridge.pollSearch('catalogue-current').state, 'pending');
          assert.deepEqual(f.catalogue.pushes, ['/search/current%20query/tracks']);
          assert.throws(() => f.bridge.beginSearch('bad clock', 'catalogue-clock', 1100000), /deadline/);
          assert.deepEqual(f.calls, []); assert.deepEqual(f.timerCalls, []);
        ''')

    def test_catalogue_cancellation_and_manual_navigation_do_not_navigate_back(self):
        self.run_js(r'''
          const f = await fixture();
          f.bridge.beginSearch('cancel me', 'catalogue-cancel');
          assert.equal(f.bridge.cancelSearch('unrelated-job').cancelled, false);
          assert.equal(f.bridge.cancelSearch('catalogue-cancel').cancelled, true);
          assert.equal(f.bridge.cancelSearch('catalogue-cancel').cancelled, false);
          f.showSearch('cancel me', [queuedTrack('late', 'b')]);
          assert.equal(f.bridge.pollSearch('catalogue-cancel').state, 'cancelled');
          f.bridge.beginSearch('manual route', 'catalogue-route');
          f.location.pathname = '/album/user-selected';
          assert.equal(f.bridge.pollSearch('catalogue-route').state, 'cancelled');
          assert.equal(f.location.pathname, '/album/user-selected');
          assert.equal(f.catalogue.pushes.length, 2);
          assert.deepEqual(f.calls, []); assert.deepEqual(f.timerCalls, []);
        ''')

    def test_catalogue_deadline_and_no_results_release_the_single_job(self):
        self.run_js(r'''
          const f = await fixture();
          f.bridge.beginSearch('never arrives', 'catalogue-expire');
          f.advance(16000);
          assert.equal(f.bridge.pollSearch('catalogue-expire').state, 'error');
          assert.equal(f.bridge.pollSearch('catalogue-expire').state, 'cancelled');
          f.bridge.beginSearch('no matching song', 'catalogue-empty');
          f.catalogue.mainText = 'No results found for no matching song';
          const empty = f.bridge.pollSearch('catalogue-empty');
          assert.equal(empty.state, 'complete'); assert.deepEqual(plain(empty.tracks), []);
          assert.equal(f.bridge.pollSearch('catalogue-empty').state, 'cancelled');
          assert.deepEqual(f.calls, []); assert.deepEqual(f.timerCalls, []);
        ''')

    def test_catalogue_identity_validation_and_cached_same_query_are_navigation_safe(self):
        self.run_js(r'''
          const f = await fixture({path: '/search/cached%20song/tracks'});
          f.showSearch('cached song', [queuedTrack('cached', 'b')]);
          for (const [query, id] of [['', 'catalogue-empty'], ['song', 'bad id'], ['song', null]]) {
            assert.throws(() => f.bridge.beginSearch(query, id), /query.*unique search ID/);
          }
          f.bridge.beginSearch('cached song', 'catalogue-cached');
          f.bridge.beginSearch('cached song', 'catalogue-cached');
          assert.throws(() => f.bridge.beginSearch('different text', 'catalogue-cached'), /another query/);
          const cached = f.bridge.pollSearch('catalogue-cached');
          assert.equal(cached.state, 'complete'); assert.equal(cached.tracks[0].uid, 'cached');
          assert.deepEqual(f.catalogue.pushes, []); assert.deepEqual(f.calls, []);
        ''')

    def test_legacy_catalogue_and_queue_metadata_fast_path_remain_compatible(self):
        self.run_js(r'''
          const found = queuedTrack('catalogue-found', 'b');
          const f = await fixture({onNavigate: ({destination, showSearch}) => {
            const query = decodeURIComponent(destination.split('/')[2]);
            showSearch(query, [found]);
          }});
          const known = await f.bridge.trackInQueue('spotify:track:' + 'a'.repeat(22));
          assert.equal(known.uid, 'occurrence-a'); assert.deepEqual(f.catalogue.pushes, []);
          assert.equal(await f.bridge.trackInQueue(found.uri), null);
          const legacy = await f.bridge.search('legacy query');
          assert.equal(legacy[0].uri, found.uri);
          const metadata = await f.bridge.track(found.uri);
          assert.equal(metadata.uri, found.uri);
          assert.equal(f.catalogue.pushes.length, 2);
          assert.deepEqual(f.calls, []);
        ''')

    def test_guarded_skip_next_confirms_new_occurrence_even_for_the_same_recording(self):
        self.run_js(r'''
const f = await fixture({queued: [queuedTrack('same-recording-next', 'a')]});
const result = await f.bridge.skip('next', 'occurrence-a', 'standalone');
assert.equal(result.ok, true); assert.equal(result.action, 'skip'); assert.equal(result.direction, 'next');
assert.equal(result.outcome, 'next'); assert.equal(result.changed, true);
assert.equal(result.fromUid, 'occurrence-a'); assert.equal(result.toUid, 'same-recording-next');
assert.equal(result.status.nowPlaying.uri, result.before.uri);
assert.equal(result.device.id, 'standalone'); assert.equal(result.status.playbackState.device.id, 'standalone');
assert.deepEqual(plain(result.before), {currentUid: 'occurrence-a', uri: 'spotify:track:' + 'a'.repeat(22),
  deviceId: 'standalone', positionMs: 12000, observedAt: 1000000, clockValid: true, buffering: false});
assert.equal(result.dispatchedAt, 1000000); assert.equal(result.status.observedAt, 1000000);
assert.deepEqual(f.skipCalls, ['next']);
assert.deepEqual(f.calls, [{action: 'nextTrack', id: 'standalone', parameters: {forceRemoteAction: true}}]);
        ''')

    def test_guarded_previous_preserves_native_threshold_seeking_rules_and_restart(self):
        self.run_js(r'''
for (const [position, disallows, outcome, command] of [
  [0, {}, 'previous', 'previousTrack'], [2999, {}, 'previous', 'previousTrack'],
  [3000, {}, 'restarted', 'seek'], [12000, {}, 'restarted', 'seek'],
  [12000, {seeking: true}, 'previous', 'previousTrack'],
  [12000, {skipping_prev: true}, 'restarted', 'seek']]) {
  const f = await fixture({raw: {position, disallows}});
  const result = await f.bridge.skip('previous', 'occurrence-a', 'standalone');
  assert.equal(result.ok, true); assert.equal(result.outcome, outcome);
  assert.equal(result.toUid, outcome === 'restarted' ? 'occurrence-a' : 'previous');
  assert.equal(result.status.nowPlaying.progressMs, 0);
  assert.deepEqual(f.skipCalls, ['previous']); assert.equal(f.calls.length, 1);
  assert.deepEqual(f.calls[0], {action: command, id: 'standalone',
    parameters: command === 'seek' ? {position: 0} : {forceRemoteAction: true}});
  if (outcome === 'restarted') {
    assert.ok(result.before.positionMs - result.status.nowPlaying.progressMs >= 1000);
    assert.ok(result.status.nowPlaying.progressMs <= result.status.observedAt - result.dispatchedAt + 1000);
  }
}
const paused = await fixture({raw: {position: 5000, paused: true}});
const restarted = await paused.bridge.skip('previous', 'occurrence-a', 'standalone');
assert.equal(restarted.outcome, 'restarted'); assert.equal(restarted.status.nowPlaying.paused, true);
        ''')

    def test_guarded_skip_rejects_invalid_preconditions_restrictions_and_unavailable_methods(self):
        self.run_js(r'''
for (const args of [['forward', 'occurrence-a', 'standalone'], ['next', '', 'standalone'],
  ['next', 'occurrence-a', ''], ['next', 'another', 'standalone'], ['next', 'occurrence-a', 'another']]) {
  const f = await fixture(); const result = await f.bridge.skip(...args);
  assert.ok(result.error); assert.equal(result.dispatched, false); assert.deepEqual(f.calls, []);
}
for (const [direction, options, remove] of [
  ['next', {fakeState: true}], ['next', {raw: {disallows: {skipping_next: true}}}],
  ['previous', {raw: {position: 1000, disallows: {skipping_prev: true}}}],
  ['previous', {raw: {position: 12000, disallows: {seeking: true, skipping_prev: true}}}],
  ['next', {raw: {loading: true}}], ['next', {raw: {position: undefined}, state: {positionAsOfTimestamp: undefined}}],
  ['next', {}, 'nextTrack'], ['previous', {}, 'smartPreviousTrack']]) {
  const f = await fixture(options); if (remove) delete f.harmony[remove];
  const result = await f.bridge.skip(direction, 'occurrence-a', 'standalone');
  assert.ok(result.error); assert.equal(result.dispatched, false);
  assert.deepEqual(f.calls, []); assert.deepEqual(f.skipCalls, []);
}
        ''')

    def test_guarded_skip_rechecks_current_and_device_immediately_before_dispatch(self):
        self.run_js(r'''
for (const options of [
  {onRawRead: ({read, raw, internal}) => {if (read === 3) {
    const changed = {...internal.current_track, uid: 'manual-current'};
    raw.track_window.current_track = changed; internal.current_track = changed;
  }}},
  {onDevicesRead: ({read, devices}) => {if (read === 2) devices[0].id = 'manual-device';}},
  {onDevicesRead: ({read, internal}) => {if (read === 2) internal.current_track = {...internal.current_track, uid: 'late-current'};}},
  {onDevicesRead: ({read, harmony}) => {if (read === 2) harmony._hasFakeState = true;}}
]) {
  const f = await fixture(options); const result = await f.bridge.skip('next', 'occurrence-a', 'standalone');
  assert.ok(result.error); assert.equal(result.dispatched, false); assert.deepEqual(f.calls, []);
}
        ''')

    def test_guarded_skip_ack_lost_response_and_post_dispatch_races_are_uncertain_once(self):
        self.run_js(r'''
for (const onSkipCommand of [() => false, () => {throw new Error('Lost response');}, () => true,
  ({advanceTo, devices}) => {advanceTo(queuedTrack('advanced')); devices[0].id = 'manual-device'; return true;},
  ({advanceTo, harmony}) => {advanceTo(queuedTrack('advanced')); harmony._hasFakeState = true; return true;}
]) {
  const f = await fixture({onSkipCommand});
  const result = await f.bridge.skip('next', 'occurrence-a', 'standalone');
  assert.ok(result.error); assert.equal(result.dispatched, true);
  assert.deepEqual(f.skipCalls, ['next']); assert.equal(f.calls.length, 1);
}
        ''')

    def test_guarded_previous_needs_a_real_backwards_jump_close_to_zero(self):
        self.run_js(r'''
for (const [position, after] of [[10000, 8500], [700, 0], [10000, 10000]]) {
  const f = await fixture({raw: {position, timestamp: 1000000},
    onSkipCommand: ({raw, clock}) => {raw.position = after; raw.timestamp = clock; return true;}});
  const result = await f.bridge.skip('previous', 'occurrence-a', 'standalone');
  assert.ok(result.error); assert.equal(result.dispatched, true);
  assert.deepEqual(f.skipCalls, ['previous']); assert.equal(f.calls.length, 1);
}
let f;
f = await fixture({onSkipCommand: ({raw}) => {
  f.advance(1500); raw.position = 1500; raw.timestamp = 1001500; return true;
}});
const delayed = await f.bridge.skip('previous', 'occurrence-a', 'standalone');
assert.equal(delayed.outcome, 'restarted'); assert.equal(delayed.status.nowPlaying.progressMs, 1500);
assert.equal(delayed.dispatchedAt, 1000000); assert.equal(delayed.status.observedAt, 1001500);
        ''')

    def test_guarded_skip_waits_through_buffering_but_never_reissues_the_command(self):
        self.run_js(r'''
const f = await fixture({onSkipCommand: ({advanceTo, raw}) => {
  advanceTo(queuedTrack('next-buffering')); raw.loading = true; return true;
}, onRawRead: ({raw, read}) => {if (read >= 5) raw.loading = false;}});
const result = await f.bridge.skip('next', 'occurrence-a', 'standalone');
assert.equal(result.outcome, 'next'); assert.equal(result.toUid, 'next-buffering');
assert.equal(result.status.nowPlaying.buffering, false); assert.equal(result.status.nowPlaying.clockValid, true);
assert.deepEqual(f.skipCalls, ['next']); assert.equal(f.calls.length, 1); assert.ok(f.timerCalls.length > 0);
        ''')

    def test_targeted_pause_and_resume_return_the_observed_device_state(self):
        self.run_js(r'''
const f = await fixture({raw: {position: 38912}});
const paused = await f.bridge.control('pause');
assert.deepEqual(f.calls, [{action: 'pause', id: 'standalone', parameters: {forceRemoteAction: true}}]);
assert.equal(paused.device.id, 'standalone');
assert.equal(paused.status.nowPlaying.progressMs, 38912);
assert.equal(paused.status.nowPlaying.paused, true);
f.advance(5000);
const resumed = await f.bridge.control('resume');
assert.equal(f.calls[1].action, 'resume');
assert.equal(f.calls[1].id, 'standalone');
assert.equal(resumed.status.nowPlaying.playing, true);
assert.equal(resumed.status.playbackState.source, 'harmony');
assert.equal(f.calls.length, 2);
''')

    def test_explicit_set_paused_dispatches_once_and_returns_actual_state(self):
        self.run_js(r'''
const f = await fixture({queued: [queuedTrack('queued')], raw: {position: 38912}});
const next = f.internal.next_items, previous = f.internal.previous_items, current = f.internal.current_track;
for (const paused of [true, false]) {
  const result = await f.bridge.setPaused(paused, 'occurrence-a', 'standalone');
  assert.equal(result.ok, true);
  assert.equal(result.action, 'setPaused');
  assert.equal(result.paused, paused);
  assert.equal(result.changed, true);
  assert.equal(result.status.nowPlaying.paused, paused);
  assert.equal(result.status.nowPlaying.uid, 'occurrence-a');
  assert.equal(result.status.playbackState.device.id, 'standalone');
  assert.equal(result.status.playbackState.clockValid, true);
}
assert.deepEqual(f.calls, [
  {action: 'pause', id: 'standalone', parameters: {forceRemoteAction: true}},
  {action: 'resume', id: 'standalone', parameters: {forceRemoteAction: true}}]);
assert.equal(f.internal.next_items, next);
assert.equal(f.internal.previous_items, previous);
assert.equal(f.internal.current_track, current);
''')

    def test_explicit_set_paused_no_op_resamples_clock_and_does_not_dispatch(self):
        self.run_js(r'''
for (const paused of [true, false]) {
  const f = await fixture({raw: {paused}, onRawRead: ({raw, read}) => {
    if (read === 2) { raw.position = 34000; raw.timestamp = 1000000; }
  }});
  const result = await f.bridge.setPaused(paused, 'occurrence-a', 'standalone');
  assert.equal(result.ok, true);
  assert.equal(result.changed, false);
  assert.equal(result.status.nowPlaying.progressMs, 34000);
  assert.equal(result.status.nowPlaying.paused, paused);
  assert.equal(f.calls.length, 0);
}
''')

    def test_explicit_set_paused_rejects_missing_or_mismatched_preconditions(self):
        self.run_js(r'''
for (const arguments_ of [
  [undefined, 'occurrence-a', 'standalone'], [null, 'occurrence-a', 'standalone'],
  ['true', 'occurrence-a', 'standalone'], [1, 'occurrence-a', 'standalone'],
  [true, undefined, 'standalone'], [true, '', 'standalone'], [true, ' ', 'standalone'],
  [true, 'occurrence-a', null], [true, 'occurrence-a', ' '],
  [true, 'other-occurrence', 'standalone'], [true, 'occurrence-a', 'other-device']
]) {
  const f = await fixture();
  const result = await f.bridge.setPaused(...arguments_);
  assert.equal(result.dispatched, false, JSON.stringify(arguments_));
  assert.equal(typeof result.error, 'string');
  assert.equal(f.calls.length, 0);
}
''')

    def test_explicit_set_paused_refuses_fake_missing_pause_buffering_or_invalid_clock(self):
        self.run_js(r'''
for (const options of [
  {fakeState: true}, {withHarmony: false},
  {raw: {paused: undefined}}, {raw: {paused: 0}},
  {raw: {position: undefined}}, {raw: {position: NaN}},
  {raw: {position: -1}}, {raw: {loading: true}},
  {raw: {timestamp: 1002000}},
  {raw: {timestamp: undefined}, state: {timestamp: undefined}},
  {observedDevice: {id: 'wrong-observed-device'}},
  {devices: [{id: 'standalone', is_active: true, is_controllable: false, is_observable: true}]},
  {devices: []}
]) {
  const f = await fixture(options);
  const result = await f.bridge.setPaused(true, 'occurrence-a', 'standalone');
  assert.equal(result.dispatched, false, JSON.stringify(options));
  assert.equal(f.calls.length, 0);
}
// A held zero-position clock remains real even without a source timestamp.
const held = await fixture({raw: {paused: true, position: 0, timestamp: undefined}, state: {timestamp: undefined}});
const result = await held.bridge.setPaused(true, 'occurrence-a', 'standalone');
assert.equal(result.changed, false);
assert.equal(result.status.nowPlaying.progressMs, 0);
assert.equal(held.calls.length, 0);
''')

    def test_explicit_set_paused_rechecks_current_device_and_state_before_dispatch_or_no_op(self):
        self.run_js(r'''
for (const paused of [true, false]) {
  for (const race of ['raw-current', 'internal-current', 'device', 'fake', 'buffering']) {
    const f = await fixture({onRawRead: ({raw, read}) => {
      if (read === 2 && race === 'raw-current') raw.track_window.current_track = {...raw.track_window.current_track, uid: 'new-occurrence-same-uri'};
      if (read === 2 && race === 'buffering') raw.loading = true;
    }, onDevicesRead: ({devices, harmony, internal, read}) => {
      if (read !== 2) return;
      if (race === 'internal-current') internal.current_track = {...internal.current_track, uid: 'new-occurrence-same-uri'};
      if (race === 'device') devices[0].id = 'new-device';
      if (race === 'fake') harmony._hasFakeState = true;
    }});
    const result = await f.bridge.setPaused(paused, 'occurrence-a', 'standalone');
    assert.equal(result.dispatched, false, race);
    assert.equal(f.calls.length, 0, race);
  }
}
''')

    def test_explicit_set_paused_handles_a_matching_manual_change_as_a_fresh_no_op(self):
        self.run_js(r'''
const f = await fixture({onRawRead: ({raw, read}) => {
  if (read === 2) { raw.paused = true; raw.position = 25000; }
}});
const result = await f.bridge.setPaused(true, 'occurrence-a', 'standalone');
assert.equal(result.changed, false);
assert.equal(result.status.nowPlaying.progressMs, 25000);
assert.equal(f.calls.length, 0);
''')

    def test_explicit_set_paused_keeps_ack_failure_and_unconfirmed_response_dispatched(self):
        self.run_js(r'''
for (const outcome of ['rejected', 'lost', 'unconfirmed', 'current-changed', 'device-changed', 'fake']) {
  const f = await fixture({onCommand: ({raw, devices, harmony}) => {
    if (outcome === 'rejected') return false;
    if (outcome === 'lost') throw new Error('Connection lost after dispatch');
    if (outcome === 'current-changed') raw.track_window.current_track = {...raw.track_window.current_track, uid: 'new-same-uri'};
    if (outcome === 'device-changed') devices[0].id = 'new-device';
    if (outcome === 'fake') harmony._hasFakeState = true;
    return true;
  }});
  const result = await f.bridge.setPaused(true, 'occurrence-a', 'standalone');
  assert.equal(result.dispatched, true, outcome);
  assert.equal(typeof result.error, 'string');
  assert.equal(f.calls.length, 1, outcome);
  assert.equal(f.calls[0].action, 'pause');
}
''')

    def test_explicit_set_paused_waits_for_real_confirmation_through_transient_buffering(self):
        self.run_js(r'''
let dispatched = false;
const f = await fixture({onCommand: ({raw}) => {
  dispatched = true; raw.paused = true; raw.loading = true; return true;
}, onRawRead: ({raw, read}) => {
  if (dispatched && read >= 4) { raw.loading = false; raw.position = 45000; }
}});
const result = await f.bridge.setPaused(true, 'occurrence-a', 'standalone');
assert.equal(result.changed, true);
assert.equal(result.status.nowPlaying.progressMs, 45000);
assert.equal(result.status.nowPlaying.buffering, false);
assert.equal(f.calls.length, 1);
''')

    def test_raw_paused_position_and_zero_are_preserved_without_elapsed_time(self):
        self.run_js(r'''
for (const position of [38912, 0]) {
  const f = await fixture({state: {positionAsOfTimestamp: 99999, duration: 0},
    raw: {paused: true, position, timestamp: 900000}});
  const status = await f.bridge.status();
  assert.equal(status.nowPlaying.progressMs, position);
  assert.equal(status.nowPlaying.durationMs, 127050);
  assert.equal(status.nowPlaying.playing, false);
  assert.equal(status.playbackState.sourceTimestamp, 900000);
}
''')

    def test_buffering_and_optimistic_page_pause_do_not_replace_raw_playback(self):
        self.run_js(r'''
const playing = await fixture({state: {isPaused: true, positionAsOfTimestamp: 0}, raw: {position: 15000}});
let status = await playing.bridge.status();
assert.equal(status.nowPlaying.playing, true);
assert.equal(status.nowPlaying.progressMs, 17000);
const buffering = await fixture({state: {isPaused: true}, raw: {paused: false, loading: true, position: 22000}});
status = await buffering.bridge.status();
assert.equal(status.nowPlaying.paused, false);
assert.equal(status.nowPlaying.buffering, true);
assert.equal(status.nowPlaying.playing, false);
assert.equal(status.nowPlaying.progressMs, 22000);
''')

    def test_a_command_ack_without_raw_confirmation_is_not_success(self):
        self.run_js(r'''
const f = await fixture({onCommand: ({state}) => {state.isPaused = true; state.positionAsOfTimestamp = 0; return true;}});
await assert.rejects(() => f.bridge.control('pause'), /state did not confirm/);
assert.equal(f.calls.length, 1);
assert.equal(f.calls[0].id, 'standalone');
const rejected = await fixture({onCommand: () => false});
await assert.rejects(() => rejected.bridge.control('pause'), /did not accept/);
assert.equal(rejected.calls.length, 1);
''')

    def test_ambiguous_restricted_and_fake_devices_cannot_dispatch(self):
        self.run_js(r'''
const good = {id: 'one', is_active: true, is_controllable: true, is_observable: true};
for (const devices of [[], [good, {...good, id: 'two'}], [{...good, is_controllable: false}], [{...good, is_observable: false}]]) {
  const f = await fixture({devices});
  await assert.rejects(() => f.bridge.control('pause'));
  assert.equal(f.calls.length, 0);
}
for (const options of [{fakeState: true}, {onRawRead: ({harmony}) => {harmony._hasFakeState = true;}}]) {
  const f = await fixture(options);
  await assert.rejects(() => f.bridge.control('pause'), /synthetic local state/);
  assert.equal(f.calls.length, 0);
}
const incomplete = await fixture({raw: {position: NaN}});
await assert.rejects(() => incomplete.bridge.control('pause'), /complete playback clock/);
assert.equal(incomplete.calls.length, 0);
''')

    def test_device_change_is_not_retried_on_the_new_device(self):
        self.run_js(r'''
const f = await fixture({onCommand: ({raw, devices}) => {
  raw.paused = true;
  devices[0].is_active = false;
  devices.push({...devices[0], id: 'manual-new-device', is_active: true});
  return true;
}});
await assert.rejects(() => f.bridge.control('pause'), /device changed/);
assert.equal(f.calls.length, 1);
assert.equal(f.calls[0].id, 'standalone');
''')

    def test_pre_dispatch_device_change_sends_no_command(self):
        self.run_js(r'''
for (const observedDevice of [undefined, {id: 'standalone', name: 'Old cached device'}]) {
  const f = await fixture({observedDevice, onRawRead: ({devices}) => {devices[0].id = 'manual-new-device';}});
  await assert.rejects(() => f.bridge.control('pause'), /device changed before transport dispatch/);
  assert.equal(f.calls.length, 0);
}
''')

    def test_browser_synthetic_state_cannot_be_reported_as_the_current_player(self):
        self.run_js(r'''
const f = await fixture({fakeState: true});
await assert.rejects(() => f.bridge.status(), /synthetic local state/);
assert.equal(f.calls.length, 0);
f.harmony._hasFakeState = false;
assert.equal((await f.bridge.status()).playbackState.source, 'harmony');
''')

    def test_same_uri_new_occurrence_does_not_reuse_the_previous_clock(self):
        self.run_js(r'''
const repeated = {uri: 'spotify:track:' + 'a'.repeat(22), uid: 'second-occurrence',
  metadata: {title: 'Same recording', duration: '127050'}};
const f = await fixture({state: {timestamp: 1000, positionAsOfTimestamp: 99000},
  raw: {track_window: {current_track: repeated}, position: 1000, timestamp: undefined}});
const status = await f.bridge.status();
assert.equal(status.nowPlaying.uid, 'second-occurrence');
assert.equal(status.nowPlaying.progressMs, 1000);
assert.equal(status.playbackState.sourceTimestamp, status.observedAt);
assert.equal(status.nowPlaying.clockValid, false);
assert.equal(status.playbackState.clockValid, false);
assert.equal(status.nowPlaying.title, 'Current track'); // Same-URI metadata is reusable.
''')

    def test_clock_validity_distinguishes_real_zero_from_missing_or_invalid_position(self):
        self.run_js(r'''
for (const position of [undefined, null, true, NaN, Infinity, -1]) {
  const f = await fixture({raw: {position}, state: {positionAsOfTimestamp: position, position}});
  const status = await f.bridge.status();
  assert.equal(status.nowPlaying.clockValid, false, String(position));
  assert.equal(status.playbackState.clockValid, false);
  assert.ok(Number.isFinite(status.nowPlaying.progressMs));
}
const zero = await fixture({raw: {position: 0, timestamp: 1000000}});
const status = await zero.bridge.status();
assert.equal(status.nowPlaying.progressMs, 0);
assert.equal(status.nowPlaying.clockValid, true);
assert.equal(status.playbackState.clockValid, true);
assert.equal(zero.calls.length, 0);
''')

    def test_playing_clock_needs_a_real_positive_source_timestamp(self):
        self.run_js(r'''
for (const timestamp of [undefined, null, true, NaN, Infinity, 0, -1]) {
  const f = await fixture({raw: {timestamp}, state: {timestamp}});
  const status = await f.bridge.status();
  assert.equal(status.nowPlaying.clockValid, false, String(timestamp));
  assert.equal(status.playbackState.clockValid, false);
  assert.equal(status.nowPlaying.playing, true); // Existing transport fields stay compatible.
}
const fallback = await fixture({raw: {timestamp: undefined}, state: {timestamp: 999000}});
const status = await fallback.bridge.status();
assert.equal(status.nowPlaying.clockValid, true);
assert.equal(status.nowPlaying.progressMs, 11000);
assert.equal(status.playbackState.sourceTimestamp, 999000);
''')

    def test_future_playing_anchors_outside_clock_tolerance_are_invalid(self):
        self.run_js(r'''
for (const withHarmony of [true, false]) {
  for (const offset of [0, 1000, 1001, 3600000]) {
    const timestamp = 1000000 + offset;
    const f = await fixture({withHarmony, raw: {timestamp}, state: {timestamp}});
    const status = await f.bridge.status();
    assert.equal(status.nowPlaying.clockValid, offset <= 1000);
    assert.equal(status.playbackState.clockValid, offset <= 1000);
    assert.equal(status.playbackState.sourceTimestamp, timestamp);
    assert.equal(status.nowPlaying.progressMs, 10000);
  }
}
for (const held of [{paused: true}, {loading: true}]) {
  const f = await fixture({raw: {...held, timestamp: 4600000}});
  const status = await f.bridge.status();
  assert.equal(status.nowPlaying.clockValid, true);
  assert.equal(status.nowPlaying.progressMs, 10000);
}
''')

    def test_explicitly_held_clocks_need_position_but_no_timestamp(self):
        self.run_js(r'''
for (const buffering of [false, true]) {
  for (const withHarmony of [false, true]) {
    const f = await fixture({withHarmony,
      state: {isPaused: !buffering, isBuffering: buffering, positionAsOfTimestamp: 0, timestamp: undefined},
      raw: {paused: !buffering, loading: buffering, position: 0, timestamp: undefined}});
    let status = await f.bridge.status();
    assert.equal(status.nowPlaying.clockValid, true);
    assert.equal(status.playbackState.clockValid, true);
    assert.equal(status.nowPlaying.progressMs, 0);
    f.advance(6000);
    status = await f.bridge.status();
    assert.equal(status.nowPlaying.progressMs, 0);
    assert.equal(status.nowPlaying.clockValid, true);
    f.state.positionAsOfTimestamp = undefined;
    f.raw.position = undefined;
    assert.equal((await f.bridge.status()).nowPlaying.clockValid, false);
  }
}
''')

    def test_native_direct_control_and_next_keep_their_existing_paths(self):
        self.run_js(r'''
const native = await fixture({hostname: 'xpui', withHarmony: false, state: {positionAsOfTimestamp: 24000}});
const result = await native.bridge.control('pause');
assert.deepEqual(native.calls, [{action: 'wrapperPause'}]);
assert.equal(result.status.nowPlaying.progressMs, 24000);
assert.equal(result.status.nowPlaying.paused, true);
const web = await fixture({withHarmony: false});
await assert.rejects(() => web.bridge.control('pause'), /no targeted transport/);
assert.equal(web.calls.length, 0);
await web.bridge.control('next');
assert.deepEqual(web.calls, [{action: 'next'}]);
''')

    def test_raw_track_identity_and_existing_queue_idempotence_survive_reload(self):
        self.run_js(r'''
const operations = new Map([['existing-request', Promise.resolve({queued: true})]]);
const other = {uri: 'spotify:track:' + 'b'.repeat(22), uid: 'new-occurrence', metadata: {title: 'Actual new track', duration: '90000'}};
const f = await fixture({previousBridge: {version: 8, operations},
  raw: {track_window: {current_track: other}, position: 1000, duration: 90000}});
const status = await f.bridge.status();
assert.equal(status.nowPlaying.uri, other.uri);
assert.equal(status.nowPlaying.title, 'Actual new track');
assert.equal(status.nowPlaying.uid, 'new-occurrence');
assert.equal(f.bridge.operations, operations);
assert.equal((await f.bridge.enqueue(other.uri, 'existing-request')).queued, true);
assert.equal(f.calls.length, 0);
const transition = await fixture({state: {duration: 127050, timestamp: 1000},
  raw: {track_window: {current_track: other}, position: 1000, duration: 0, timestamp: undefined}});
const current = await transition.bridge.status();
assert.equal(current.nowPlaying.durationMs, 90000);
assert.equal(current.nowPlaying.progressMs, 1000);
assert.equal(current.playbackState.sourceTimestamp, current.observedAt);
const incomplete = await fixture({raw: {track_window: {current_track: other}, position: NaN}});
await assert.rejects(() => incomplete.bridge.status(), /changed playback occurrences before a complete clock/);
''')

    def test_enqueue_does_not_confirm_the_recording_that_was_already_current(self):
        self.run_js(r'''
const f = await fixture();
const uri = f.queue.current.uri;
const result = await f.bridge.enqueue(uri, 'same-current-request');
assert.equal(result.dispatched, true);
assert.match(result.error, /new requested occurrence was not confirmed/);
assert.equal(f.calls.length, 1);
assert.deepEqual(plain(await f.bridge.enqueue(uri, 'same-current-request')), plain(result));
assert.equal(f.calls.length, 1, 'uncertain result was replayed');
''')

    def test_enqueue_confirms_the_new_queued_uid_among_repeated_recordings(self):
        self.run_js(r'''
const f = await fixture({queued: [queuedTrack('existing-one', 'a'), queuedTrack('existing-two', 'a')],
  onAddCommand: ({internal, syncQueue}) => {
    internal.next_items.push(queuedTrack('actual-new-request', 'a')); syncQueue(); return true;
  }});
const result = await f.bridge.enqueue(f.queue.current.uri, 'new-request');
assert.deepEqual(plain(result), {queued: true, uri: f.queue.current.uri, requestId: 'new-request', uid: 'actual-new-request'});
assert.equal(f.calls.length, 1);
await f.bridge.enqueue(f.queue.current.uri, 'new-request');
assert.equal(f.calls.length, 1);
''')

    def test_enqueue_confirms_a_new_occurrence_that_started_before_the_queue_read(self):
        self.run_js(r'''
const f = await fixture({onAddCommand: ({internal, syncQueue}) => {
  internal.current_track = queuedTrack('new-current-occurrence', 'a'); syncQueue(); return true;
}});
const result = await f.bridge.enqueue(f.queue.current.uri, 'fast-start-request');
assert.equal(result.queued, true);
assert.equal(result.uid, 'new-current-occurrence');
assert.equal(f.calls.length, 1);
''')

    def test_enqueue_rejects_false_ack_lost_ack_and_malformed_after_queue_without_replay(self):
        self.run_js(r'''
for (const outcome of ['false', 'lost', 'malformed', 'ambiguous-new-uids']) {
  const f = await fixture({onAddCommand: ({queue, internal, syncQueue}) => {
    if (outcome === 'false') return false;
    if (outcome === 'lost') throw new Error('Lost queue acknowledgement');
    if (outcome === 'malformed') queue.queued = null;
    if (outcome === 'ambiguous-new-uids') {
      internal.next_items.push(queuedTrack('new-one', 'a'), queuedTrack('new-two', 'a')); syncQueue();
    }
    return true;
  }});
  const uri = f.queue.current.uri;
  const result = await f.bridge.enqueue(uri, 'uncertain-request');
  assert.equal(result.dispatched, true, outcome);
  assert.equal(typeof result.error, 'string');
  await f.bridge.enqueue(uri, 'uncertain-request');
  assert.equal(f.calls.length, 1, outcome);
}
''')

    def test_enqueue_cannot_claim_an_existing_queued_occurrence_or_late_current_uid(self):
        self.run_js(r'''
for (const kind of ['previously-queued', 'previously-unidentified']) {
  const f = await fixture({queued: kind === 'previously-queued' ? [queuedTrack('already-queued', 'a')] : [],
    onAddCommand: ({internal, syncQueue}) => {
      internal.current_track = queuedTrack(kind === 'previously-queued' ? 'already-queued' : 'late-known-uid', 'a');
      internal.next_items = []; syncQueue(); return true;
    }});
  if (kind === 'previously-unidentified') f.queue.current = {...f.queue.current, uid: null};
  const result = await f.bridge.enqueue(f.queue.current.uri, 'different-occurrence-required');
  assert.equal(result.dispatched, true, kind);
  assert.equal(f.calls.length, 1);
}
''')

    def test_enqueue_synthetic_preflight_is_definitely_unsent_even_after_queue_read(self):
        self.run_js(r'''
for (const duringRead of [false, true]) {
  const f = await fixture({fakeState: !duringRead,
    onQueueRead: ({harmony, read}) => { if (duringRead && read === 1) harmony._hasFakeState = true; }});
  const result = await f.bridge.enqueue(f.queue.current.uri, 'fake-state-request');
  assert.equal(result.dispatched, false);
  assert.match(result.error, /synthetic local state/);
  assert.equal(f.calls.length, 0);
  assert.equal(f.bridge.operations.has('fake-state-request'), false);
}
''')

    def test_queue_move_retains_duplicate_recordings_and_full_context_history(self):
        self.run_js(r'''
const first = queuedTrack('first', 'b'), other = queuedTrack('other', 'c'), repeated = queuedTrack('repeat', 'b');
const context = Array.from({length: 14}, (_, n) => ({uid: 'context-' + (n % 3), uri: 'spotify:track:' + 'd'.repeat(22), provider: 'context', metadata: {iteration: n}}));
context.splice(5, 0, {uid: 'delimiter0', uri: 'spotify:delimiter', provider: 'context'});
const f = await fixture({queued: [first, other, repeated], context});
const oldCurrent = f.internal.current_track, oldPrevious = f.internal.previous_items;
const result = await f.bridge.moveQueue('repeat', 'first', ['first', 'other', 'repeat']);
assert.equal(result.ok, true);
assert.equal(result.moved, true);
assert.deepEqual(plain(result.status.queue.filter(t => t.source === 'queue').map(t => t.uid)), ['repeat', 'first', 'other']);
assert.equal(result.status.nowPlaying.uid, oldCurrent.uid);
assert.equal(result.status.nowPlaying.playing, true);
assert.equal(result.device.id, 'standalone');
assert.equal(f.calls.length, 1);
assert.equal(f.calls[0].action, 'setInternalPlayerQueue');
assert.equal(f.calls[0].id, 'standalone');
assert.deepEqual(f.calls[0].parameters, {ignoreRevision: false});
assert.equal(f.calls[0].payload.revision, 'revision-before');
const sent = f.lastQueuePayload();
assert.equal(sent.next_items[0], repeated);
assert.equal(sent.next_items[1], first);
assert.equal(sent.next_items[2], other);
context.forEach((item, i) => assert.equal(sent.next_items[3+i], item));
assert.equal(sent.previous_items, oldPrevious);
assert.equal(f.internal.current_track, oldCurrent);
assert.equal(sent.next_items.length, 3 + context.length);
''')

    def test_queue_move_rejects_stale_order_missing_and_nonunique_occurrences(self):
        self.run_js(r'''
const normal = [queuedTrack('one'), queuedTrack('two'), queuedTrack('three')];
const cases = [
  {items: normal, uid: 'three', anchor: 'one', expected: ['two', 'one', 'three']},
  {items: normal, uid: 'missing', anchor: 'one', expected: ['one', 'two', 'three']},
  {items: normal, uid: 'three', anchor: 'missing', expected: ['one', 'two', 'three']},
  {items: [normal[0], {...normal[1], uid: null}, normal[2]], uid: 'three', anchor: 'one', expected: ['one', 'two', 'three']},
  {items: [normal[0], normal[1], {...normal[2], uid: 'two'}], uid: 'two', anchor: 'one', expected: ['one', 'two']},
  {items: [normal[0], {...normal[1], uid: 'one'}, normal[2]], uid: 'three', anchor: 'one', expected: ['one', 'three']},
  {items: [...normal, {...normal[2], hidden_in_queue: true}], uid: 'three', anchor: 'one', expected: ['one', 'two', 'three']},
  {items: normal, uid: 'one', anchor: 'one', expected: ['one', 'two', 'three']},
  {items: normal, uid: 'three', anchor: 'one', expected: ['one', 'two', 'two']}
];
for (const row of cases) {
  const f = await fixture({queued: row.items});
  const result = await f.bridge.moveQueue(row.uid, row.anchor, row.expected);
  assert.equal(typeof result.error, 'string', JSON.stringify(row));
  assert.equal(result.dispatched, false);
  assert.equal(f.calls.length, 0);
}
''')

    def test_queue_move_requires_real_targeted_capability_and_restriction_checks(self):
        self.run_js(r'''
const queued = [queuedTrack('one'), queuedTrack('two')];
for (const options of [{fakeState: true}, {devices: []}, {withHarmony: false},
    {devices: [{id: 'one', is_active: true, is_controllable: false, is_observable: true}]},
    {restrictions: {disallow_set_queue_reasons: ['restricted']}},
    {observedDevice: {id: 'different-device'}}]) {
  const f = await fixture({queued, ...options});
  const result = await f.bridge.moveQueue('two', 'one', ['one', 'two']);
  assert.equal(result.dispatched, false);
  assert.equal(typeof result.error, 'string');
  assert.equal(f.calls.length, 0);
}
const unavailable = await fixture({queued});
delete unavailable.api._queue._queueManager.setInternalPlayerQueue;
const result = await unavailable.bridge.moveQueue('two', 'one', ['one', 'two']);
assert.match(result.error, /revision-checked, targeted/);
assert.equal(result.dispatched, false);
assert.equal(unavailable.calls.length, 0);
''')

    def test_queue_move_rechecks_revision_current_full_order_and_device_before_dispatch(self):
        self.run_js(r'''
const mutations = [
  f => {f.internal.revision = 'concurrent-revision';},
  f => {f.internal.next_items.reverse(); f.syncQueue();},
  f => {f.internal.previous_items = [queuedTrack('new-history')];},
  f => {f.internal.current_track = {...f.internal.current_track, uid: 'new-occurrence'}; f.raw.track_window.current_track = f.internal.current_track;},
  f => {f.devices[0].id = 'new-device';},
  f => {f.harmony._hasFakeState = true;}
];
for (const mutate of mutations) {
  const f = await fixture({queued: [queuedTrack('one'), queuedTrack('two')],
    onRawRead: state => {if (state.read === 2) mutate(state);}});
  const result = await f.bridge.moveQueue('two', 'one', ['one', 'two']);
  assert.equal(result.dispatched, false);
  assert.equal(typeof result.error, 'string');
  assert.equal(f.calls.length, 0);
}
// A context-only reorder with an unchanged explicit queue must also abort.
const context = [0, 1].map(n => ({uid: 'ctx' + n, uri: 'spotify:track:' + 'c'.repeat(22), provider: 'context'}));
const f = await fixture({queued: [queuedTrack('one'), queuedTrack('two')], context,
  onRawRead: state => {if (state.read === 2) state.internal.next_items.splice(2, 2, context[1], context[0]);}});
assert.equal((await f.bridge.moveQueue('two', 'one', ['one', 'two'])).dispatched, false);
assert.equal(f.calls.length, 0);
''')

    def test_queue_move_acknowledgement_requires_observed_order_and_never_retries(self):
        self.run_js(r'''
for (const onQueueCommand of [() => true, () => false, () => {throw new Error('revision is stale');}]) {
  const f = await fixture({queued: [queuedTrack('one'), queuedTrack('two')], onQueueCommand});
  const result = await f.bridge.moveQueue('two', 'one', ['one', 'two']);
  assert.equal(result.dispatched, true);
  assert.equal(typeof result.error, 'string');
  assert.equal(f.calls.length, 1);
  assert.deepEqual(f.queue.queued.map(t => t.uid), ['one', 'two']);
}
const delayed = await fixture({queued: [queuedTrack('one'), queuedTrack('two')],
  onQueueCommand: () => true,
  onQueueRead: state => {if (state.calls.length && state.read === 4) {state.internal.next_items.reverse(); state.syncQueue();}}});
const confirmed = await delayed.bridge.moveQueue('two', 'one', ['one', 'two']);
assert.equal(confirmed.ok, true);
assert.equal(delayed.calls.length, 1);
assert.ok(confirmed.status.observedAt > 1000000);
''')

    def test_queue_move_reports_post_dispatch_concurrency_without_a_second_command(self):
        self.run_js(r'''
for (const change of [
  state => {state.devices[0].id = 'new-device';},
  state => {state.internal.next_items.push(queuedTrack('new-occurrence')); state.syncQueue();},
  state => {state.internal.current_track = {...state.internal.current_track, uid: 'new-current'};
    state.raw.track_window.current_track = state.internal.current_track; state.syncQueue();},
  state => {state.internal.previous_items = [];}]) {
  const f = await fixture({queued: [queuedTrack('one'), queuedTrack('two')],
    onQueueCommand: state => {change(state); return true;}});
  const result = await f.bridge.moveQueue('two', 'one', ['one', 'two']);
  assert.equal(result.dispatched, true);
  assert.equal(typeof result.error, 'string');
  assert.equal(f.calls.length, 1);
}
''')

    def test_queue_move_already_satisfied_order_is_a_command_free_observation(self):
        self.run_js(r'''
const f = await fixture({queued: [queuedTrack('one'), queuedTrack('two'), queuedTrack('three')]});
const result = await f.bridge.moveQueue('two', 'three', ['one', 'two', 'three']);
assert.equal(result.ok, true);
assert.equal(result.moved, false);
assert.equal(result.status.nowPlaying.playing, true);
assert.deepEqual(plain(result.status.queue.map(t => t.uid)), ['one', 'two', 'three']);
assert.equal(f.calls.length, 0);
''')

    def test_atomic_reorder_keeps_all_context_and_hidden_slots_with_one_command(self):
        self.run_js(r'''
const items = [queuedTrack('one', 'b'), queuedTrack('two', 'c'), queuedTrack('three', 'b'), queuedTrack('four', 'd')];
const context = Array.from({length: 16}, (_, n) => ({uid: n % 2 ? 'context-repeat' : 'one',
  uri: 'spotify:track:' + 'e'.repeat(22), provider: 'context', metadata: {iteration: n}}));
const hidden = {...queuedTrack('hidden-queued'), hidden_in_queue: true};
const delimiter = {uid: 'delimiter0', uri: 'spotify:delimiter', provider: 'context'};
const original = [context[0], items[0], hidden, items[1], delimiter, items[2], context[1], items[3], ...context.slice(2)];
const f = await fixture({internal: {next_items: original}});
const previous = f.internal.previous_items, current = f.internal.current_track;
const desired = ['four', 'two', 'one', 'three'];
const result = await f.bridge.reorderQueue(desired, ['one', 'two', 'three', 'four'], current.uid, 'standalone');
assert.equal(result.ok, true);
assert.equal(result.action, 'reorderQueue');
assert.equal(result.moved, true);
assert.deepEqual(plain(result.orderedQueuedUids), desired);
assert.deepEqual(plain(result.status.queue.filter(t => t.source === 'queue').map(t => t.uid)), desired);
assert.equal(result.status.nowPlaying.uid, current.uid);
assert.equal(result.status.nowPlaying.playing, true);
assert.equal(f.calls.length, 1);
assert.equal(f.calls[0].action, 'setInternalPlayerQueue');
assert.equal(f.calls[0].id, 'standalone');
assert.deepEqual(f.calls[0].parameters, {ignoreRevision: false});
const payload = f.lastQueuePayload();
assert.equal(payload.revision, 'revision-before');
assert.equal(payload.previous_items, previous);
assert.equal(f.internal.current_track, current);
assert.equal(payload.next_items.length, original.length);
let slot = 0;
original.forEach((item, index) => {
  if (item.provider === 'queue' && !item.hidden_in_queue) {
    const wanted = desired[slot++];
    assert.equal(payload.next_items[index], items.find(value => value.uid === wanted));
  } else assert.equal(payload.next_items[index], item);
});
''')

    def test_reorder_requires_an_exact_uid_permutation_and_explicit_current_device(self):
        self.run_js(r'''
const expected = ['one', 'two', 'three'], desired = ['three', 'one', 'two'];
const cases = [
  {desired: ['three', 'one']}, {desired: ['three', 'one', 'two', 'added']},
  {desired: ['three', 'one', 'one']}, {desired: ['three', 'one', 'missing']},
  {desired: ['three', 'one', null]}, {desired: 'three,one,two'},
  {expected: ['one', 'one', 'three']}, {expected: ['one', 'three', 'two']},
  {current: undefined}, {current: ''}, {current: 'another-occurrence'},
  {device: undefined}, {device: ''}, {device: 'another-device'}
];
for (const changes of cases) {
  const row = {desired, expected, current: 'occurrence-a', device: 'standalone', ...changes};
  const f = await fixture({queued: expected.map(id => queuedTrack(id))});
  const result = await f.bridge.reorderQueue(row.desired, row.expected, row.current, row.device);
  assert.equal(result.dispatched, false, JSON.stringify(changes));
  assert.equal(typeof result.error, 'string');
  assert.equal(f.calls.length, 0);
}
const ambiguous = await fixture({queued: [queuedTrack('one'), queuedTrack('two')],
  context: [{...queuedTrack('two'), hidden_in_queue: true}]});
const rejected = await ambiguous.bridge.reorderQueue(['two', 'one'], ['one', 'two'], 'occurrence-a', 'standalone');
assert.equal(rejected.dispatched, false);
assert.equal(ambiguous.calls.length, 0);
''')

    def test_reorder_rechecks_manual_order_revision_current_and_device_changes(self):
        self.run_js(r'''
for (const mutate of [
  state => {state.internal.next_items.reverse(); state.syncQueue();},
  state => {state.internal.next_items.push(queuedTrack('manual-add')); state.syncQueue();},
  state => {state.internal.revision = 'manual-revision';},
  state => {state.internal.current_track = {...state.internal.current_track, uid: 'repeat-of-current-uri'};
    state.raw.track_window.current_track = state.internal.current_track;},
  state => {state.devices[0].id = 'manual-device';},
  state => {state.devices[0].is_controllable = false;},
  state => {state.harmony._hasFakeState = true;}
]) {
  const f = await fixture({queued: [queuedTrack('one'), queuedTrack('two'), queuedTrack('three')],
    onRawRead: state => {if (state.read === 2) mutate(state);}});
  const result = await f.bridge.reorderQueue(['three', 'two', 'one'], ['one', 'two', 'three'], 'occurrence-a', 'standalone');
  assert.equal(result.dispatched, false);
  assert.equal(typeof result.error, 'string');
  assert.equal(f.calls.length, 0);
}
const finalRace = await fixture({queued: [queuedTrack('one'), queuedTrack('two')],
  onDevicesRead: state => {if (state.read === 2) {
    state.internal.current_track = {...state.internal.current_track, uid: 'latest-current'};
    state.raw.track_window.current_track = state.internal.current_track;
  }}});
assert.equal((await finalRace.bridge.reorderQueue(['two', 'one'], ['one', 'two'], 'occurrence-a', 'standalone')).dispatched, false);
assert.equal(finalRace.calls.length, 0);
''')

    def test_reorder_waits_for_observation_and_reports_unconfirmed_once(self):
        self.run_js(r'''
for (const onQueueCommand of [() => true, () => false, () => {throw new Error('stale revision');}]) {
  const f = await fixture({queued: [queuedTrack('one'), queuedTrack('two')], onQueueCommand});
  const result = await f.bridge.reorderQueue(['two', 'one'], ['one', 'two'], 'occurrence-a', 'standalone');
  assert.equal(result.dispatched, true);
  assert.equal(typeof result.error, 'string');
  assert.equal(f.calls.length, 1);
}
const delayed = await fixture({queued: [queuedTrack('one'), queuedTrack('two'), queuedTrack('three')],
  onQueueCommand: () => true,
  onQueueRead: state => {if (state.calls.length && state.read === 4) {
    state.internal.next_items.reverse(); state.syncQueue();
  }}});
const result = await delayed.bridge.reorderQueue(['three', 'two', 'one'], ['one', 'two', 'three'], 'occurrence-a', 'standalone');
assert.equal(result.ok, true);
assert.ok(result.status.observedAt > 1000000);
assert.equal(delayed.calls.length, 1);
''')

    def test_reorder_does_not_retry_post_dispatch_manual_changes(self):
        self.run_js(r'''
for (const change of [
  state => {state.devices[0].id = 'new-device';},
  state => {state.internal.current_track = {...state.internal.current_track, uid: 'new-current'};
    state.raw.track_window.current_track = state.internal.current_track; state.syncQueue();},
  state => {state.internal.next_items.push(queuedTrack('new-item')); state.syncQueue();},
  state => {state.internal.previous_items = [];}
]) {
  const f = await fixture({queued: [queuedTrack('one'), queuedTrack('two')],
    onQueueCommand: state => {change(state); return true;}});
  const result = await f.bridge.reorderQueue(['two', 'one'], ['one', 'two'], 'occurrence-a', 'standalone');
  assert.equal(result.dispatched, true);
  assert.equal(typeof result.error, 'string');
  assert.equal(f.calls.length, 1);
}
''')

    def test_queue_no_op_resamples_pause_and_clock_after_preflight(self):
        self.run_js(r'''
for (const operation of ['move', 'reorder']) {
  let f;
  f = await fixture({queued: [queuedTrack('one'), queuedTrack('two')], onRawRead: state => {
    if (state.read === 2) {
      state.raw.paused = true; state.raw.position = 321; state.raw.timestamp = 1000000;
      f.advance(700);
    }
  }});
  const result = operation === 'move'
    ? await f.bridge.moveQueue('one', 'two', ['one', 'two'])
    : await f.bridge.reorderQueue(['one', 'two'], ['one', 'two'], 'occurrence-a', 'standalone');
  assert.equal(result.ok, true);
  assert.equal(result.moved, false);
  assert.equal(result.status.nowPlaying.paused, true);
  assert.equal(result.status.nowPlaying.progressMs, 321);
  assert.equal(result.status.observedAt, 1000700);
  assert.equal(f.calls.length, 0);
}
''')

    def test_queue_no_op_revalidates_changes_during_its_final_status_read(self):
        self.run_js(r'''
for (const mutate of [
  state => {state.internal.next_items.reverse(); state.syncQueue();},
  state => {state.internal.revision = 'manual-no-op-revision';},
  state => {state.internal.current_track = {...state.internal.current_track, uid: 'new-current'};
    state.raw.track_window.current_track = state.internal.current_track;},
  state => {state.devices[0].id = 'new-device';},
  state => {state.harmony._hasFakeState = true;}
]) {
  const f = await fixture({queued: [queuedTrack('one'), queuedTrack('two')],
    onRawRead: state => {if (state.read === 3) mutate(state);}});
  const result = await f.bridge.reorderQueue(['one', 'two'], ['one', 'two'], 'occurrence-a', 'standalone');
  assert.equal(typeof result.error, 'string');
  assert.equal(result.dispatched, false);
  assert.equal(f.calls.length, 0);
}
''')

    def test_reorder_empty_single_and_matching_orders_are_guarded_no_ops(self):
        self.run_js(r'''
for (const uids of [[], ['one'], ['one', 'two']]) {
  const f = await fixture({queued: uids.map(id => queuedTrack(id))});
  const result = await f.bridge.reorderQueue(uids, uids, 'occurrence-a', 'standalone');
  assert.equal(result.ok, true);
  assert.equal(result.moved, false);
  assert.deepEqual(plain(result.orderedQueuedUids), uids);
  assert.equal(result.status.nowPlaying.uid, 'occurrence-a');
  assert.equal(result.status.playbackState.device.id, 'standalone');
  assert.equal(f.calls.length, 0);
  const stale = await f.bridge.reorderQueue(uids, uids, 'stale-current', 'standalone');
  assert.equal(stale.dispatched, false);
  assert.equal(f.calls.length, 0);
}
''')


if __name__ == '__main__':
    unittest.main()
