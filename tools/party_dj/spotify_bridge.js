(async () => {
  if (window.__laricsDJ?.version === 10) return {ready: true};
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

  async function applyQueueOrder(orderedQueuedUids, expectedQueuedUids, expectedCurrentUid, expectedDeviceId, resultFields) {
    let dispatched = false;
    try {
      const validUid = value => typeof value === 'string' && value.length > 0;
      const same = (left, right) => JSON.stringify(left) === JSON.stringify(right);
      const uniqueUids = values => Array.isArray(values) && values.every(validUid)
        && new Set(values).size === values.length;
      if (!uniqueUids(expectedQueuedUids) || !uniqueUids(orderedQueuedUids)
          || orderedQueuedUids.length !== expectedQueuedUids.length
          || orderedQueuedUids.some(id => !expectedQueuedUids.includes(id))) {
        throw new Error('The desired queue order must be an exact permutation of unique explicit occurrence UIDs.');
      }
      // Snapshot caller inputs before the first await; a pending operation must
      // keep the preconditions that were validated at entry.
      expectedQueuedUids = expectedQueuedUids.slice();
      orderedQueuedUids = orderedQueuedUids.slice();
      if (resultFields.action === 'reorderQueue') {
        if (!validUid(expectedCurrentUid) || !validUid(expectedDeviceId)) {
          throw new Error('Queue reorder requires the expected current occurrence UID and active device ID.');
        }
        resultFields = {...resultFields, orderedQueuedUids};
      }
      const queueApi = player._queue;
      const manager = queueApi?._queueManager;
      if (typeof queueApi?.getInternalQueue !== 'function' || typeof manager?.setInternalPlayerQueue !== 'function'
          || typeof harmony?.getCurrentState !== 'function') {
        throw new Error('Spotify has no revision-checked, targeted queue change connection.');
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
          throw new Error('Spotify has no complete revision and current occurrence for a queue change.');
        }
        return value;
      };
      const target = deviceInfo(await activeTarget());
      if (expectedDeviceId !== undefined && target.id !== expectedDeviceId) {
        throw new Error('The active Spotify device differs from the expected reorder device.');
      }
      const before = await bridge.status();
      const initial = internal();
      const current = identity(initial.current_track);
      if (expectedCurrentUid !== undefined && current[0] !== expectedCurrentUid) {
        throw new Error('The current Spotify occurrence differs from the expected reorder occurrence.');
      }
      const revision = initial.revision;
      let next = initial.next_items.slice();
      const previous = initial.previous_items;
      const originalNext = signature(next);
      const originalPrevious = signature(previous);
      const checkStatus = status => {
        if (status.playbackState.source !== 'harmony') throw new Error('The active Spotify device has no complete playback clock.');
        if (status.playbackState.device && status.playbackState.device.id !== target.id) {
          throw new Error('The observed Spotify device changed during the queue change.');
        }
        if (!same(identity(status.nowPlaying), current)) throw new Error('The current Spotify occurrence changed during the queue change.');
      };
      checkStatus(before);
      const observedUids = before.queue.filter(item => item.source === 'queue').map(item => item.uid);
      if (!same(observedUids, expectedQueuedUids)
          || !same(explicit(next).map(item => item.uid), expectedQueuedUids)) {
        throw new Error('The explicit Spotify queue changed; observe its current order before reordering it.');
      }
      // Uniqueness belongs to explicit occurrences. Repeated context items and
      // delimiters stay intact and are never reconstructed from status.queue.
      const byUid = new Map();
      for (const id of expectedQueuedUids) {
        const matches = next.filter(item => item.provider === 'queue' && item.uid === id);
        if (matches.length !== 1 || matches[0].hidden_in_queue) {
          throw new Error('Every reordered occurrence must exist exactly once in the explicit queue.');
        }
        byUid.set(id, matches[0]);
      }
      let slot = 0;
      next = next.map(item => item.provider === 'queue' && !item.hidden_in_queue
        ? byUid.get(orderedQueuedUids[slot++]) : item);
      const desiredUids = explicit(next).map(item => item.uid);
      const desiredNext = signature(next);

      const raw = await harmony.getCurrentState();
      if (!same(identity(raw?.track_window?.current_track), current)) {
        throw new Error('The current Spotify occurrence changed before queue dispatch.');
      }
      if ((await activeTarget()).id !== target.id) throw new Error('The active Spotify device changed before queue dispatch.');
      ensureActualState();
      const checkUnchanged = () => {
        const latest = internal();
        if (latest.revision !== revision || !same(identity(latest.current_track), current)
            || !same(signature(latest.next_items), originalNext)
            || !same(signature(latest.previous_items), originalPrevious)) {
          throw new Error('The Spotify queue revision or current occurrence changed before queue dispatch.');
        }
      };
      checkUnchanged();
      if (manager._currentPlayerState?.restrictions?.disallow_set_queue_reasons?.length) {
        throw new Error('The active Spotify device currently forbids queue changes.');
      }
      if (same(desiredUids, expectedQueuedUids)) {
        const status = await bridge.status();
        checkStatus(status);
        if ((await activeTarget()).id !== target.id) throw new Error('The active Spotify device changed before the queue no-op was confirmed.');
        ensureActualState();
        checkUnchanged();
        if (!same(status.queue.filter(item => item.source === 'queue').map(item => item.uid), expectedQueuedUids)) {
          throw new Error('The explicit Spotify queue changed before the queue no-op was confirmed.');
        }
        return {ok: true, ...resultFields, moved: false, device: target, status};
      }
      // Use the awaitable manager behind the UI: the public reorder wrapper
      // drops this promise. Keep the server revision check and explicit target.
      dispatched = true;
      const accepted = await manager.setInternalPlayerQueue({revision, next_items: next, previous_items: previous},
        target.id, {ignoreRevision: false});
      if (accepted === false) throw new Error('Spotify did not accept the queue change for the active device.');
      const deadline = Date.now() + 8000;
      while (Date.now() < deadline) {
        if ((await activeTarget()).id !== target.id) throw new Error('The active Spotify device changed before the queue change was confirmed.');
        const status = await bridge.status();
        checkStatus(status);
        const observed = internal();
        if (!same(identity(observed.current_track), current) || !same(signature(observed.previous_items), originalPrevious)) {
          throw new Error('The current occurrence or playback history changed before the queue change was confirmed.');
        }
        const observedNext = signature(observed.next_items);
        const publicOrder = status.queue.filter(item => item.source === 'queue').map(item => item.uid);
        if (same(publicOrder, desiredUids) && same(observedNext, desiredNext)) {
          return {ok: true, ...resultFields, moved: true, device: target, status};
        }
        if ((!same(publicOrder, expectedQueuedUids) && !same(publicOrder, desiredUids))
            || (!same(observedNext, originalNext) && !same(observedNext, desiredNext))) {
          throw new Error('The Spotify queue changed concurrently before the queue change was confirmed.');
        }
        await new Promise(resolve => setTimeout(resolve, 250));
      }
      throw new Error('Spotify queue change was sent, but the observed queue did not confirm it.');
    } catch (error) {
      return {error: String(error?.message || error), dispatched};
    }
  }

  const operations = window.__laricsDJ?.operations || new Map();
  let searchJob = null;
  let legacySearchSequence = 0;
  const sameSearchRoute = destination => {
    try { return decodeURIComponent(location.pathname) === decodeURIComponent(destination); }
    catch { return false; }
  };
  const bridge = {
    version: 10,
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
      const positionAnchor = useRaw ? raw.position : number(state.positionAsOfTimestamp, state.position);
      const timestampAnchor = number(useRaw ? raw.timestamp : null, normalizedClock.timestamp);
      const held = useRaw ? raw.paused === true || raw.loading === true
        : state.isPaused === true || state.isBuffering === true;
      const clockValid = !!item && positionAnchor !== undefined && positionAnchor >= 0
        && (held || (timestampAnchor !== undefined && timestampAnchor > 0 && timestampAnchor <= stamp + 1000));
      // Preserve the existing numeric fields for callers while exposing whether
      // they came from a real anchor; a default zero/now is never a valid clock.
      const position = Math.max(0, number(positionAnchor, 0));
      const sourceTimestamp = number(timestampAnchor, stamp);
      const speed = number(useRaw ? raw.playback_speed : null, normalizedClock.speed, 1);
      const elapsed = playing && sourceTimestamp > 0 ? Math.max(0, stamp - sourceTimestamp) * Math.max(0, speed) : 0;
      const progress = Math.max(0, position + elapsed);
      const device = typeof connectDevices?.getActiveDevice === 'function' ? await connectDevices.getActiveDevice() : null;
      return {
        nowPlaying: item ? {...item, playing, paused, buffering, clockValid, durationMs: duration,
          progressMs: duration > 0 ? Math.min(duration, progress) : progress,
          observedAt: stamp} : null,
        queue: [...(queue.queued || []), ...(queue.nextUp || []).slice(0, 8)].map(normalize).filter(Boolean),
        explicitQueueComplete: true,
        playbackState: {source: useRaw ? 'harmony' : 'player-api', paused, buffering, clockValid,
          positionMs: position, sourceTimestamp, device: deviceInfo(device)},
        observedAt: stamp
      };
    },
    beginSearch(query, searchId, expiresAt = Date.now() + 16000) {
      if (typeof query !== 'string' || !query.trim() || query.length > 512
          || typeof searchId !== 'string' || !/^[A-Za-z0-9_-]{8,128}$/.test(searchId)) {
        throw new Error('Supply a catalogue query and a unique search ID.');
      }
      const stamp = Date.now();
      if (typeof expiresAt !== 'number' || !Number.isFinite(expiresAt) || expiresAt > stamp + 17000) {
        throw new Error('Supply a valid catalogue request deadline.');
      }
      if (expiresAt <= stamp) return {searchId, state: 'cancelled'};
      if (searchJob?.id === searchId) {
        if (searchJob.query !== query) throw new Error('This catalogue search ID belongs to another query.');
        return {searchId, state: 'pending'};
      }
      const destination = '/search/' + encodeURIComponent(query) + '/tracks';
      const previous = searchResults();
      const sameQuery = sameSearchRoute(destination);
      searchJob = {id: searchId, query, destination, previousId: previous?.requestId,
        sameQuery, deadline: Math.min(expiresAt, stamp + 16000)};
      try {
        if (!sameQuery) platform.getHistory().push(destination);
      } catch (error) {
        searchJob = null;
        throw error;
      }
      // No in-page wait, click or playback call is started. The client can
      // observe the player or dispatch an explicit command before the next poll.
      return {searchId, state: 'pending'};
    },
    pollSearch(searchId) {
      const job = searchJob;
      if (!job || job.id !== searchId) return {searchId, state: 'cancelled'};
      if (!sameSearchRoute(job.destination)) {
        searchJob = null;
        return {searchId, state: 'cancelled'};
      }
      if (Date.now() >= job.deadline) {
        searchJob = null;
        return {searchId, state: 'error', error: 'Spotify search is still loading. Try again in a moment.'};
      }
      const results = searchResults();
      const list = document.querySelector('[data-testid="track-list"]');
      const label = list?.getAttribute('aria-label') || '';
      const labelMatches = label.endsWith(`“${job.query}”`) || label.endsWith(`"${job.query}"`);
      const row = list?.querySelector('[data-testid="tracklist-row"]');
      if (labelMatches && row && results && (job.sameQuery || results.requestId !== job.previousId)) {
        let fiber = row[Object.keys(row).find(key => key.startsWith('__reactFiber$'))];
        for (let i = 0; fiber && i < 45; ++i, fiber = fiber.return) {
          const tracks = fiber.memoizedProps?.tracks;
          if (Array.isArray(tracks) && tracks.length) {
            searchJob = null;
            return {searchId, state: 'complete', tracks: tracks.filter(t => t.isPlayable !== false).slice(0, 12).map(normalize)};
          }
        }
      }
      const text = document.querySelector('main')?.innerText || '';
      if (text.includes(job.query) && /No results found for|No results for/.test(text)) {
        searchJob = null;
        return {searchId, state: 'complete', tracks: []};
      }
      return {searchId, state: 'pending'};
    },
    cancelSearch(searchId) {
      const cancelled = searchJob?.id === searchId;
      if (cancelled) searchJob = null;
      return {searchId, state: 'cancelled', cancelled};
    },
    async search(query) {
      // Compatibility for existing callers. SpotifyClient uses the short
      // methods above so waiting never occupies its shared transport lock.
      const searchId = `legacy-${Date.now()}-${++legacySearchSequence}`;
      bridge.beginSearch(query, searchId);
      try {
        while (true) {
          const result = bridge.pollSearch(searchId);
          if (result.state === 'complete') return result.tracks;
          if (result.state === 'cancelled') throw new Error('This catalogue search was superseded.');
          if (result.state === 'error') throw new Error(result.error);
          await new Promise(resolve => setTimeout(resolve, 180));
        }
      } finally {
        bridge.cancelSearch(searchId);
      }
    },
    async trackInQueue(uri) {
      if (!/^spotify:track:[A-Za-z0-9]{22}$/.test(uri)) throw new Error('Invalid track.');
      const q = await player.getQueue();
      const known = [q.current, ...(q.queued || []), ...(q.nextUp || [])].find(t => t?.uri === uri);
      return known ? normalize(known) : null;
    },
    async track(uri) {
      const known = await bridge.trackInQueue(uri);
      if (known) return known;
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
          if (browserController) ensureActualState();
          before = await player.getQueue();
          if (!before || !Array.isArray(before.queued)) throw new Error('Spotify has not supplied its current explicit queue.');
          if (browserController) ensureActualState();
        } catch (error) {
          return {error: String(error?.message || error), dispatched: false};
        }
        const validUid = value => typeof value === 'string' && value.trim().length > 0;
        const initialCurrentUid = before.current?.uid;
        const previousCount = before.queued.filter(t => t.uri === uri).length;
        const previousUids = new Set(before.queued.map(t => t.uid));
        previousUids.add(initialCurrentUid);
        try {
          const accepted = await player.addToQueue([{uri}]);
          if (accepted === false) throw new Error('Spotify did not acknowledge the queue addition; inspect its actual queue.');
          const deadline = Date.now() + 6000;
          while (Date.now() < deadline) {
            if (browserController) ensureActualState();
            const after = await player.getQueue();
            if (browserController) ensureActualState();
            if (!after || !Array.isArray(after.queued)) throw new Error('Spotify returned an incomplete queue after the addition.');
            const matching = after.queued.filter(t => t.uri === uri);
            const added = matching.filter(t => validUid(t.uid) && !previousUids.has(t.uid));
            if (matching.length > previousCount && added.length === 1) {
              return {queued: true, uri, requestId, uid: added[0].uid};
            }
            const current = after.current;
            if (validUid(initialCurrentUid) && current?.uri === uri && validUid(current.uid)
                && current.uid !== initialCurrentUid && !previousUids.has(current.uid)) {
              return {queued: true, uri, requestId, uid: current.uid};
            }
            await new Promise(resolve => setTimeout(resolve, 180));
          }
          throw new Error('Spotify received the queue command, but a new requested occurrence was not confirmed.');
        } catch (error) {
          return {error: String(error?.message || error), dispatched: true};
        }
      })();
      operations.set(requestId, pending);
      if (operations.size > 500) operations.delete(operations.keys().next().value);
      const result = await pending;
      if (result?.dispatched === false) operations.delete(requestId);
      return result;
    },
    async moveQueue(uid, beforeUid, expectedQueuedUids) {
      if (typeof uid !== 'string' || !uid || typeof beforeUid !== 'string' || !beforeUid || uid === beforeUid
          || !Array.isArray(expectedQueuedUids) || expectedQueuedUids.length < 2
          || !expectedQueuedUids.includes(uid) || !expectedQueuedUids.includes(beforeUid)) {
        return {error: 'Choose two distinct explicit queue occurrence UIDs for the move.', dispatched: false};
      }
      const ordered = expectedQueuedUids.filter(id => id !== uid);
      ordered.splice(ordered.indexOf(beforeUid), 0, uid);
      return applyQueueOrder(ordered, expectedQueuedUids, undefined, undefined, {action: 'moveQueue', uid, beforeUid});
    },
    async reorderQueue(orderedQueuedUids, expectedQueuedUids, expectedCurrentUid, expectedDeviceId) {
      return applyQueueOrder(orderedQueuedUids, expectedQueuedUids, expectedCurrentUid, expectedDeviceId, {action: 'reorderQueue'});
    },
    async setPaused(paused, expectedCurrentUid, expectedDeviceId) {
      let dispatched = false;
      try {
        const validId = value => typeof value === 'string' && value.trim().length > 0;
        if (typeof paused !== 'boolean' || !validId(expectedCurrentUid) || !validId(expectedDeviceId)) {
          throw new Error('Playback requires an explicit paused boolean, current occurrence UID and active device ID.');
        }
        const action = paused ? 'pause' : 'resume';
        if (typeof harmony?.[action] !== 'function' || typeof harmony?.getCurrentState !== 'function'
            || typeof player._queue?.getInternalQueue !== 'function') {
          throw new Error('Spotify has no targeted, occurrence-checked playback connection.');
        }
        const target = deviceInfo(await activeTarget());
        if (target.id !== expectedDeviceId) throw new Error('The active Spotify device differs from the expected playback device.');
        let uri;
        const checkStatus = (status, admission = false) => {
          const playback = status?.playbackState, current = status?.nowPlaying;
          if (playback?.source !== 'harmony' || typeof playback.paused !== 'boolean'
              || typeof current?.paused !== 'boolean' || playback.paused !== current.paused) {
            throw new Error('The active Spotify device has no explicit actual pause state.');
          }
          if (playback.device?.id !== expectedDeviceId) throw new Error('The observed Spotify device changed during playback control.');
          if (current.uid !== expectedCurrentUid || !current.uri || (uri && current.uri !== uri)) {
            throw new Error('The current Spotify occurrence changed during playback control.');
          }
          uri = current.uri;
          if (admission && (playback.buffering || current.buffering
              || playback.clockValid !== true || current.clockValid !== true)) {
            throw new Error('The actual Spotify playback state is buffering or has no valid clock; observe it again before controlling it.');
          }
          return playback;
        };
        const checkCurrent = () => {
          // This synchronous final guard covers a track change while the last
          // active-device read was awaiting its result.
          const current = player._queue.getInternalQueue()?.current_track;
          if (current?.uid !== expectedCurrentUid || current.uri !== uri) {
            throw new Error('The current Spotify occurrence changed before playback control could be confirmed.');
          }
        };
        checkStatus(await bridge.status(), true);
        // Resample even for a no-op: publish the actual latest clock and pause
        // state, while retaining the caller's exact occurrence/device guards.
        const latest = await bridge.status();
        const playback = checkStatus(latest, true);
        if ((await activeTarget()).id !== expectedDeviceId) throw new Error('The active Spotify device changed before playback dispatch.');
        ensureActualState();
        checkCurrent();
        if (playback.paused === paused) {
          return {ok: true, action: 'setPaused', paused, changed: false, device: target, status: latest};
        }
        dispatched = true;
        const accepted = await harmony[action](target.id, {forceRemoteAction: true});
        if (accepted === false) throw new Error(`Spotify did not accept ${action} for the active device.`);
        const deadline = Date.now() + 8000;
        while (Date.now() < deadline) {
          const status = await bridge.status();
          const observed = checkStatus(status);
          if ((await activeTarget()).id !== expectedDeviceId) throw new Error('The active Spotify device changed before playback could be confirmed.');
          ensureActualState();
          checkCurrent();
          if (observed.paused === paused && !observed.buffering && !status.nowPlaying.buffering
              && observed.clockValid === true && status.nowPlaying.clockValid === true) {
            return {ok: true, action: 'setPaused', paused, changed: true, device: target, status};
          }
          await new Promise(resolve => setTimeout(resolve, 250));
        }
        throw new Error(`Spotify ${action} was sent, but the actual player's state did not confirm it.`);
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
