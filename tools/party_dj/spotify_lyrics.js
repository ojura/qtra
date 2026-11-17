(() => {
  // Source: installed Spotify 1.2.95, modules 88971/60518/52388. Only the
  // existing RequestBuilder and read-only current-player APIs are used here.
  const own = (object, key) => {
    if (!object || !['object', 'function'].includes(typeof object)) return undefined;
    const descriptor = Object.getOwnPropertyDescriptor(object, key);
    return descriptor && Object.prototype.hasOwnProperty.call(descriptor, 'value') ? descriptor.value : undefined;
  };
  let platform;
  const element = [...document.querySelectorAll('div')].find(node =>
    Object.keys(node).some(key => key.startsWith('__reactFiber$')));
  if (element) {
    let root = own(element, Object.keys(element).find(key => key.startsWith('__reactFiber$')));
    let ancestors = 0;
    while (root?.return && ancestors++ < 1000) root = root.return;
    root = root?.stateNode?.current || root;
    const stack = [root], seen = new Set();
    while (stack.length && seen.size < 30000) {
      const fiber = stack.pop();
      if (!fiber || seen.has(fiber)) continue;
      seen.add(fiber);
      const value = fiber.memoizedProps?.value;
      if (value && typeof value.getRegistry === 'function' && typeof value.getSession === 'function') {
        platform = value;
        break;
      }
      if (fiber.child) stack.push(fiber.child);
      if (fiber.sibling) stack.push(fiber.sibling);
    }
  }
  if (!platform) return {ready: false};
  const registry = platform.getRegistry();
  const map = own(registry, '_map');
  const instance = name => {
    try {
      let scanned = 0;
      for (const [key, entry] of Map.prototype.entries.call(map)) {
        if (++scanned > 256) break;
        if (typeof key === 'symbol' && Symbol.prototype.toString.call(key) === `Symbol(${name})`) {
          const value = own(entry, 'instance');
          return value && typeof value === 'object' ? value : null;
        }
      }
    } catch {}
    return null;
  };
  // Never resolve a registration or establish a session as a side effect.
  const builder = instance('RequestBuilder'), player = instance('PlayerAPI');
  const devices = instance('ConnectDevicesAPI'), harmony = player?._harmony;
  if (typeof builder?.build !== 'function' || typeof harmony?.getCurrentState !== 'function'
      || typeof devices?.getActiveDevice !== 'function') return {ready: false};
  if (window.__laricsLyrics?.version === 1 && window.__laricsLyrics.isFor(registry, builder, player, devices)) {
    return {ready: true};
  }
  window.__laricsLyrics?.dispose?.();
  const jobs = new Map(), pending = new Set();
  const validId = value => typeof value === 'string' && /^[A-Za-z0-9_-]{8,128}$/.test(value);
  const validTarget = value => value && Object.keys(value).length === 3
    && typeof value.uid === 'string' && !!value.uid.trim() && value.uid.length <= 512
    && typeof value.deviceId === 'string' && !!value.deviceId.trim() && value.deviceId.length <= 512
    && typeof value.uri === 'string' && /^spotify:track:[A-Za-z0-9]{22}$/.test(value.uri);
  const sameTarget = (a, b) => a?.uid === b.uid && a?.uri === b.uri && a?.deviceId === b.deviceId;
  const unavailable = (target, reason) => ({target, availability: 'unavailable', timing: 'none', provider: null, lines: [], reason});
  const alive = job => jobs.get(job.id) === job && window.__laricsLyrics === api
    && !job.cancelled && Date.now() < job.expires;
  const fail = state => { const error = new Error('Lyrics read unavailable.'); error.lyricsState = state; throw error; };
  async function currentTarget() {
    if (harmony._hasFakeState) fail('stale');
    const [raw, device] = await Promise.all([harmony.getCurrentState(), devices.getActiveDevice()]);
    if (harmony._hasFakeState) fail('stale');
    return {uid: raw?.track_window?.current_track?.uid, uri: raw?.track_window?.current_track?.uri, deviceId: device?.id};
  }
  function normalize(body, job) {
    const lyrics = body?.lyrics;
    if (!lyrics || typeof lyrics.syncType !== 'string' || lyrics.syncType.length > 80
        || !Array.isArray(lyrics.lines) || lyrics.lines.length > 1000) fail('error');
    const timed = lyrics.syncType === 'LINE_SYNCED' || lyrics.syncType === 'SYLLABLE_SYNCED';
    let previous = -1, characters = 0;
    const lines = lyrics.lines.map(line => {
      if (typeof line?.words !== 'string' || line.words.length > 2000
          || /[\u0000-\u0008\u000b\u000c\u000e-\u001f]/.test(line.words)) fail('error');
      characters += line.words.length;
      if (characters > 100000) fail('error');
      let start = null;
      if (timed) {
        const raw = line.startTimeMs;
        if (!(typeof raw === 'number' || (typeof raw === 'string' && /^\d+(?:\.\d+)?$/.test(raw)))) fail('error');
        start = Number(raw);
        if (!Number.isFinite(start) || start < previous || start < 0 || start > job.duration + 2000) fail('error');
        previous = start;
      }
      return {text: line.words, startMs: start};
    });
    if (!lines.some(line => line.text.trim())) return unavailable(job.target, 'empty_lyrics');
    const label = typeof lyrics.providerDisplayName === 'string' && lyrics.providerDisplayName.trim()
      ? lyrics.providerDisplayName : typeof lyrics.provider === 'string' && lyrics.provider.trim() ? lyrics.provider : 'Spotify';
    if (label.length > 200 || /[\u0000-\u001f]/.test(label)) fail('error');
    return {target: job.target, availability: 'ready', timing: timed ? 'line' : 'none',
      provider: {name: label, attribution: label, sourceUrl: `https://open.spotify.com/track/${job.target.uri.split(':').pop()}`}, lines};
  }
  async function run(job, art) {
    try {
      const before = await currentTarget();
      if (!alive(job)) return;
      if (!sameTarget(before, job.target)) fail('stale');
      let result;
      try {
        const response = await builder.build()
          .withHost('https://spclient.wg.spotify.com/color-lyrics/v2')
          .withPath(`/track/${encodeURIComponent(job.target.uri.split(':').pop())}/image/${encodeURIComponent(art)}`)
          .withQueryParameters({format: 'json', vocalRemoval: false})
          .withEndpointIdentifier('/track/{trackId}').send();
        if (!alive(job)) return;
        result = normalize(response?.body, job);
      } catch (error) {
        if (error?.status === 404) result = unavailable(job.target, 'no_lyrics');
        else throw error;
      }
      const after = await currentTarget();
      if (!alive(job)) return;
      if (!sameTarget(after, job.target)) fail('stale');
      job.result = result;
      job.state = 'done';
    } catch (error) {
      if (alive(job)) job.state = error?.lyricsState === 'stale' ? 'stale' : 'error';
    } finally {
      pending.delete(job);
    }
  }
  const api = {
    version: 1,
    isFor: (r, b, p, d) => r === registry && b === builder && p === player && d === devices,
    begin(id, target, recording, expires) {
      const stamp = Date.now();
      for (const [key, value] of jobs) if (value.expires <= stamp) api.cancel(key);
      if (!validId(id) || !validTarget(target) || jobs.has(id)
          || typeof recording?.artUrl !== 'string' || !recording.artUrl.startsWith('https://') || recording.artUrl.length > 2048
          || !Number.isFinite(recording.durationMs) || recording.durationMs <= 0 || recording.durationMs > 86400000
          || !Number.isFinite(expires) || expires <= stamp || expires > stamp + 9000
          || pending.size >= 8 || jobs.size >= 16) return {id, state: 'error'};
      const job = {id, target: {...target}, duration: recording.durationMs, expires, state: 'pending', cancelled: false};
      jobs.set(id, job);
      pending.add(job);
      job.timer = setTimeout(() => {
        if (jobs.get(id) === job) { job.state = 'error'; job.result = undefined; }
      }, Math.max(1, expires - stamp));
      void run(job, recording.artUrl);
      return {id, state: 'pending'};
    },
    poll(id) {
      const job = jobs.get(id);
      if (!job || Date.now() >= job.expires) { api.cancel(id); return {id, state: 'error'}; }
      const result = {id, state: job.state, ...(job.state === 'done' ? {result: job.result} : {})};
      if (job.state !== 'pending') { clearTimeout(job.timer); jobs.delete(id); }
      return result;
    },
    cancel(id) {
      const job = jobs.get(id);
      if (job) { job.cancelled = true; job.result = undefined; clearTimeout(job.timer); jobs.delete(id); }
      return {id, state: 'cancelled'};
    },
    dispose() { for (const id of jobs.keys()) api.cancel(id); }
  };
  window.__laricsLyrics = api;
  return {ready: true};
})()
