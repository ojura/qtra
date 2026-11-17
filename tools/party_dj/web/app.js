'use strict';

(() => {
  const byId = (id) => document.getElementById(id);
  const elements = Object.fromEntries([
    'connection', 'connection-text', 'room-notice', 'search', 'search-form',
    'search-status', 'search-spinner', 'results', 'search-empty', 'request-name',
    'dedication', 'room-mood', 'mood-feedback', 'message-form', 'message',
    'message-status', 'now-art', 'now-title', 'now-artist', 'track-progress',
    'progress-fill', 'elapsed', 'duration', 'now-spotify', 'queue', 'queue-count',
    'queue-empty', 'book-panel', 'book-image', 'book-title', 'book-caption',
    'book-page', 'book-source', 'toasts', 'chat-log', 'chat-empty', 'chat-name', 'chat-new',
    'chat-unread', 'dj-presence', 'dj-presence-text'
  ].map((id) => [id, byId(id)]));
  const moodNames = { chill: 'Chill', groove: 'Groove', dance: 'Dance', chaos: 'Chaos', singalong: 'Sing along' };
  const stored = {
    get(key) { try { return localStorage.getItem('larics.dj.' + key); } catch { return null; } },
    set(key, value) { try { localStorage.setItem('larics.dj.' + key, value); } catch { /* The page works without storage. */ } }
  };
  function uuid() {
    if (globalThis.crypto?.randomUUID) return crypto.randomUUID();
    const bytes = new Uint8Array(16);
    if (globalThis.crypto?.getRandomValues) crypto.getRandomValues(bytes);
    else bytes.forEach((_, i) => { bytes[i] = Math.floor(Math.random() * 256); });
    bytes[6] = (bytes[6] & 15) | 64;
    bytes[8] = (bytes[8] & 63) | 128;
    const hex = Array.from(bytes, (byte) => byte.toString(16).padStart(2, '0')).join('');
    return [hex.slice(0, 8), hex.slice(8, 12), hex.slice(12, 16), hex.slice(16, 20), hex.slice(20)].join('-');
  }
  const guestId = stored.get('guest') || uuid();
  stored.set('guest', guestId);
  elements['request-name'].value = elements['chat-name'].value = stored.get('name') || '';
  function saveName(source) {
    const name = source.value.trim();
    elements['request-name'].value = elements['chat-name'].value = name;
    stored.set('name', name);
  }
  elements['request-name'].addEventListener('change', () => saveName(elements['request-name']));
  elements['chat-name'].addEventListener('change', () => saveName(elements['chat-name']));

  let latestState = null;
  let receivedAt = 0;
  let stateTimer = null;
  let searchTimer = null;
  let searchController = null;
  let searchSequence = 0;
  let queueSignature = '';
  let artSignature = '';
  let bookSignature = '';
  let selectedMood = stored.get('mood') || '';
  let moodPending = false;
  let messagePending = false;
  let pendingMessage = null;
  let chatCursor = '';
  let chatTimer = null;
  let chatPending = false;
  const chatMessages = new Map();
  const requests = new Map();
  let requestButtons = new Map();
  try {
    const previous = JSON.parse(stored.get('requests') || '{}');
    Object.entries(previous).forEach(([uri, value]) => {
      if (typeof value?.requestId === 'string' && Date.now() - value.createdAt < 12 * 60 * 60 * 1000) {
        requests.set(uri, { ...value, pending: false });
      }
    });
  } catch { /* Ignore a damaged local request cache. */ }

  function saveRequests() {
    stored.set('requests', JSON.stringify(Object.fromEntries(requests)));
  }
  function safeImage(value) {
    if (typeof value !== 'string' || !value) return null;
    try {
      const url = new URL(value, location.href);
      return ['http:', 'https:'].includes(url.protocol) ? url.href : null;
    } catch { return null; }
  }
  function spotifyUrl(track) {
    const uri = String(track?.uri || '');
    const id = uri.startsWith('spotify:track:') ? uri.slice(14) : String(track?.id || '');
    return /^[a-zA-Z0-9]{22}$/.test(id) ? 'https://open.spotify.com/track/' + id : null;
  }
  function textNode(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    node.textContent = String(text ?? '');
    return node;
  }
  function artNode(url, className, alt = '') {
    const wrapper = textNode('div', className, '♫');
    const source = safeImage(url);
    if (source) {
      const img = new Image();
      img.alt = alt;
      img.loading = 'lazy';
      img.referrerPolicy = 'no-referrer';
      img.addEventListener('error', () => { img.remove(); wrapper.textContent = '♫'; }, { once: true });
      img.src = source;
      wrapper.replaceChildren(img);
    }
    return wrapper;
  }
  function duration(ms) {
    const seconds = Math.max(0, Math.floor(Number(ms || 0) / 1000));
    return Math.floor(seconds / 60) + ':' + String(seconds % 60).padStart(2, '0');
  }
  function toast(message, tone = 'success') {
    const item = textNode('div', 'toast', message);
    item.dataset.tone = tone;
    elements.toasts.append(item);
    while (elements.toasts.children.length > 3) elements.toasts.firstChild.remove();
    setTimeout(() => item.remove(), 6000);
  }
  function readableError(value) {
    const message = String(value || 'The DJ desk could not complete that request.')
      .split(/\r?\n/, 1)[0].replace(/^Error:\s*/, '');
    if (/returned (?:HTTP )?429\b/.test(message)) return 'Spotify is busy. Give it a moment and try again.';
    return message.slice(0, 300);
  }
  async function api(url, options = {}) {
    const controller = options.signal ? null : new AbortController();
    const timeout = controller && setTimeout(() => controller.abort(), 12000);
    try {
      const response = await fetch(url, { cache: 'no-store', ...options, signal: options.signal || controller.signal });
      let body;
      try { body = await response.json(); } catch { throw new Error('The DJ desk returned an unreadable response. Please try again.'); }
      if (!response.ok || body?.ok === false) throw new Error(readableError(body?.error || body?.message));
      return body;
    } finally { if (timeout) clearTimeout(timeout); }
  }
  function post(url, body) {
    return api(url, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
  }
  function queued(uri) {
    if (latestState?.nowPlaying?.requestedUri === uri) return true;
    return Array.isArray(latestState?.queue) && latestState.queue.some((item) =>
      item.uri === uri && !['played', 'failed', 'rejected', 'cancelled', 'canceled', 'removed'].includes(item.status));
  }
  function updateRequestButton(button, track) {
    const state = requests.get(track.uri);
    const sent = queued(track.uri) || state?.accepted;
    button.disabled = Boolean(state?.pending || sent || !track.uri);
    button.dataset.status = state?.pending ? 'pending' : sent ? 'sent' : 'ready';
    button.textContent = state?.pending ? 'Sending…' : sent ? 'Requested ✓' : state?.uncertain ? 'Try again' : 'Request';
    button.setAttribute('aria-label', sent ? track.title + ' has been requested' : 'Request ' + track.title + ' by ' + track.artist);
  }
  function syncRequestButtons() {
    requestButtons.forEach(({ button, track }) => updateRequestButton(button, track));
  }
  async function requestTrack(track) {
    const current = requests.get(track.uri);
    if (!track.uri || current?.pending || current?.accepted || queued(track.uri)) return;
    const state = current || {
      requestId: uuid(), createdAt: Date.now(),
      requestedBy: elements['request-name'].value.trim(),
      dedication: elements.dedication.value.trim()
    };
    state.pending = true;
    state.uncertain = false;
    requests.set(track.uri, state);
    saveRequests();
    syncRequestButtons();
    try {
      const answer = await post('/api/request', {
        requestId: state.requestId, uri: track.uri,
        requestedBy: state.requestedBy, dedication: state.dedication
      });
      if (['failed', 'rejected', 'error'].includes(answer.status)) throw new Error(answer.error || answer.message || 'That song could not be requested.');
      state.pending = false;
      state.accepted = true;
      state.acceptedAt = Date.now();
      saveRequests();
      syncRequestButtons();
      toast('“' + track.title + '” is on the request list.');
      void pollState();
    } catch (error) {
      state.pending = false;
      state.uncertain = true;
      saveRequests();
      syncRequestButtons();
      toast(error.name === 'AbortError' ? 'The connection is taking a moment. Try again to check the same request.' : error.message, 'error');
    }
  }
  function renderResults(tracks) {
    requestButtons = new Map();
    const fragment = document.createDocumentFragment();
    for (const track of tracks) {
      const row = textNode('li', 'track-row', '');
      row.append(artNode(track.artUrl, 'track-art'));
      const info = textNode('div', 'track-info', '');
      info.append(textNode('span', 'track-title', track.title || 'Untitled track'));
      info.append(textNode('div', 'track-artist', track.artist || 'Unknown artist'));
      const meta = textNode('div', 'track-meta', duration(track.durationMs));
      const url = spotifyUrl(track);
      if (url) {
        const link = textNode('a', '', 'Spotify ↗');
        link.href = url; link.target = '_blank'; link.rel = 'noopener noreferrer';
        link.setAttribute('aria-label', 'Open ' + (track.title || 'track') + ' in Spotify');
        meta.append(link);
      }
      info.append(meta);
      const button = textNode('button', 'request-button', 'Request');
      button.type = 'button';
      button.addEventListener('click', () => void requestTrack(track));
      requestButtons.set(track.uri || track.id, { button, track });
      updateRequestButton(button, track);
      row.append(info, button);
      fragment.append(row);
    }
    elements.results.replaceChildren(fragment);
    elements['search-empty'].hidden = tracks.length > 0;
  }
  async function search() {
    clearTimeout(searchTimer);
    if (searchController) searchController.abort();
    const sequence = ++searchSequence;
    const query = elements.search.value.trim();
    if (!query) {
      renderResults([]);
      elements['search-spinner'].hidden = true;
      elements['search-status'].textContent = 'Search the Spotify catalogue, then tap Request.';
      elements['search-empty'].querySelector('p').textContent = 'The next good idea could be yours.';
      return;
    }
    const controller = new AbortController();
    searchController = controller;
    elements['search-spinner'].hidden = false;
    elements['search-status'].textContent = 'Looking for “' + query + '”…';
    const timeout = setTimeout(() => controller.abort(), 15000);
    try {
      const answer = await api('/api/search?q=' + encodeURIComponent(query), { signal: controller.signal });
      if (sequence !== searchSequence) return;
      if (answer.error) throw new Error(answer.error);
      const tracks = Array.isArray(answer.tracks) ? answer.tracks : [];
      renderResults(tracks);
      elements['search-status'].textContent = tracks.length ? tracks.length + ' songs found. Tap Request to send one.' : 'Nothing for “' + query + '” yet.';
      elements['search-empty'].querySelector('p').textContent = tracks.length ? '' : 'Try a different title or artist.';
    } catch (error) {
      if (sequence !== searchSequence) return;
      renderResults([]);
      elements['search-status'].textContent = error.name === 'AbortError' ? 'Search is taking a moment. Tap Find to try again.' : error.message;
      elements['search-empty'].querySelector('p').textContent = 'Your next song is worth another try.';
    } finally {
      clearTimeout(timeout);
      if (sequence === searchSequence) elements['search-spinner'].hidden = true;
    }
  }
  elements['search-form'].addEventListener('submit', (event) => { event.preventDefault(); void search(); });
  elements.search.addEventListener('input', () => {
    clearTimeout(searchTimer);
    if (searchController) searchController.abort();
    ++searchSequence;
    searchTimer = setTimeout(() => void search(), 280);
  });
  document.querySelectorAll('[data-search]').forEach((button) => button.addEventListener('click', () => {
    elements.search.value = button.dataset.search;
    elements.search.focus();
    void search();
  }));

  function renderQueue(queue) {
    const entries = queue.filter((item) => !['played', 'removed', 'cancelled', 'canceled'].includes(item.status));
    const signature = JSON.stringify(entries);
    if (signature === queueSignature) return;
    queueSignature = signature;
    const fragment = document.createDocumentFragment();
    entries.forEach((track, index) => {
      const row = textNode('li', 'track-row', '');
      row.append(textNode('span', 'queue-position', String(index + 1).padStart(2, '0')));
      const info = textNode('div', 'track-info', '');
      info.append(textNode('span', 'track-title', track.title || 'Finding your song…'));
      const description = [track.artist, track.requestedBy ? 'for ' + track.requestedBy : ''].filter(Boolean).join(' / ');
      if (description) info.append(textNode('div', 'track-artist', description));
      const labels = { queued: 'Added to Spotify', playing: 'Playing now', pending: 'With the DJ', accepted: 'With the DJ', sending: 'Adding to Spotify…', processing: 'Adding to Spotify…', uncertain: 'Checking Spotify…', failed: 'Could not add this song', rejected: 'Request declined' };
      const label = textNode('div', 'queue-status', labels[track.status] || 'With the DJ');
      label.dataset.status = track.status || 'pending';
      info.append(label);
      row.append(info);
      fragment.append(row);
    });
    elements.queue.replaceChildren(fragment);
    elements['queue-count'].textContent = entries.length;
    elements['queue-empty'].hidden = entries.length > 0;
  }
  function renderNow(track) {
    const current = track || {};
    elements['now-title'].textContent = current.title || 'The night is warming up';
    elements['now-artist'].textContent = current.artist || (latestState?.spotify?.connected ? 'Pick the next song with us' : 'Waiting for Spotify');
    document.body.classList.toggle('playing', Boolean(current.playing));
    const signature = current.artUrl || '';
    if (signature !== artSignature) {
      artSignature = signature;
      const art = artNode(signature, 'now-art', current.album ? 'Album artwork for ' + current.album : 'Current album artwork');
      elements['now-art'].replaceChildren(...art.childNodes);
    }
    const url = spotifyUrl(current);
    elements['now-spotify'].hidden = !url;
    if (url) elements['now-spotify'].href = url;
    updateProgress();
  }
  function updateProgress() {
    const track = latestState?.nowPlaying;
    const total = Math.max(0, Number(track?.durationMs) || 0);
    let observedAt = track?.observedAt;
    if (typeof observedAt === 'number' && observedAt < 1e12) observedAt *= 1000;
    if (typeof observedAt === 'string') observedAt = Date.parse(observedAt);
    const anchor = Number.isFinite(observedAt) ? observedAt : receivedAt;
    const advance = track?.playing ? Math.max(0, Date.now() - anchor) : 0;
    const elapsed = Math.min(total || Infinity, Math.max(0, Number(track?.progressMs) || 0) + advance);
    const percent = total ? Math.min(100, elapsed / total * 100) : 0;
    elements['progress-fill'].style.width = percent + '%';
    elements['track-progress'].setAttribute('aria-valuenow', String(Math.round(percent)));
    elements['track-progress'].setAttribute('aria-valuetext', duration(elapsed) + ' of ' + duration(total));
    elements.elapsed.textContent = duration(elapsed);
    elements.duration.textContent = duration(total);
  }
  function renderBook(moment) {
    const signature = JSON.stringify(moment || null);
    if (signature === bookSignature) return;
    bookSignature = signature;
    const imageUrl = safeImage(moment?.imageUrl || moment?.url);
    elements['book-panel'].hidden = !moment || !imageUrl;
    if (!moment || !imageUrl) return;
    elements['book-image'].src = imageUrl;
    elements['book-image'].alt = String(moment.alt || moment.title || '');
    elements['book-title'].textContent = moment.title || 'Thirty years of stories';
    elements['book-caption'].textContent = moment.caption || moment.text || '';
    elements['book-page'].textContent = moment.page ? 'Spomenar, p. ' + moment.page : '';
    const sourceUrl = safeImage(moment.sourceUrl);
    elements['book-source'].hidden = !sourceUrl;
    if (sourceUrl) elements['book-source'].href = sourceUrl;
  }
  let statePending = false;
  async function pollState() {
    clearTimeout(stateTimer);
    if (statePending) return;
    statePending = true;
    try {
      const state = await api('/api/state?guestId=' + encodeURIComponent(guestId));
      latestState = state;
      receivedAt = Date.now();
      elements.connection.dataset.state = state.spotify?.connected ? 'online' : 'connecting';
      elements['connection-text'].textContent = state.spotify?.connected ? 'You’re in the room' : 'DJ desk connected';
      const notice = state.spotify?.error;
      elements['room-notice'].hidden = !notice;
      elements['room-notice'].textContent = notice ? readableError(notice) : '';
      renderNow(state.nowPlaying);
      renderQueue(Array.isArray(state.queue) ? state.queue : []);
      if (state.spotify?.connected) {
        let changed = false;
        for (const [uri, request] of requests) {
          const present = queued(uri) || state.nowPlaying?.uri === uri;
          if (present && request.accepted && !request.observed) {
            request.observed = true;
            changed = true;
          }
          if (request.accepted && !present && (request.observed || Date.now() - (request.acceptedAt || request.createdAt) > 15000)) {
            requests.delete(uri);
            changed = true;
          }
        }
        if (changed) saveRequests();
      }
      syncRequestButtons();
      elements['room-mood'].textContent = moodNames[state.mood?.label] || state.mood?.label || 'Finding the groove';
      renderBook(state.bookMoment);
    } catch {
      elements.connection.dataset.state = 'offline';
      elements['connection-text'].textContent = 'Reconnecting';
      elements['room-notice'].hidden = false;
      elements['room-notice'].textContent = 'The DJ desk is reconnecting. Your requests stay here.';
      document.body.classList.remove('playing');
    } finally {
      statePending = false;
      if (!document.hidden) stateTimer = setTimeout(() => void pollState(), 2000);
    }
  }
  function paintMood() {
    document.querySelectorAll('[data-mood]').forEach((button) => {
      button.setAttribute('aria-pressed', String(button.dataset.mood === selectedMood));
      button.disabled = moodPending;
    });
  }
  paintMood();
  document.querySelectorAll('[data-mood]').forEach((button) => button.addEventListener('click', async () => {
    if (moodPending) return;
    const mood = button.dataset.mood;
    moodPending = true; paintMood();
    elements['mood-feedback'].textContent = 'Passing that to the DJ…';
    try {
      await post('/api/mood', { guestId, mood });
      selectedMood = mood;
      stored.set('mood', mood);
      elements['mood-feedback'].textContent = mood === 'chaos' ? 'A considered request for chaos. Noted.' : moodNames[mood] + '. The DJ got the hint.';
      void pollState();
    } catch (error) {
      elements['mood-feedback'].textContent = error.name === 'AbortError' ? 'That took too long. Tap your mood again.' : error.message;
    } finally { moodPending = false; paintMood(); }
  }));
  function chatTime(value) {
    let number = value;
    if (typeof value === 'number' && value < 1e12) number = value * 1000;
    const date = new Date(number);
    return Number.isFinite(date.getTime()) ? date.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' }) : '';
  }
  function scrollChat() {
    elements['chat-log'].scrollTop = elements['chat-log'].scrollHeight;
    elements['chat-new'].hidden = true;
    elements['chat-unread'].hidden = true;
  }
  function appendMessages(messages, own = false) {
    const log = elements['chat-log'];
    const atBottom = log.scrollHeight - log.scrollTop - log.clientHeight < 75;
    let added = false;
    for (const message of messages) {
      if (message?.id === undefined || message?.id === null) continue;
      const key = String(message.id);
      if (chatMessages.has(key)) {
        const old = chatMessages.get(key);
        if (message.status && old.status !== message.status) {
          old.status = message.status;
          old.node.querySelector('.chat-delivery').textContent = message.status === 'read' ? 'Read by DJ' : '';
        }
        continue;
      }
      const assistant = message.role === 'assistant';
      const bubble = textNode('article', 'chat-bubble ' + (assistant ? 'chat-dj' : 'chat-guest'), '');
      const meta = textNode('div', 'chat-meta', '');
      meta.append(textNode('strong', '', assistant ? (message.name || 'Your DJ') : (message.name || 'Guest')));
      meta.append(textNode('time', '', chatTime(message.createdAt)));
      bubble.append(meta, textNode('p', 'chat-text', message.text || ''));
      bubble.append(textNode('span', 'chat-delivery', message.status === 'read' ? 'Read by DJ' : ''));
      let rawTime = message.createdAt;
      if (typeof rawTime === 'number' && rawTime < 1e12) rawTime *= 1000;
      const stamp = typeof rawTime === 'number' ? rawTime : Date.parse(rawTime || '');
      const order = /^\d+$/.test(key) ? Number(key) : 0;
      const next = [...chatMessages.values()]
        .filter((entry) => entry.stamp > stamp || (entry.stamp === stamp && entry.order > order))
        .sort((a, b) => a.stamp - b.stamp || a.order - b.order)[0];
      if (next) log.insertBefore(bubble, next.node);
      else log.append(bubble);
      chatMessages.set(key, { node: bubble, status: message.status, stamp, order });
      added = true;
    }
    elements['chat-empty'].hidden = chatMessages.size > 0;
    if (added) {
      if (atBottom || own) scrollChat();
      else { elements['chat-new'].hidden = false; elements['chat-unread'].hidden = false; }
    }
  }
  function paintPresence(dj) {
    let lastSeen = dj?.lastSeen;
    if (typeof lastSeen === 'number' && lastSeen < 1e12) lastSeen *= 1000;
    const stamp = typeof lastSeen === 'number' ? lastSeen : Date.parse(lastSeen || '');
    const recent = Number.isFinite(stamp) && Date.now() - stamp < 90000;
    const status = String(dj?.status || '');
    const active = recent && !['offline', 'away', 'stopped'].includes(status);
    elements['dj-presence'].dataset.state = active ? 'online' : 'away';
    elements['dj-presence-text'].textContent = active ? (['thinking', 'working'].includes(status) ? 'DJ is on it' : 'DJ is here') : 'Leave the DJ a message';
  }
  async function pollChat() {
    clearTimeout(chatTimer);
    if (chatPending) return;
    chatPending = true;
    try {
      const reply = await api('/api/chat' + (chatCursor ? '?after=' + encodeURIComponent(chatCursor) : ''));
      const messages = Array.isArray(reply.messages) ? reply.messages : [];
      appendMessages(messages);
      const last = messages[messages.length - 1];
      if (last?.id !== undefined && last?.id !== null) chatCursor = String(last.id);
      paintPresence(reply.dj);
    } catch {
      elements['dj-presence'].dataset.state = 'away';
      elements['dj-presence-text'].textContent = 'Chat reconnecting';
    } finally {
      chatPending = false;
      if (!document.hidden) chatTimer = setTimeout(() => void pollChat(), 2000);
    }
  }
  elements['chat-new'].addEventListener('click', scrollChat);
  elements['chat-log'].addEventListener('scroll', () => {
    const log = elements['chat-log'];
    if (log.scrollHeight - log.scrollTop - log.clientHeight < 75) {
      elements['chat-new'].hidden = true;
      elements['chat-unread'].hidden = true;
    }
  }, { passive: true });
  elements.message.addEventListener('keydown', (event) => {
    if (event.key === 'Enter' && !event.shiftKey && !event.isComposing) {
      event.preventDefault();
      elements['message-form'].requestSubmit();
    }
  });
  elements['message-form'].addEventListener('submit', async (event) => {
    event.preventDefault();
    const content = elements.message.value.trim();
    if (!content || messagePending) return;
    if (!pendingMessage || pendingMessage.text !== content) {
      pendingMessage = { guestId, name: elements['chat-name'].value.trim(), text: content, requestId: uuid() };
    }
    messagePending = true;
    const button = elements['message-form'].querySelector('button');
    button.disabled = true;
    elements['message-status'].textContent = 'Sending…';
    try {
      const response = await post('/api/message', pendingMessage);
      if (response.message) appendMessages([response.message], true);
      if (elements.message.value.trim() === content) elements.message.value = '';
      pendingMessage = null;
      elements['message-status'].textContent = 'Delivered. Replies appear above.';
      void pollChat();
    } catch (error) {
      elements['message-status'].textContent = error.name === 'AbortError' ? 'The connection is slow. Your message is still here.' : error.message;
    } finally { messagePending = false; button.disabled = false; }
  });
  document.addEventListener('visibilitychange', () => {
    clearTimeout(stateTimer);
    clearTimeout(chatTimer);
    if (!document.hidden) { void pollState(); void pollChat(); }
  });
  setInterval(() => { if (!document.hidden) updateProgress(); }, 1000);
  void pollState();
  void pollChat();
})();
