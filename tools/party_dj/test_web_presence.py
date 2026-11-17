"""Run the actual web presence consumer with private DOM, clock and HTTP fakes."""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


APP = Path(__file__).with_name('web').joinpath('app.js').resolve()
HARNESS = r'''
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const appSource = fs.readFileSync(process.argv[1], 'utf8');
const fixtures = [];
const START = 1800000000000;
const lease = (changes = {}) => ({status: 'online', lastSeen: START,
  expiresAt: START + 30000, remainingMs: 30000, activeCount: 1, ...changes});

async function fixture(state = {}) {
  let wall = START, monotonic = 0, timerId = 0;
  const timeouts = new Map(), intervals = new Map(), pending = [], calls = [], unexpected = [];
  const blocked = detail => { unexpected.push(detail); throw new Error(detail); };
  class Element {
    constructor(tag = 'div') {
      this.tagName = tag;
      this.children = [];
      this.parentNode = null;
      this.dataset = {};
      this.style = {};
      this.attributes = {};
      this.events = new Map();
      this.className = '';
      this.value = '';
      this._text = '';
      this.scrollHeight = 1000;
      this.clientHeight = 400;
      this.scrollTop = 600;
      this.classList = {
        add: name => this.changeClass(name, true),
        remove: name => this.changeClass(name, false),
        toggle: (name, enabled) => this.changeClass(name, enabled),
      };
    }
    changeClass(name, enabled) {
      const names = new Set(this.className.split(/\s+/).filter(Boolean));
      if (enabled) names.add(name); else names.delete(name);
      this.className = [...names].join(' ');
    }
    get textContent() { return this._text; }
    set textContent(value) { this._text = String(value); this.children = []; }
    get childNodes() { return this.children; }
    get firstChild() { return this.children[0]; }
    append(...nodes) {
      for (const node of nodes) { node.parentNode = this; this.children.push(node); }
    }
    replaceChildren(...nodes) { this.children = []; this.append(...nodes); }
    insertBefore(node, before) {
      const index = this.children.indexOf(before);
      if (index < 0) throw new Error('Fixture insertBefore target is absent.');
      node.parentNode = this; this.children.splice(index, 0, node);
    }
    remove() {
      if (this.parentNode) this.parentNode.children = this.parentNode.children.filter(node => node !== this);
      this.parentNode = null;
    }
    setAttribute(name, value) { this.attributes[name] = String(value); }
    addEventListener(name, callback) { this.events.set(name, callback); }
    querySelector(selector) {
      const matches = node => selector.startsWith('.')
        ? node.className.split(/\s+/).includes(selector.slice(1)) : node.tagName === selector;
      for (const child of this.children) {
        if (matches(child)) return child;
        const nested = child.querySelector(selector);
        if (nested) return nested;
      }
      return null;
    }
    focus() {}
  }
  const elements = new Map(), documentEvents = new Map();
  const document = {
    hidden: false,
    body: new Element('body'),
    getElementById(id) {
      if (!elements.has(id)) elements.set(id, new Element());
      return elements.get(id);
    },
    createElement: tag => new Element(tag),
    createDocumentFragment: () => new Element('fragment'),
    querySelectorAll: selector => {
      if (!['[data-mood]', '[data-search]'].includes(selector)) return blocked('Unexpected DOM query: ' + selector);
      return [];
    },
    addEventListener: (name, callback) => documentEvents.set(name, callback),
  };
  document.getElementById('search-empty').append(new Element('p'));
  const storage = new Map();
  class Clock extends Date {
    constructor(...args) { super(...(args.length ? args : [wall])); }
    static now() { return wall; }
  }
  class Controller {
    constructor() { this.signal = {aborted: false}; }
    abort() { this.signal.aborted = true; }
  }
  class Image extends Element {
    set src(value) { blocked('Unexpected image load: ' + value); }
  }
  const response = body => ({ok: true, json: async () => body});
  const context = vm.createContext({
    document, Date: Clock, performance: {now: () => monotonic}, URL,
    location: {href: 'https://fixture.invalid/'},
    localStorage: {getItem: key => storage.get(key) ?? null, setItem: (key, value) => storage.set(key, value)},
    crypto: {randomUUID: () => '00000000-0000-4000-8000-000000000001'},
    AbortController: Controller, Image,
    fetch: (value, options = {}) => {
      const url = new URL(value, 'https://fixture.invalid/');
      if (url.origin !== 'https://fixture.invalid' || (options.method || 'GET') !== 'GET'
          || !['/api/state', '/api/chat'].includes(url.pathname)) {
        return blocked('Unexpected HTTP operation: ' + (options.method || 'GET') + ' ' + value);
      }
      calls.push({path: url.pathname, url: value});
      if (url.pathname === '/api/state') return Promise.resolve(response({
        spotify: {connected: false}, nowPlaying: null, queue: [], mood: {}, bookMoment: null, ...state,
      }));
      return new Promise((resolve, reject) => pending.push({resolve, reject}));
    },
    setTimeout: (callback, delay) => { const id = ++timerId; timeouts.set(id, {callback, delay}); return id; },
    clearTimeout: id => timeouts.delete(id),
    setInterval: (callback, delay) => {
      const id = ++timerId; intervals.set(id, {callback, delay, next: monotonic + delay}); return id;
    },
    clearInterval: id => intervals.delete(id),
  });
  assert.equal(vm.runInContext('typeof process', context), 'undefined');
  assert.equal(vm.runInContext('typeof require', context), 'undefined');
  assert.equal(vm.runInContext('typeof WebSocket', context), 'undefined');
  const flush = async () => { for (let count = 0; count < 12; count++) await Promise.resolve(); };
  vm.runInContext(appSource, context, {filename: 'web/app.js', timeout: 1000});
  await flush();
  const result = {
    calls, elements,
    presence: () => ({state: elements.get('dj-presence').dataset.state,
      text: elements.get('dj-presence-text').textContent}),
    async reply(dj, messages = []) {
      assert.ok(pending.length, 'A real app.js chat poll must be waiting.');
      pending.shift().resolve(response({dj, messages}));
      await flush();
    },
    async failChat() {
      assert.ok(pending.length, 'A real app.js chat poll must be waiting.');
      pending.shift().reject(new Error('Private fixture connection failure.'));
      await flush();
    },
    async poll() {
      const scheduled = [...timeouts].filter(([, timer]) => timer.delay === 2000);
      assert.ok(scheduled.length, 'The real app must schedule its next poll.');
      for (const [id, timer] of scheduled) { timeouts.delete(id); timer.callback(); }
      await flush();
    },
    async advance(milliseconds, options = {}) {
      wall += options.wall ?? milliseconds;
      monotonic += options.monotonic ?? milliseconds;
      assert.ok(intervals.size, 'The real app must refresh displayed time locally.');
      for (const timer of intervals.values()) {
        if (timer.next <= monotonic) {
          timer.next += (Math.floor((monotonic - timer.next) / timer.delay) + 1) * timer.delay;
          timer.callback();
        }
      }
      await flush();
    },
    async visible(value) {
      document.hidden = !value;
      documentEvents.get('visibilitychange')();
      await flush();
    },
    assertSafe() {
      assert.deepEqual(unexpected, []);
      assert.ok(calls.every(call => ['/api/state', '/api/chat'].includes(call.path)));
    },
  };
  fixtures.push(result);
  return result;
}
'''


class WebPresenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        candidates = (shutil.which('node'), shutil.which('nodejs'),
                      '/usr/bin/node', '/usr/local/bin/node', '/snap/node/current/bin/node')
        for candidate in candidates:
            if not candidate:
                continue
            try:
                executable = Path(candidate).resolve(strict=True)
                if (executable.name not in ('node', 'nodejs') or not executable.is_file()
                        or not os.access(executable, os.X_OK)):
                    continue
                with executable.open('rb') as handle:
                    if handle.read(4) != b'\x7fELF':
                        continue
            except OSError:
                continue
            cls.node = str(executable)
            return
        raise RuntimeError('The private web fixture requires a direct executable Node ELF binary; PATH dispatchers are not launched.')

    def run_js(self, script):
        source = (HARNESS + '\n(async()=>{\n' + script
                  + '\nfor (const fixture of fixtures) fixture.assertSafe();'
                  + "\nprocess.stdout.write('WEB_PRESENCE_OK\\n');"
                  + '\n})().catch(error=>{console.error(error.stack);process.exitCode=1;});')
        with tempfile.TemporaryDirectory(prefix='qtra-web-presence-') as directory:
            result = subprocess.run(
                [self.node, '-e', source, str(APP)], input='', capture_output=True,
                text=True, timeout=10, cwd=directory, env={'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8'},
                shell=False, close_fds=True)
        diagnostic = f'Node executable: {self.node}\n{result.stdout}{result.stderr}'
        self.assertEqual(result.returncode, 0, diagnostic)
        self.assertEqual(result.stdout.strip(), 'WEB_PRESENCE_OK', diagnostic)

    def test_explicit_agent_lease_is_authoritative_over_legacy_aliases(self):
        self.run_js(r'''
const f = await fixture();
await f.reply({llm: lease(), status: 'offline', lastSeen: 0, watcher: {status: 'offline'}});
assert.deepEqual(f.presence(), {state: 'online', text: 'DJ je tu'});
await f.poll();
await f.reply({llm: lease({activeCount: 256})});
assert.equal(f.presence().state, 'online');
await f.poll();
await f.reply({llm: {status: 'offline', lastSeen: START, expiresAt: 0, remainingMs: 0, activeCount: 0},
  status: 'listening', lastSeen: START, watcher: {status: 'online'}});
assert.equal(f.presence().state, 'away');
''')

    def test_watcher_and_active_work_cannot_manufacture_agent_availability(self):
        self.run_js(r'''
const f = await fixture();
await f.reply({status: 'working', lastSeen: START, watcher: {status: 'online'},
  processingMessageIds: [1], messageWork: {'1': {state: 'active', effectiveState: 'active'}}},
  [{id: 1, role: 'user', name: 'Fixture', text: 'A private fixture message.', status: 'read', createdAt: START}]);
assert.equal(f.presence().state, 'away');
const delivery = f.elements.get('chat-log').querySelector('.chat-delivery');
assert.match(delivery.textContent, /DJ radi na ovome/);
''')

    def test_local_expiry_works_while_the_next_poll_remains_unresolved(self):
        self.run_js(r'''
const f = await fixture();
await f.reply({llm: lease()});
await f.advance(2000);
await f.poll();
const calls = f.calls.length;
await f.advance(27999);
assert.equal(f.presence().state, 'online');
await f.advance(1);
assert.equal(f.presence().state, 'away');
assert.equal(f.calls.length, calls);
''')

    def test_identical_cached_projection_never_renews_monotonic_expiry(self):
        self.run_js(r'''
const f = await fixture();
const data = {llm: lease()};
await f.reply(data);
await f.advance(20000, {wall: 1000});
await f.poll();
await f.reply(data);
await f.advance(10000, {wall: 1000});
assert.equal(f.presence().state, 'away');
await f.poll();
await f.reply(data);
assert.equal(f.presence().state, 'away');
''')

    def test_wall_deadline_expires_even_if_monotonic_time_has_barely_advanced(self):
        self.run_js(r'''
const f = await fixture();
await f.reply({llm: lease()});
await f.advance(30000, {monotonic: 1000});
assert.equal(f.presence().state, 'away');
''')

    def test_a_cached_lease_becomes_unavailable_if_the_wall_clock_moves_before_its_receipt(self):
        self.run_js(r'''
const f = await fixture();
await f.reply({llm: lease()});
await f.advance(1000, {wall: -1});
assert.equal(f.presence().state, 'away');
''')

    def test_missing_malformed_and_future_leases_are_unavailable(self):
        self.run_js(r'''
const cases = [
  undefined, null, [], 'online', {}, lease({status: 'listening'}),
  lease({lastSeen: undefined}), lease({lastSeen: true}), lease({lastSeen: String(START)}),
  lease({lastSeen: NaN}), lease({lastSeen: Infinity}), lease({lastSeen: START + 1}),
  lease({expiresAt: true}), lease({expiresAt: START}), lease({expiresAt: START + 120001}),
  lease({expiresAt: START + 29999}), lease({remainingMs: undefined}),
  lease({remainingMs: true}), lease({remainingMs: NaN}), lease({remainingMs: 0}),
  lease({remainingMs: -1}), lease({remainingMs: 30001}), lease({activeCount: true}),
  lease({activeCount: '1'}), lease({activeCount: 0}), lease({activeCount: 1.5}), lease({activeCount: 257}),
  lease({activeCount: Number.MAX_SAFE_INTEGER + 1}),
];
for (const invalid of cases) {
  const f = await fixture();
  await f.reply({llm: invalid, status: 'online', lastSeen: START, watcher: {status: 'online'}});
  assert.equal(f.presence().state, 'away', JSON.stringify(invalid));
}
''')

    def test_invalid_update_removes_availability_without_resetting_the_old_deadline(self):
        self.run_js(r'''
const f = await fixture();
const valid = {llm: lease()};
await f.reply(valid);
await f.advance(10000, {wall: 1000});
await f.poll();
await f.reply({llm: lease({remainingMs: '30000'})});
assert.equal(f.presence().state, 'away');
await f.advance(20000, {wall: 1000});
await f.poll();
await f.reply(valid);
assert.equal(f.presence().state, 'away');
''')

    def test_network_failure_preserves_only_the_unexpired_explicit_lease(self):
        self.run_js(r'''
const f = await fixture();
await f.reply({llm: lease()});
await f.advance(5000);
await f.poll();
await f.failChat();
assert.equal(f.presence().state, 'online');
await f.advance(25000);
assert.deepEqual(f.presence(), {state: 'away', text: 'Spajam razgovor'});
''')

    def test_genuine_renewal_can_extend_presence(self):
        self.run_js(r'''
const f = await fixture();
await f.reply({llm: lease()});
await f.advance(20000);
await f.poll();
await f.reply({llm: lease({lastSeen: START + 20000, expiresAt: START + 80000, remainingMs: 60000})});
await f.advance(20000);
assert.equal(f.presence().state, 'online');
await f.advance(40000);
assert.equal(f.presence().state, 'away');
''')

    def test_newer_session_departure_can_move_last_seen_back_without_renewal(self):
        self.run_js(r'''
const f = await fixture();
await f.advance(10000);
await f.reply({llm: lease({lastSeen: START + 10000, expiresAt: START + 120000,
  remainingMs: 110000, activeCount: 2})});
await f.advance(5000);
await f.poll();
await f.reply({llm: lease({lastSeen: START, expiresAt: START + 120000, remainingMs: 105000})});
assert.equal(f.presence().state, 'online');
await f.advance(105000, {wall: 1000});
assert.equal(f.presence().state, 'away');
''')

    def test_visibility_restore_expires_cached_presence_before_a_poll_reply(self):
        self.run_js(r'''
const f = await fixture();
await f.reply({llm: lease()});
await f.visible(false);
await f.advance(30000);
await f.visible(true);
assert.equal(f.presence().state, 'away');
''')

    def test_croatian_statuses_preserve_original_music_metadata(self):
        self.run_js(r'''
const f = await fixture({spotify: {connected: true},
  nowPlaying: {title: 'Original song title', artist: 'Original artist', progressMs: 60000,
    durationMs: 180000, observedAt: START, playing: false},
  queue: [{title: 'Another original title', artist: 'Original artist', status: 'uncertain'}],
  mood: {label: 'singalong'}});
assert.equal(f.elements.get('connection-text').textContent, 'Tu si s ekipom');
assert.equal(f.elements.get('room-mood').textContent, 'Pjevamo');
assert.equal(f.elements.get('now-title').textContent, 'Original song title');
assert.equal(f.elements.get('now-artist').textContent, 'Original artist');
assert.equal(f.elements.get('track-progress').attributes['aria-valuetext'], '1:00 od 3:00');
assert.equal(f.elements.get('queue').querySelector('.queue-status').textContent, 'Provjeravam Spotify…');
assert.equal(f.elements.get('queue').querySelector('.track-title').textContent, 'Another original title');
await f.reply({llm: lease()}, [{id: 1, role: 'user', text: 'Keep this original message.',
  status: 'read', createdAt: START, work: {state: 'waiting'}}]);
assert.equal(f.elements.get('chat-log').querySelector('.chat-text').textContent, 'Keep this original message.');
assert.equal(f.elements.get('chat-log').querySelector('.chat-delivery').textContent, 'DJ je pročitao · Na čekanju');
''')

    def test_displayed_service_errors_are_localized_and_do_not_leak_unknown_details(self):
        self.run_js(r'''
const cases = [
  ['Choose a song from the search results.', 'Odaberi pjesmu iz rezultata pretrage.'],
  ['Spotify search is still loading. Try again in a moment.', 'Spotify još učitava pretragu. Pokušaj ponovno za trenutak.'],
  ['A message can contain at most 1500 characters.', 'Poruka može sadržavati najviše 1500 znakova.'],
  ['Spotify returned HTTP 429', 'Previše zahtjeva odjednom. Pričekaj trenutak pa pokušaj ponovno.'],
  ['Spotify is disconnected', 'Veza s DJ-em ili Spotifyjem trenutačno nije dostupna. Pokušaj ponovno za trenutak.'],
  ['Unhandled internal fixture detail\nstack trace', 'Zahtjev se nije mogao dovršiti. Pokušaj ponovno.'],
  ['constructor', 'Zahtjev se nije mogao dovršiti. Pokušaj ponovno.'],
];
for (const [error, expected] of cases) {
  const f = await fixture({spotify: {connected: false, error}});
  assert.equal(f.elements.get('room-notice').textContent, expected);
  assert.equal(f.elements.get('room-notice').hidden, false);
}
''')


if __name__ == '__main__':
    unittest.main()
