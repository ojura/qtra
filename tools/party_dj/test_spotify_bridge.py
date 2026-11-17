"""Run the real bridge against isolated Spotify/Connect fixtures in Node."""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import unittest


BRIDGE = Path(os.environ.get('SPOTIFY_BRIDGE_PATH', Path(__file__).with_name('spotify_bridge.js'))).resolve()
HARNESS = r'''
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const bridgeSource = fs.readFileSync(process.argv[1], 'utf8');
const plain = value => JSON.parse(JSON.stringify(value));
async function fixture(options = {}) {
  let clock = 1000000;
  const calls = [];
  const track = {uri: 'spotify:track:' + 'a'.repeat(22), uid: 'occurrence-a',
    metadata: {title: 'Current track', artist_name: 'Current artist', duration: '127050'}};
  const state = {item: track, isPaused: false, isBuffering: false,
    timestamp: clock - 2000, positionAsOfTimestamp: 10000, duration: 127050, speed: 1, ...options.state};
  const raw = {track_window: {current_track: track}, paused: false, loading: false,
    timestamp: clock - 2000, position: 10000, duration: 127050, playback_speed: 1, ...options.raw};
  const devices = options.devices || [{id: 'standalone', name: 'Native Spotify', type: 'computer',
    local: false, is_active: true, is_controllable: true, is_observable: true}];
  const queue = {current: track, queued: [], nextUp: []};
  let api, harmony;
  const execute = async (action, id, parameters) => {
    calls.push({action, id, parameters: plain(parameters)});
    if (options.onCommand) return options.onCommand({action, id, parameters, state, raw, devices, harmony});
    raw.paused = action === 'pause';
    raw.loading = false;
    raw.timestamp = clock;
    return true;
  };
  harmony = {_hasFakeState: !!options.fakeState, getDevices: async () => devices,
    getCurrentState: async () => {
      if (options.onRawRead) options.onRawRead({harmony, state, raw, devices});
      return structuredClone(raw);
    }, pause: (id, parameters) => execute('pause', id, parameters),
    resume: (id, parameters) => execute('resume', id, parameters)};
  api = {_harmony: options.withHarmony === false ? undefined : harmony,
    getState: () => structuredClone(state), getQueue: async () => structuredClone(queue),
    addToQueue: async () => calls.push({action: 'addToQueue'}),
    skipToNext: async () => calls.push({action: 'next'}),
    pause: async () => { calls.push({action: 'wrapperPause'}); state.isPaused = true; },
    resume: async () => { calls.push({action: 'wrapperResume'}); state.isPaused = false; }};
  const connect = {getActiveDevice: () => options.observedDevice || devices.find(device => device.is_active)};
  const map = new Map([[Symbol('PlayerAPI'), api], [Symbol('ConnectDevicesAPI'), connect]]);
  const registry = {_map: map, resolve: symbol => map.get(symbol)};
  const platform = {getRegistry: () => registry, getSession: async () => ({isAnonymous: false})};
  const element = {'__reactFiber$fixture': {memoizedProps: {value: platform}}};
  const window = {__laricsDJ: options.previousBridge};
  const context = {window, document: {querySelectorAll: () => [element]},
    location: {hostname: options.hostname || 'open.spotify.com'},
    Date: {now: () => clock}, setTimeout: (callback, ms) => { clock += ms; callback(); }};
  const ready = await vm.runInNewContext(bridgeSource, context);
  assert.equal(ready.ready, true);
  return {bridge: window.__laricsDJ, calls, raw, state, devices, harmony,
    advance: milliseconds => { clock += milliseconds; }};
}
'''


class BridgeTransportTests(unittest.TestCase):
    def run_js(self, script):
        source = HARNESS + '\n(async()=>{' + script + '\n})().catch(error=>{console.error(error.stack);process.exitCode=1;});'
        result = subprocess.run(['node', '-e', source, str(BRIDGE)], capture_output=True,
                                text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

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
assert.equal(status.nowPlaying.title, 'Current track'); // Same-URI metadata is reusable.
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
const f = await fixture({previousBridge: {version: 6, operations},
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


if __name__ == '__main__':
    unittest.main()
