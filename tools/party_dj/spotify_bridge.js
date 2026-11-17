(async () => {
  if (window.__laricsDJ?.version === 8) return {ready: true};
  if (!/spotify\.com$/.test(location.hostname) && !location.hostname.includes('xpui')) {
    return {ready: false, error: 'Waiting for the Spotify client.'};
  }

  // Spotify provides its services through a React context registry. Read the
  // actual provider in this running version, rather than module numbers.
  let platform;
  const element = [...document.querySelectorAll('div')].find(node =>
    Object.keys(node).some(key => key.startsWith('__reactFiber$')));
  if (element) {
    let root = element[Object.keys(element).find(key => key.startsWith('__reactFiber$'))];
    while (root?.return) root = root.return;
    root = root?.stateNode?.current || root;
    const stack = [root], visited = new Set();
    while (stack.length && visited.size < 30000) {
      const fiber = stack.pop();
      if (!fiber || visited.has(fiber)) continue;
      visited.add(fiber);
      const value = fiber.memoizedProps?.value;
      if (value && typeof value.getSession === 'function' && typeof value.getRegistry === 'function') {
        platform = value;
        break;
      }
      if (fiber.child) stack.push(fiber.child);
      if (fiber.sibling) stack.push(fiber.sibling);
    }
  }
  if (!platform) return {ready: false, error: 'Spotify is loading its player connection.'};
  const session = await platform.getSession();
  if (session.isAnonymous) return {ready: false, error: 'Sign into Spotify to connect the DJ.'};
  const registry = platform.getRegistry();
  const resolve = name => {
    const symbol = [...registry._map.keys()].find(key => String(key) === `Symbol(${name})`);
    return symbol ? registry.resolve(symbol) : null;
  };
  const player = resolve('PlayerAPI');
  if (!player?.getQueue || !player?.getState || !player?.addToQueue) {
    return {ready: false, error: 'Spotify has not exposed its queue controls yet.'};
  }
  const harmony = player._harmony;
  const connectDevices = resolve('ConnectDevicesAPI');
  const browserController = /spotify\.com$/.test(location.hostname);

  const number = (...values) => values.find(value => typeof value === 'number' && Number.isFinite(value));
  const deviceInfo = device => device ? {id: device.id, name: device.name, type: device.type,
    isLocal: !!(device.isLocal ?? device.local)} : null;

  function ensureActualState() {
    // Harmony can claim synthetic state by transferring to its local streamer.
    // A transport-only command must never enter that path.
    if (harmony?._hasFakeState) throw new Error('Spotify is showing synthetic local state; reconnect to the active player first.');
  }

  async function activeTarget() {
    ensureActualState();
    if (typeof harmony?.getDevices !== 'function') throw new Error('Spotify has not exposed active-device transport.');
    const devices = await harmony.getDevices();
    const active = Array.isArray(devices) ? devices.filter(device => device.is_active === true && device.id) : [];
    if (active.length !== 1) throw new Error('Spotify must identify one active playback device before transport control.');
    if (active[0].is_controllable !== true || active[0].is_observable !== true) {
      throw new Error('The active Spotify device cannot be controlled and observed from this controller.');
    }
    return active[0];
  }

  function normalize(item) {
    if (!item) return null;
    const metadata = item.metadata || {};
    const artists = item.artists?.map(a => a.name || a.profile?.name).filter(Boolean).join(', ');
    let art = item.album?.images?.[0]?.url || item.images?.[0]?.url || metadata.image_url || '';
    if (art.startsWith('spotify:image:')) art = `https://i.scdn.co/image/${art.split(':').pop()}`;
    const uri = item.uri || `spotify:track:${item.id}`;
    return {id: item.id || uri.split(':').pop(), uri, uid: item.uid || null,
      title: item.name || metadata.title || 'Unknown track',
      artist: artists || metadata.artist_name || '',
      album: item.album?.name || metadata.album_title || '', artUrl: art,
      durationMs: item.duration_ms || item.duration?.milliseconds || Number(metadata.duration || 0),
      explicit: !!(item.explicit || item.isExplicit),
      source: item.provider === 'queue' ? 'queue' : 'context'};
  }

  function searchResults() {
    const page = document.getElementById('searchPage');
    if (!page) return null;
    let fiber = page[Object.keys(page).find(key => key.startsWith('__reactFiber$'))];
    for (let i = 0; fiber && i < 30; ++i, fiber = fiber.return) {
      const results = fiber.memoizedProps?.results;
      if (results?.tracks?.items) return results;
    }
    return null;
  }

  const operations = window.__laricsDJ?.operations || new Map();
  const bridge = {
    version: 8,
    operations,
    async status() {
      if (browserController) ensureActualState();
      const state = await player.getState();
      const queue = await player.getQueue();
      if (!state || !queue) throw new Error('Spotify is connecting to the active player.');
      const raw = typeof harmony?.getCurrentState === 'function' && !harmony._hasFakeState
        ? await harmony.getCurrentState() : null;
      if (browserController) ensureActualState();
      const rawTrack = raw?.track_window?.current_track;
      let current = state.item || queue.current;
      if (rawTrack?.uri) {
        const matching = [state.item, queue.current].find(track => track?.uri === rawTrack.uri);
        current = {...(matching || rawTrack), uri: rawTrack.uri, uid: rawTrack.uid ?? matching?.uid};
      }
      const item = normalize(current);
      const useRaw = typeof raw?.paused === 'boolean' && number(raw.position) !== undefined
        && !!rawTrack?.uri && rawTrack.uri === item?.uri;
      const stateTrack = state.item || queue.current;
      const sameStateOccurrence = stateTrack?.uri === item?.uri
        && !(stateTrack?.uid && item?.uid && stateTrack.uid !== item.uid);
      if (!sameStateOccurrence && !useRaw) throw new Error('Spotify changed playback occurrences before a complete clock was available.');
      const normalizedClock = sameStateOccurrence ? state : {};
      const paused = useRaw ? raw.paused : !!state.isPaused;
      const buffering = useRaw ? !!raw.loading : !!state.isBuffering;
      const stamp = Date.now();
      const playing = !!item && !paused && !buffering;
      const duration = [useRaw ? raw.duration : null, normalizedClock.duration, item?.durationMs]
        .find(value => typeof value === 'number' && Number.isFinite(value) && value > 0) || 0;
      const position = Math.max(0, useRaw ? raw.position : number(state.positionAsOfTimestamp, state.position, 0));
      const sourceTimestamp = number(useRaw ? raw.timestamp : null, normalizedClock.timestamp, stamp);
      const speed = number(useRaw ? raw.playback_speed : null, normalizedClock.speed, 1);
      const elapsed = playing && sourceTimestamp > 0 ? Math.max(0, stamp - sourceTimestamp) * Math.max(0, speed) : 0;
      const progress = Math.max(0, position + elapsed);
      const device = typeof connectDevices?.getActiveDevice === 'function' ? await connectDevices.getActiveDevice() : null;
      return {
        nowPlaying: item ? {...item, playing, paused, buffering, durationMs: duration,
          progressMs: duration > 0 ? Math.min(duration, progress) : progress,
          observedAt: stamp} : null,
        queue: [...(queue.queued || []), ...(queue.nextUp || []).slice(0, 8)].map(normalize).filter(Boolean),
        explicitQueueComplete: true,
        playbackState: {source: useRaw ? 'harmony' : 'player-api', paused, buffering,
          positionMs: position, sourceTimestamp, device: deviceInfo(device)},
        observedAt: stamp
      };
    },
    async search(query) {
      const destination = '/search/' + encodeURIComponent(query) + '/tracks';
      const previous = searchResults();
      const sameQuery = decodeURIComponent(location.pathname) === decodeURIComponent(destination);
      if (!sameQuery) platform.getHistory().push(destination);
      const deadline = Date.now() + 16000;
      while (Date.now() < deadline) {
        const results = searchResults();
        const onQuery = decodeURIComponent(location.pathname) === decodeURIComponent(destination);
        const list = document.querySelector('[data-testid="track-list"]');
        const label = list?.getAttribute('aria-label') || '';
        const labelMatches = label.endsWith(`“${query}”`) || label.endsWith(`"${query}"`);
        const row = list?.querySelector('[data-testid="tracklist-row"]');
        if (onQuery && labelMatches && row && results && (sameQuery || results.requestId !== previous?.requestId)) {
          let fiber = row[Object.keys(row).find(key => key.startsWith('__reactFiber$'))];
          for (let i = 0; fiber && i < 45; ++i, fiber = fiber.return) {
            const tracks = fiber.memoizedProps?.tracks;
            if (Array.isArray(tracks) && tracks.length) {
              return tracks.filter(t => t.isPlayable !== false).slice(0, 12).map(normalize);
            }
          }
        }
        const text = document.querySelector('main')?.innerText || '';
        if (onQuery && text.includes(query) && /No results found for|No results for/.test(text)) return [];
        await new Promise(resolve => setTimeout(resolve, 180));
      }
      throw new Error('Spotify search is still loading. Try again in a moment.');
    },
    async track(uri) {
      if (!/^spotify:track:[A-Za-z0-9]{22}$/.test(uri)) throw new Error('Invalid track.');
      const q = await player.getQueue();
      const known = [q.current, ...(q.queued || []), ...(q.nextUp || [])].find(t => t?.uri === uri);
      if (known) return normalize(known);
      const results = await bridge.search(uri);
      const found = results.find(t => t.uri === uri);
      if (!found) throw new Error('Find this track in search again before requesting it.');
      return found;
    },
    async enqueue(uri, requestId) {
      if (!/^spotify:track:[A-Za-z0-9]{22}$/.test(uri)) throw new Error('Invalid track.');
      if (operations.has(requestId)) return operations.get(requestId);
      const pending = (async () => {
        let before;
        try {
          before = await player.getQueue();
        } catch (error) {
          return {error: String(error?.message || error), dispatched: false};
        }
        const previousCount = (before.queued || []).filter(t => t.uri === uri).length;
        await player.addToQueue([{uri}]);
        const deadline = Date.now() + 6000;
        while (Date.now() < deadline) {
          const after = await player.getQueue();
          const matching = (after.queued || []).filter(t => t.uri === uri);
          if (matching.length > previousCount || after.current?.uri === uri) {
            return {queued: true, uri, requestId, uid: matching.at(-1)?.uid || null};
          }
          await new Promise(resolve => setTimeout(resolve, 180));
        }
        throw new Error('Spotify accepted the request; waiting for the queue update.');
      })();
      operations.set(requestId, pending);
      if (operations.size > 500) operations.delete(operations.keys().next().value);
      const result = await pending;
      if (result?.dispatched === false) operations.delete(requestId);
      return result;
    },
    async moveQueue(uid, beforeUid, expectedQueuedUids) {
      let dispatched = false;
      try {
        const validUid = value => typeof value === 'string' && value.length > 0;
        const same = (left, right) => JSON.stringify(left) === JSON.stringify(right);
        if (!validUid(uid) || !validUid(beforeUid) || uid === beforeUid) {
          throw new Error('Choose two distinct queue occurrence UIDs for the move.');
        }
        if (!Array.isArray(expectedQueuedUids) || expectedQueuedUids.length < 2
            || !expectedQueuedUids.every(validUid) || new Set(expectedQueuedUids).size !== expectedQueuedUids.length) {
          throw new Error('The expected explicit queue must contain unique, nonempty occurrence UIDs.');
        }
        const queueApi = player._queue;
        const manager = queueApi?._queueManager;
        if (typeof queueApi?.getInternalQueue !== 'function' || typeof manager?.setInternalPlayerQueue !== 'function'
            || typeof harmony?.getCurrentState !== 'function') {
          throw new Error('Spotify has no revision-checked, targeted queue move connection.');
        }
        const identity = item => [item?.uid, item?.uri];
        const signature = items => items.map(item => [item.uid, item.uri, item.provider, item.type,
          !!item.hidden_in_queue, item.metadata?.is_queued]);
        const explicit = items => items.filter(item => item.provider === 'queue' && !item.hidden_in_queue);
        const internal = () => {
          const value = queueApi.getInternalQueue();
          if (!value || value.revision === undefined || value.revision === null
              || !Array.isArray(value.next_items) || !Array.isArray(value.previous_items)
              || !validUid(value.current_track?.uid) || !value.current_track?.uri) {
            throw new Error('Spotify has no complete revision and current occurrence for a queue move.');
          }
          return value;
        };
        const target = deviceInfo(await activeTarget());
        const before = await bridge.status();
        const initial = internal();
        const current = identity(initial.current_track);
        const revision = initial.revision;
        const next = initial.next_items.slice();
        const previous = initial.previous_items;
        const originalNext = signature(next);
        const originalPrevious = signature(previous);
        const checkStatus = status => {
          if (status.playbackState.source !== 'harmony') throw new Error('The active Spotify device has no complete playback clock.');
          if (status.playbackState.device && status.playbackState.device.id !== target.id) {
            throw new Error('The observed Spotify device changed during the queue move.');
          }
          if (!same(identity(status.nowPlaying), current)) throw new Error('The current Spotify occurrence changed during the queue move.');
        };
        checkStatus(before);
        const observedUids = before.queue.filter(item => item.source === 'queue').map(item => item.uid);
        if (!same(observedUids, expectedQueuedUids)
            || !same(explicit(next).map(item => item.uid), expectedQueuedUids)) {
          throw new Error('The explicit Spotify queue changed; observe its current order before moving an occurrence.');
        }
        // Uniqueness belongs to explicit occurrences. Repeated context items and
        // delimiters stay intact and are never reconstructed from status.queue.
        const matches = id => next.filter(item => item.provider === 'queue' && item.uid === id);
        const moved = matches(uid), anchor = matches(beforeUid);
        if (moved.length !== 1 || anchor.length !== 1
            || moved[0].hidden_in_queue || anchor[0].hidden_in_queue
            || !expectedQueuedUids.includes(uid) || !expectedQueuedUids.includes(beforeUid)) {
          throw new Error('The moved occurrence and its anchor must each exist exactly once in the explicit queue.');
        }
        next.splice(next.indexOf(moved[0]), 1);
        next.splice(next.indexOf(anchor[0]), 0, moved[0]);
        const desiredUids = explicit(next).map(item => item.uid);
        const desiredNext = signature(next);

        const raw = await harmony.getCurrentState();
        if (!same(identity(raw?.track_window?.current_track), current)) {
          throw new Error('The current Spotify occurrence changed before queue dispatch.');
        }
        if ((await activeTarget()).id !== target.id) throw new Error('The active Spotify device changed before queue dispatch.');
        ensureActualState();
        const latest = internal();
        if (latest.revision !== revision || !same(identity(latest.current_track), current)
            || !same(signature(latest.next_items), originalNext)
            || !same(signature(latest.previous_items), originalPrevious)) {
          throw new Error('The Spotify queue revision or current occurrence changed before queue dispatch.');
        }
        if (manager._currentPlayerState?.restrictions?.disallow_set_queue_reasons?.length) {
          throw new Error('The active Spotify device currently forbids queue changes.');
        }
        if (same(desiredUids, expectedQueuedUids)) {
          return {ok: true, action: 'moveQueue', moved: false, uid, beforeUid, device: target, status: before};
        }
        // Use the awaitable manager behind the UI: the public reorder wrapper
        // drops this promise. Keep the server revision check and explicit target.
        dispatched = true;
        const accepted = await manager.setInternalPlayerQueue({revision, next_items: next, previous_items: previous},
          target.id, {ignoreRevision: false});
        if (accepted === false) throw new Error('Spotify did not accept the queue move for the active device.');
        const deadline = Date.now() + 8000;
        while (Date.now() < deadline) {
          if ((await activeTarget()).id !== target.id) throw new Error('The active Spotify device changed before the queue move was confirmed.');
          const status = await bridge.status();
          checkStatus(status);
          const observed = internal();
          if (!same(identity(observed.current_track), current) || !same(signature(observed.previous_items), originalPrevious)) {
            throw new Error('The current occurrence or playback history changed before the queue move was confirmed.');
          }
          const observedNext = signature(observed.next_items);
          const publicOrder = status.queue.filter(item => item.source === 'queue').map(item => item.uid);
          if (same(publicOrder, desiredUids) && same(observedNext, desiredNext)) {
            return {ok: true, action: 'moveQueue', moved: true, uid, beforeUid, device: target, status};
          }
          if ((!same(publicOrder, expectedQueuedUids) && !same(publicOrder, desiredUids))
              || (!same(observedNext, originalNext) && !same(observedNext, desiredNext))) {
            throw new Error('The Spotify queue changed concurrently before the move was confirmed.');
          }
          await new Promise(resolve => setTimeout(resolve, 250));
        }
        throw new Error('Spotify queue move was sent, but the observed queue did not confirm it.');
      } catch (error) {
        return {error: String(error?.message || error), dispatched};
      }
    },
    async control(action) {
      if (action === 'next') {
        await player.skipToNext();
        return {ok: true, action};
      }
      if (action !== 'pause' && action !== 'resume') throw new Error('Unknown playback action.');
      let target = null;
      if (typeof harmony?.[action] === 'function' && typeof harmony?.getCurrentState === 'function') {
        target = deviceInfo(await activeTarget());
        const before = await bridge.status();
        if (before.playbackState.source !== 'harmony') throw new Error('The active Spotify device has no complete playback clock.');
        if (before.playbackState.device && before.playbackState.device.id !== target.id) {
          throw new Error('The observed Spotify device changed before transport dispatch.');
        }
        if ((await activeTarget()).id !== target.id) throw new Error('The active Spotify device changed before transport dispatch.');
        ensureActualState();
        // The page-level wrappers emit cancellable UI actions. Dispatch through
        // the existing Connect controller for the explicitly selected device.
        const accepted = await harmony[action](target.id, {forceRemoteAction: true});
        if (accepted === false) throw new Error(`Spotify did not accept ${action} for the active device.`);
      } else {
        if (browserController) throw new Error('The Spotify web controller has no targeted transport connection.');
        await player[action]();
      }
      const deadline = Date.now() + 8000;
      while (Date.now() < deadline) {
        if (target && (await activeTarget()).id !== target.id) {
          throw new Error('The active Spotify device changed before transport could be confirmed.');
        }
        const status = await bridge.status();
        const observed = status.playbackState;
        if (target && observed.device && observed.device.id !== target.id) {
          throw new Error('The observed Spotify device no longer matches the transport target.');
        }
        if ((!target || observed.source === 'harmony') && observed.paused === (action === 'pause') && !observed.buffering) {
          return {ok: true, action, device: deviceInfo(target), status};
        }
        await new Promise(resolve => setTimeout(resolve, 250));
      }
      throw new Error(`Spotify ${action} was sent, but the active player's state did not confirm it.`);
    }
  };
  window.__laricsDJ = bridge;
  return {ready: true};
})()
