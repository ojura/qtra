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
  const moodNames = { chill: 'Lagano', groove: 'Ritam', dance: 'Ples', chaos: 'Kaos', singalong: 'Pjevamo' };
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
  let presenceLease = null;
  let presenceAvailable = false;
  let presenceConnection = 'connecting';
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
    const message = String(value?.message || value || '')
      .split(/\r?\n/, 1)[0].replace(/^Error:\s*/, '');
    const known = {
      'Expected text.': 'Upiši tekst pa pokušaj ponovno.',
      'A little breathing room, please. Try again in a moment.': 'Samo trenutak predaha. Pokušaj ponovno malo kasnije.',
      'Choose an exact Spotify track URI.': 'Odaberi pjesmu iz rezultata pretrage.',
      'Choose a song from the search results.': 'Odaberi pjesmu iz rezultata pretrage.',
      'This request ID belongs to a different song.': 'Ovaj je zahtjev već vezan uz drugu pjesmu. Ponovi pretragu i odaberi pjesmu.',
      'This message ID was already used.': 'Ova je poruka već poslana. Provjeri razgovor prije novog slanja.',
      'Write a message first.': 'Najprije napiši poruku.',
      'Choose one of the mood buttons.': 'Odaberi jednu od ponuđenih atmosfera.',
      'A guest ID is required.': 'Nedostaje oznaka gosta. Ponovno otvori ovu stranicu.',
      'Invalid message size.': 'Poruka je preduga. Skrati je pa pokušaj ponovno.',
      'Expected a JSON object.': 'Zahtjev nije ispravno oblikovan. Osvježi stranicu pa pokušaj ponovno.',
      'That song could not be requested.': 'Pjesmu nije bilo moguće zatražiti. Pokušaj ponovno.',
      'Spotify search is still loading. Try again in a moment.': 'Spotify još učitava pretragu. Pokušaj ponovno za trenutak.',
      'The DJ desk returned an unreadable response. Please try again.': 'DJ pult vratio je nečitljiv odgovor. Pokušaj ponovno.'
    };
    if (Object.prototype.hasOwnProperty.call(known, message)) return known[message];
    const length = message.match(/^A message can contain at most (\d+) characters\.$/);
    if (length) return 'Poruka može sadržavati najviše ' + length[1] + ' znakova.';
    const field = message.match(/^(name|requestedBy|dedication|text) must be text of at most (\d+) characters\.$/);
    if (field) return ({ name: 'Ime', requestedBy: 'Ime', dedication: 'Posveta', text: 'Tekst' })[field[1]]
      + ' može sadržavati najviše ' + field[2] + ' znakova.';
    if (value?.status === 429 || /(?:returned (?:HTTP )?429\b|too many requests|rate.?limit)/i.test(message))
      return 'Previše zahtjeva odjednom. Pričekaj trenutak pa pokušaj ponovno.';
    if (value?.status === 401 || value?.status === 403 || /(?:unauthorized|forbidden|authentication)/i.test(message))
      return 'Pristup trenutačno nije dostupan. Ponovno otvori DJ stranicu.';
    if (/(?:timed? ?out|timeout|taking too long)/i.test(message))
      return 'Odgovor kasni. Pričekaj trenutak pa pokušaj ponovno.';
    if (/(?:not connected|disconnected|connection|network|failed to fetch|fetch failed|unavailable)/i.test(message))
      return 'Veza s DJ-em ili Spotifyjem trenutačno nije dostupna. Pokušaj ponovno za trenutak.';
    return 'Zahtjev se nije mogao dovršiti. Pokušaj ponovno.';
  }
  async function api(url, options = {}) {
    const controller = options.signal ? null : new AbortController();
    const timeout = controller && setTimeout(() => controller.abort(), 12000);
    try {
      const response = await fetch(url, { cache: 'no-store', ...options, signal: options.signal || controller.signal });
      let body;
      try { body = await response.json(); } catch { throw new Error('The DJ desk returned an unreadable response. Please try again.'); }
      if (!response.ok || body?.ok === false) {
        const error = new Error(body?.error || body?.message || '');
        error.status = response.status;
        throw error;
      }
      return body;
    } finally { if (timeout) clearTimeout(timeout); }
  }
  function post(url, body) {
    return api(url, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ ...body, guestId, source: 'web' }) });
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
    button.textContent = state?.pending ? 'Šaljem…' : sent ? 'Zatraženo ✓' : state?.uncertain ? 'Pokušaj opet' : 'Zatraži';
    button.setAttribute('aria-label', sent ? 'Pjesma ' + track.title + ' je zatražena' : 'Zatraži ' + track.title + ', ' + track.artist);
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
      toast('„' + track.title + '” je na popisu želja.');
      void pollState();
    } catch (error) {
      state.pending = false;
      state.uncertain = true;
      saveRequests();
      syncRequestButtons();
      toast(error.name === 'AbortError' ? 'Veza kasni. Pokušaj opet za provjeru istog zahtjeva.' : readableError(error), 'error');
    }
  }
  function renderResults(tracks) {
    requestButtons = new Map();
    const fragment = document.createDocumentFragment();
    for (const track of tracks) {
      const row = textNode('li', 'track-row', '');
      row.append(artNode(track.artUrl, 'track-art'));
      const info = textNode('div', 'track-info', '');
      info.append(textNode('span', 'track-title', track.title || 'Pjesma bez naslova'));
      info.append(textNode('div', 'track-artist', track.artist || 'Nepoznat izvođač'));
      const meta = textNode('div', 'track-meta', duration(track.durationMs));
      const url = spotifyUrl(track);
      if (url) {
        const link = textNode('a', '', 'Spotify ↗');
        link.href = url; link.target = '_blank'; link.rel = 'noopener noreferrer';
        link.setAttribute('aria-label', 'Otvori ' + (track.title || 'pjesmu') + ' u Spotifyju');
        meta.append(link);
      }
      info.append(meta);
      const button = textNode('button', 'request-button', 'Zatraži');
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
      elements['search-status'].textContent = 'Pretraži Spotify pa dodirni Zatraži.';
      elements['search-empty'].querySelector('p').textContent = 'Sljedeća dobra ideja može biti tvoja.';
      return;
    }
    const controller = new AbortController();
    searchController = controller;
    elements['search-spinner'].hidden = false;
    elements['search-status'].textContent = 'Tražim „' + query + '”…';
    const timeout = setTimeout(() => controller.abort(), 15000);
    try {
      const answer = await api('/api/search?q=' + encodeURIComponent(query), { signal: controller.signal });
      if (sequence !== searchSequence) return;
      if (answer.error) throw new Error(answer.error);
      const tracks = Array.isArray(answer.tracks) ? answer.tracks : [];
      renderResults(tracks);
      elements['search-status'].textContent = tracks.length ? 'Rezultati: ' + tracks.length + '. Dodirni Zatraži za slanje.' : 'Još nema rezultata za „' + query + '”.';
      elements['search-empty'].querySelector('p').textContent = tracks.length ? '' : 'Pokušaj s drugim naslovom ili izvođačem.';
    } catch (error) {
      if (sequence !== searchSequence) return;
      renderResults([]);
      elements['search-status'].textContent = error.name === 'AbortError' ? 'Pretraga kasni. Dodirni Traži za novi pokušaj.' : readableError(error);
      elements['search-empty'].querySelector('p').textContent = 'Sljedeća pjesma vrijedi još jednog pokušaja.';
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
      info.append(textNode('span', 'track-title', track.title || 'Tražimo tvoju pjesmu…'));
      const description = [track.artist, track.requestedBy ? 'za ' + track.requestedBy : ''].filter(Boolean).join(' / ');
      if (description) info.append(textNode('div', 'track-artist', description));
      const labels = { queued: 'Dodano u Spotify', playing: 'Sada svira', pending: 'Čeka DJ-a', accepted: 'Čeka DJ-a', sending: 'Dodajem u Spotify…', processing: 'Dodajem u Spotify…', uncertain: 'Provjeravam Spotify…', failed: 'Pjesmu nije bilo moguće dodati', rejected: 'Zahtjev je odbijen' };
      const label = textNode('div', 'queue-status', labels[track.status] || 'Čeka DJ-a');
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
    elements['now-title'].textContent = current.title || 'Glazba se zagrijava';
    elements['now-artist'].textContent = current.artist || (latestState?.spotify?.connected ? 'Odaberi sljedeću pjesmu s nama' : 'Čekamo Spotify');
    document.body.classList.toggle('playing', Boolean(current.playing));
    const signature = current.artUrl || '';
    if (signature !== artSignature) {
      artSignature = signature;
      const art = artNode(signature, 'now-art', current.album ? 'Omot albuma ' + current.album : 'Omot trenutačnog albuma');
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
    elements['track-progress'].setAttribute('aria-valuetext', duration(elapsed) + ' od ' + duration(total));
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
    elements['book-title'].textContent = moment.title || 'Trideset godina priča';
    elements['book-caption'].textContent = moment.caption || moment.text || '';
    elements['book-page'].textContent = moment.page ? 'Spomenar, str. ' + moment.page : '';
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
      elements['connection-text'].textContent = state.spotify?.connected ? 'Tu si s ekipom' : 'Veza s DJ pultom';
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
      elements['room-mood'].textContent = moodNames[state.mood?.label] || state.mood?.label || 'Tražimo ritam';
      renderBook(state.bookMoment);
    } catch {
      elements.connection.dataset.state = 'offline';
      elements['connection-text'].textContent = 'Ponovno spajanje';
      elements['room-notice'].hidden = false;
      elements['room-notice'].textContent = 'DJ pult ponovno se spaja. Tvoji zahtjevi ostaju ovdje.';
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
    elements['mood-feedback'].textContent = 'Javljam DJ-u…';
    try {
      await post('/api/mood', { guestId, mood });
      selectedMood = mood;
      stored.set('mood', mood);
      elements['mood-feedback'].textContent = mood === 'chaos' ? 'Promišljena želja za kaosom. Zabilježeno.' : moodNames[mood] + '. DJ je primio mig.';
      void pollState();
    } catch (error) {
      elements['mood-feedback'].textContent = error.name === 'AbortError' ? 'Odgovor kasni. Ponovno odaberi atmosferu.' : readableError(error);
    } finally { moodPending = false; paintMood(); }
  }));
  function chatTime(value) {
    let number = value;
    if (typeof value === 'number' && value < 1e12) number = value * 1000;
    const date = new Date(number);
    return Number.isFinite(date.getTime()) ? date.toLocaleTimeString('hr-HR', { hour: '2-digit', minute: '2-digit' }) : '';
  }
  function scrollChat() {
    elements['chat-log'].scrollTop = elements['chat-log'].scrollHeight;
    elements['chat-new'].hidden = true;
    elements['chat-unread'].hidden = true;
  }
  function paintDelivery(entry) {
    const work = entry.message.work;
    const labels = { active: 'DJ radi na ovome', waiting: 'Na čekanju', blocked: 'Rad je blokiran',
      completed: 'Dovršeno', cancelled: 'Otkazano', unconfirmed: 'Stanje rada treba potvrdu' };
    const parts = [];
    if (entry.status === 'read') parts.push('DJ je pročitao');
    if (work && labels[work.effectiveState || work.state]) parts.push(labels[work.effectiveState || work.state]);
    const node = entry.node.querySelector('.chat-delivery');
    node.textContent = parts.join(' · ');
    node.title = work ? [work.summary, work.outcome, ...(Array.isArray(work.evidence) ? work.evidence : [])].filter(Boolean).join('\n') : '';
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
        old.message = { ...old.message, ...message };
        old.status = old.message.status;
        paintDelivery(old);
        continue;
      }
      const assistant = message.role === 'assistant';
      const testing = message.isTest || message.role === 'test';
      const bubble = textNode('article', 'chat-bubble ' + (assistant ? 'chat-dj' : 'chat-guest'), '');
      const meta = textNode('div', 'chat-meta', '');
      meta.append(textNode('strong', '', (testing ? 'Proba · ' : '') + (assistant ? (message.name || 'Tvoj DJ') : (message.name || 'Gost'))));
      meta.append(textNode('time', '', chatTime(message.createdAt)));
      bubble.append(meta, textNode('p', 'chat-text', message.text || ''));
      bubble.append(textNode('span', 'chat-delivery', message.status === 'read' ? 'DJ je pročitao' : ''));
      let rawTime = message.createdAt;
      if (typeof rawTime === 'number' && rawTime < 1e12) rawTime *= 1000;
      const stamp = typeof rawTime === 'number' ? rawTime : Date.parse(rawTime || '');
      const order = /^\d+$/.test(key) ? Number(key) : 0;
      const next = [...chatMessages.values()]
        .filter((entry) => entry.stamp > stamp || (entry.stamp === stamp && entry.order > order))
        .sort((a, b) => a.stamp - b.stamp || a.order - b.order)[0];
      if (next) log.insertBefore(bubble, next.node);
      else log.append(bubble);
      const entry = { node: bubble, message, status: message.status, stamp, order };
      chatMessages.set(key, entry);
      paintDelivery(entry);
      added = true;
    }
    elements['chat-empty'].hidden = chatMessages.size > 0;
    if (added) {
      if (atBottom || own) scrollChat();
      else { elements['chat-new'].hidden = false; elements['chat-unread'].hidden = false; }
    }
  }
  function paintPresence(dj) {
    if (dj?.messageWork && typeof dj.messageWork === 'object') {
      for (const [id, work] of Object.entries(dj.messageWork)) {
        const entry = chatMessages.get(String(id));
        if (entry) { entry.message.work = work; paintDelivery(entry); }
      }
    }
    // Availability comes from an explicit agent lease. Watcher and work records
    // keep their own lifetimes and cannot renew it.
    presenceConnection = 'connected';
    presenceAvailable = false;
    const lease = dj?.llm;
    const wall = Date.now(), monotonic = performance.now();
    if (lease && typeof lease === 'object' && !Array.isArray(lease)
        && lease.status === 'online' && Number.isSafeInteger(lease.activeCount) && lease.activeCount > 0 && lease.activeCount <= 256
        && Number.isFinite(wall) && Number.isFinite(monotonic)
        && Number.isFinite(lease.lastSeen) && lease.lastSeen > 0 && lease.lastSeen <= wall
        && Number.isFinite(lease.expiresAt) && lease.expiresAt > wall
        && lease.expiresAt - lease.lastSeen >= 30000 && lease.expiresAt - lease.lastSeen <= 120000
        && Number.isFinite(lease.remainingMs) && lease.remainingMs > 0
        && lease.remainingMs <= lease.expiresAt - lease.lastSeen) {
      let deadline = monotonic + Math.min(lease.remainingMs, lease.expiresAt - wall);
      // Re-reading a lease with the same wall deadline must not extend its
      // monotonic deadline, including when another active session leaves.
      if (presenceLease?.expiresAt === lease.expiresAt) deadline = Math.min(deadline, presenceLease.deadline);
      presenceLease = { lastSeen: lease.lastSeen, expiresAt: lease.expiresAt, deadline };
      presenceAvailable = true;
    }
    renderPresence();
  }
  function renderPresence() {
    const wall = Date.now(), monotonic = performance.now();
    const active = presenceAvailable && presenceLease && Number.isFinite(wall) && Number.isFinite(monotonic)
      && wall >= presenceLease.lastSeen && wall < presenceLease.expiresAt && monotonic < presenceLease.deadline;
    elements['dj-presence'].dataset.state = active ? 'online' : 'away';
    elements['dj-presence-text'].textContent = active ? 'DJ je tu'
      : presenceConnection === 'reconnecting' ? 'Spajam razgovor' : 'Ostavi poruku DJ-u';
  }
  async function pollChat() {
    clearTimeout(chatTimer);
    if (chatPending) return;
    chatPending = true;
    try {
      const reply = await api('/api/chat' + (chatCursor ? '?after=' + encodeURIComponent(chatCursor) : ''));
      for (const id of Array.isArray(reply.withdrawnMessageIds) ? reply.withdrawnMessageIds : []) {
        const key = String(id), entry = chatMessages.get(key);
        if (entry) { entry.node.remove(); chatMessages.delete(key); }
      }
      const messages = Array.isArray(reply.messages) ? reply.messages : [];
      appendMessages(messages);
      const last = messages[messages.length - 1];
      if (last?.id !== undefined && last?.id !== null) chatCursor = String(last.id);
      paintPresence(reply.dj);
    } catch {
      presenceConnection = 'reconnecting';
      renderPresence();
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
    elements['message-status'].textContent = 'Šaljem…';
    try {
      const response = await post('/api/message', pendingMessage);
      if (response.message) appendMessages([response.message], true);
      if (elements.message.value.trim() === content) elements.message.value = '';
      pendingMessage = null;
      elements['message-status'].textContent = 'Dostavljeno. Odgovor stiže iznad.';
      void pollChat();
    } catch (error) {
      elements['message-status'].textContent = error.name === 'AbortError' ? 'Veza je spora. Tvoja je poruka još ovdje.' : readableError(error);
    } finally { messagePending = false; button.disabled = false; }
  });
  document.addEventListener('visibilitychange', () => {
    clearTimeout(stateTimer);
    clearTimeout(chatTimer);
    if (!document.hidden) { renderPresence(); void pollState(); void pollChat(); }
  });
  setInterval(() => { if (!document.hidden) { updateProgress(); renderPresence(); } }, 1000);
  void pollState();
  void pollChat();
})();
