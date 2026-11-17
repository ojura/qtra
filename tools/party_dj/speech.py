"""Prepared Croatian dedications mixed over the native Spotify stream."""
from __future__ import annotations

import collections
from dataclasses import dataclass
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time
import wave


class SpeechError(RuntimeError):
    pass


class SpeechCancelled(SpeechError):
    pass


def short_text(text, maximum=170):
    text = ' '.join(str(text).split())
    if len(text) > maximum:
        text = text[:maximum].rsplit(' ', 1)[0].rstrip(' ,;:') + '.'
    return text


@dataclass
class PreparedSpeech:
    path: Path
    seconds: float
    directory: tempfile.TemporaryDirectory

    def close(self):
        self.directory.cleanup()


class CroatianVoice:
    def __init__(self, directory):
        self.directory = Path(directory)
        local = Path(__file__).resolve().parents[2] / 'build/party-audio-deps/usr/bin/espeak-ng'
        self.command = shutil.which('espeak-ng') or (str(local) if local.is_file() else None)
        self.ffmpeg = shutil.which('ffmpeg')
        self.available = bool(self.command and self.ffmpeg)

    def prepare(self, text):
        if not self.available:
            raise SpeechError('Croatian speech needs espeak-ng and ffmpeg.')
        text = short_text(text)
        if not text:
            raise SpeechError('A dedication needs text.')
        self.directory.mkdir(parents=True, exist_ok=True)
        temporary = tempfile.TemporaryDirectory(prefix='voice-', dir=self.directory)
        raw, ready = Path(temporary.name) / 'raw.wav', Path(temporary.name) / 'voice.wav'
        try:
            # Measure the actual voice duration, then shorten whole words if needed.
            while True:
                subprocess.run([self.command, '-v', 'hr', '-s', '180', '-p', '43',
                                '-a', '110', '-w', str(raw), '--stdin'],
                               input=text, text=True, capture_output=True, timeout=15, check=True)
                with wave.open(str(raw)) as audio:
                    duration = audio.getnframes() / audio.getframerate()
                if duration <= 11.5:
                    break
                words = text.split()
                keep = min(len(words) - 1, max(1, int(len(words) * 10.8 / duration)))
                if keep < 1 or len(words) < 2:
                    raise SpeechError('The dedication cannot fit a short interlude.')
                text = ' '.join(words[:keep]).rstrip(' ,;:') + '.'
            filters = ('highpass=f=80,lowpass=f=8500,loudnorm=I=-19:TP=-3:LRA=6,'
                       f'afade=t=in:st=0:d=0.03,afade=t=out:st={max(0, duration-.08):.3f}:d=0.08')
            subprocess.run([self.ffmpeg, '-nostdin', '-loglevel', 'error', '-y', '-i', str(raw),
                            '-af', filters, '-ar', '48000', '-ac', '1', str(ready)],
                           capture_output=True, timeout=15, check=True)
            with wave.open(str(ready)) as audio:
                seconds = audio.getnframes() / audio.getframerate()
            if not 0 < seconds <= 12:
                raise SpeechError('The prepared dedication has an invalid duration.')
            return PreparedSpeech(ready, seconds, temporary)
        except Exception:
            temporary.cleanup()
            raise


class PulseAudio:
    available = bool(shutil.which('pactl') and shutil.which('paplay'))

    def _list(self, kind):
        result = subprocess.run(['pactl', '--format=json', 'list', kind],
                                capture_output=True, text=True, timeout=3, check=True)
        return json.loads(result.stdout)

    def find_spotify(self):
        streams = [s for s in self._list('sink-inputs')
                   if s.get('properties', {}).get('application.name') == 'Spotify'
                   and not s.get('corked') and not s.get('mute')]
        if len(streams) != 1:
            raise SpeechError('A single playing native Spotify stream is required.')
        return streams[0]

    def stream(self, index):
        return next((s for s in self._list('sink-inputs') if s['index'] == index), None)

    @staticmethod
    def volumes(stream):
        return tuple(v['value'] for v in stream['volume'].values())

    def set_volume(self, index, values):
        subprocess.run(['pactl', 'set-sink-input-volume', str(index), *map(str, values)],
                       capture_output=True, timeout=3, check=True)

    def sink_name(self, index):
        sink = next((s for s in self._list('sinks') if s['index'] == index), None)
        if not sink:
            raise SpeechError('Spotify output disappeared before the dedication.')
        return sink['name']

    def play(self, path, sink, volume):
        return subprocess.Popen(['paplay', '--client-name=LARICS-DJ', '--stream-name=Posveta',
                                 '--property=application.name=LARICS DJ',
                                 '--volume=' + str(volume), '--device=' + sink, str(path)],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


class SpotifyMixer:
    def __init__(self, pulse=None, sleep=time.sleep, clock=time.monotonic):
        self.pulse = pulse or PulseAudio()
        self.sleep, self.clock = sleep, clock
        self.available = self.pulse.available

    def play(self, prepared, valid, stop, on_started=None):
        pulse = self.pulse
        stream = pulse.find_spotify()
        index, sink = stream['index'], stream['sink']
        serial = stream.get('properties', {}).get('object.serial')
        if not serial:
            raise SpeechError('Spotify stream identity is unavailable.')
        original = pulse.volumes(stream)
        if not original or not max(original):
            raise SpeechCancelled('Spotify is silent.')
        output = pulse.sink_name(sink)
        voice_volume = min(57000, round(max(original) * 57000 / 65536))
        expected = original
        owned = True
        player = None

        def checked():
            nonlocal owned
            current = pulse.stream(index)
            if not current or current.get('properties', {}).get('object.serial') != serial:
                owned = False
                raise SpeechCancelled('Spotify stream changed.')
            values = pulse.volumes(current)
            if len(values) != len(expected) or any(abs(a-b) > 64 for a, b in zip(values, expected)):
                owned = False
                raise SpeechCancelled('Host changed Spotify volume.')
            return current

        def ramp(target, seconds, restoring=False):
            nonlocal expected
            start = expected
            started = self.clock()
            count = max(2, math.ceil(seconds / .04))
            for step in range(1, count + 1):
                if not restoring and (stop.is_set() or not valid()):
                    raise SpeechCancelled('Playback changed before the dedication.')
                current = checked()
                if not restoring and (current.get('corked') or current.get('mute') or current['sink'] != sink):
                    raise SpeechCancelled('Spotify output changed.')
                t = step / count
                smooth = t * t * (3 - 2 * t)
                values = tuple(round(a + (b-a) * smooth) for a, b in zip(start, target))
                pulse.set_volume(index, values)
                expected = values
                self.sleep(max(0, started + seconds * step / count - self.clock()))

        try:
            # Pulse's raw volume is cubic: a dB change uses 60*log10(raw ratio).
            duck_ratio = 10 ** (-10 / 60)
            ramp(tuple(round(v * duck_ratio) for v in original), .48)
            if stop.is_set() or not valid():
                raise SpeechCancelled('Playback changed before speech started.')
            player = pulse.play(prepared.path, output, voice_volume)
            if player.poll() not in (None, 0):
                raise SpeechError('The speech player could not start this dedication.')
            if stop.is_set() or not valid():
                raise SpeechCancelled('Playback changed before speech started.')
            if on_started is not None:
                on_started()
            deadline = self.clock() + prepared.seconds + 3
            while player.poll() is None:
                current = checked()
                if (stop.is_set() or not valid() or current.get('corked') or current.get('mute')
                        or current['sink'] != sink):
                    raise SpeechCancelled('Playback changed during the dedication.')
                if self.clock() > deadline:
                    raise SpeechError('The speech player did not finish in time.')
                self.sleep(.10)
            if player.returncode:
                raise SpeechError('The speech player could not play this dedication.')
            return {'kind': 'process_exit', 'returnCode': 0, 'audibleVerification': 'unperformed'}
        finally:
            if player is not None and player.poll() is None:
                player.terminate()
                try:
                    player.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    player.kill()
                    player.wait(timeout=1)
            if owned:
                try:
                    ramp(original, .88, restoring=True)
                except SpeechCancelled:
                    pass  # The host's new volume or stream takes precedence.
                except Exception:
                    # A failed ramp can still restore the last owned level directly.
                    checked()
                    pulse.set_volume(index, original)


class DedicationAudio:
    PREPARATION_LIMIT = 3
    OBSERVATION_TTL_MS = 10000

    def __init__(self, data, voice=None, mixer=None, *, clock=time.monotonic, wall_clock=time.time):
        self.data = Path(data)
        self.voice = voice or CroatianVoice(self.data / 'speech')
        self.mixer = mixer or SpotifyMixer()
        self.clock, self.wall_clock = clock, wall_clock
        self.available = bool(self.voice.available and self.mixer.available)
        self.unavailable_error = 'Speech preparation or local playback is unavailable.' if not self.available else None
        self.enabled = True
        self.lock = threading.RLock()
        self.changed = threading.Condition(self.lock)
        self.stop = threading.Event()
        self.thread = None
        self.current = {}
        self.current_device = None
        self.current_started_at = None
        self.initial_track = None
        self.current_seen = self.clock()
        self.generation = 0
        self.jobs = collections.deque()
        self.prepares = collections.OrderedDict()
        self.preparing = {}
        self.targets = collections.OrderedDict()
        self.next_preparation = 0
        self.ready = {}
        self.ready_at = {}
        self.prepare_errors = {}
        self.last = None
        self.history_file = self.data / 'speech-history.json'
        try:
            self.history = json.loads(self.history_file.read_text())
            if not isinstance(self.history, dict):
                raise ValueError('Speech history must be an object.')
        except FileNotFoundError:
            self.history = {}
        except (ValueError, OSError):
            self.history = {}
            self.available = False
            self.unavailable_error = 'Speech history cannot be read.'
            self.last = {'status': 'unavailable', 'error': self.unavailable_error}

    @staticmethod
    def track_key(track):
        return track.get('uid') or track.get('uri') or ''

    @staticmethod
    def occurrence_uid(track):
        uid = track.get('uid')
        return uid if isinstance(uid, str) and uid.strip() else None

    @staticmethod
    def playing(track):
        return (track.get('playing') is True and track.get('paused', False) is False
                and track.get('buffering', False) is False)

    @staticmethod
    def finite_number(value):
        if not isinstance(value, (int, float)) or isinstance(value, bool):
            return False
        try:
            return math.isfinite(value)
        except OverflowError:
            return False

    def _position(self, track):
        if 'clockValid' in track and track['clockValid'] is not True:
            return None
        progress, observed_at = track.get('progressMs'), track.get('observedAt')
        wall_now = self.wall_clock()
        if (not self.finite_number(progress) or progress < 0
                or not self.finite_number(observed_at) or observed_at < 0
                or not self.finite_number(wall_now)):
            return None
        age = wall_now * 1000 - observed_at
        if age < 0 or age >= self.OBSERVATION_TTL_MS:
            return None
        return progress + age

    @staticmethod
    def device_key(playback_state):
        device = (playback_state or {}).get('device') or {}
        return device.get('id') if isinstance(device, dict) else None

    def _record(self, key, text, status, error=None, evidence=None):
        item = {'id': key, 'text': text, 'status': status, 'updatedAt': int(self.wall_clock()*1000)}
        if error:
            item['error'] = str(error)[:250]
        if evidence is not None:
            item['evidence'] = dict(evidence)
        updated = {**self.history, key: item}
        temporary = self.history_file.with_suffix('.tmp')
        temporary.write_text(json.dumps(updated, ensure_ascii=False))
        os.replace(temporary, self.history_file)
        self.history = updated
        self.last = item
        return item

    def _finish(self, job, status, error=None, evidence=None):
        try:
            return self._record(job['key'], job['text'], status, error, evidence)
        except OSError as failure:
            self.last = {'id': job['key'], 'status': 'failed', 'error': str(failure)[:250]}

    def _cancel_jobs(self, reason):
        self.generation += 1
        while self.jobs:
            job = self.jobs.popleft()
            if job.get('prepared') is not None:
                job['prepared'].close()
            self._finish(job, 'cancelled', reason)

    def _discard_preparation(self, rid):
        self.targets.pop(rid, None)
        self.prepares.pop(rid, None)
        self.prepare_errors.pop(rid, None)
        self.ready_at.pop(rid, None)
        prepared = self.ready.pop(rid, None)
        if prepared is not None:
            prepared.close()

    def snapshot(self):
        with self.lock:
            preparations, attention = [], []
            for rid, target in self.targets.items():
                error = self.prepare_errors.get(rid)
                if error:
                    status = 'failed'
                    attention.append({'requestId': rid, 'kind': 'preparation_failed', 'error': error})
                elif rid in self.ready:
                    status = 'ready'
                elif target.get('terminal'):
                    status = target['terminal']
                elif self.preparing.get(rid) == target['token']:
                    status = 'preparing'
                else:
                    status = 'queued'
                item = {'requestId': rid, 'status': status, 'ready': status == 'ready',
                        'occurrenceUid': target.get('uid'), 'text': target['text']}
                if rid in self.ready:
                    item['seconds'] = self.ready[rid].seconds
                if error:
                    item['error'] = error
                preparations.append(item)
            if self.unavailable_error:
                attention.append({'requestId': None, 'kind': 'unavailable', 'error': self.unavailable_error})
            return {'available': self.available, 'enabled': self.available and self.enabled, 'voice': 'hr',
                    'speaking': bool(self.last and self.last['status'] == 'speaking'),
                    'pending': len(self.jobs), 'last': dict(self.last) if self.last else None,
                    'preparations': preparations[:self.PREPARATION_LIMIT + 1],
                    'attention': attention[:self.PREPARATION_LIMIT + 1]}

    def start(self):
        if self.available and self.thread is None:
            self.thread = threading.Thread(target=self._run, name='dedication-audio', daemon=True)
            self.thread.start()

    def set_enabled(self, enabled):
        with self.changed:
            self.enabled = bool(enabled)
            if not self.enabled:
                self._cancel_jobs('Speech was disabled.')
                for rid in list(self.targets):
                    self._discard_preparation(rid)
            self.changed.notify_all()

    def interrupt(self):
        with self.changed:
            self._cancel_jobs('Playback was interrupted.')
            self.current['playing'] = False
            self.changed.notify_all()

    def _want_preparation(self, request):
        rid, text = request['id'], short_text(request['dedication'])
        uid = request.get('uid')
        target = self.targets.get(rid)
        if target and (target['text'] != text or (target.get('uid') and uid and target['uid'] != uid)):
            self._discard_preparation(rid)
            target = None
        if target is None:
            self.next_preparation += 1
            target = {'text': text, 'uid': uid, 'token': self.next_preparation}
            self.targets[rid] = target
        elif uid:
            target['uid'] = uid
        if (rid not in self.ready and rid not in self.prepare_errors and not target.get('terminal')
                and self.preparing.get(rid) != target['token']):
            self.prepares[rid] = (text, target['token'])

    def observe(self, current, requests, *, playback_state=None):
        with self.changed:
            previous_key = self.track_key(self.current)
            previous_playing = self.playing(self.current)
            device = self.current_device if playback_state is None else self.device_key(playback_state)
            incoming = dict(current or {})
            if playback_state is not None:
                for flag in ('paused', 'buffering'):
                    if flag in playback_state and playback_state[flag] is not False:
                        incoming[flag] = playback_state[flag]
                if 'clockValid' in playback_state and playback_state['clockValid'] is not True:
                    incoming['clockValid'] = playback_state['clockValid']
            key = self.track_key(incoming)
            progress = self._position(incoming)
            if key != previous_key or device != self.current_device:
                self._cancel_jobs('The requested occurrence or output device changed.')
            elif key and (progress is None or (previous_playing and not self.playing(incoming))):
                self._cancel_jobs('Playback paused, buffered or lost a fresh observation.')
            if key != previous_key:
                self.current_started_at = None
            self.current, self.current_device = incoming, device
            self.current_seen = self.clock()
            if progress is not None and self.playing(self.current) and self.current_started_at is None:
                self.current_started_at = self.current_seen - progress / 1000
            if not self.available:
                return
            if key and self.initial_track is None:
                self.initial_track = key
            elif key and self.initial_track and key != self.initial_track:
                self.initial_track = ''
            if not self.enabled:
                return
            future = [r for r in requests if r.get('dedication') and r.get('status') != 'playing'][:self.PREPARATION_LIMIT]
            rid, text = self.current.get('requestId'), self.current.get('dedication')
            retained = {r['id'] for r in future} | {rid}
            for old in list(self.targets):
                if old not in retained:
                    self._discard_preparation(old)
            for request in future:
                self._want_preparation(request)
            if (rid and text and key != self.initial_track and self.playing(self.current)
                    and progress is not None):
                job_key = f'dedication:{rid}:{key}'
                if job_key not in self.history:
                    target = self.targets.get(rid)
                    ready = self.ready.get(rid)
                    reason = None
                    if self.occurrence_uid(self.current) is None:
                        reason = 'The requested occurrence has no verified UID.'
                    elif progress > 12000:
                        reason = 'The opening of the requested song has passed.'
                    elif ready is None or target is None or target['text'] != short_text(text):
                        reason = self.prepare_errors.get(rid) or 'The dedication was not ready before its occurrence.'
                    elif target.get('uid') and target['uid'] != self.current['uid']:
                        reason = 'The prepared dedication belongs to another occurrence.'
                    elif self.ready_at[rid] > self.current_started_at:
                        reason = 'The dedication became ready after its occurrence started.'
                    try:
                        if reason:
                            self._record(job_key, short_text(text), 'missed', reason)
                            self.prepares.pop(rid, None)
                            if target is not None:
                                self.next_preparation += 1
                                target.update(token=self.next_preparation, terminal='missed')
                            stale = self.ready.pop(rid, None)
                            self.ready_at.pop(rid, None)
                            if stale is not None:
                                stale.close()
                        else:
                            self._enqueue(job_key, short_text(text), key, rid, automatic=True, prepared=ready)
                            self.ready.pop(rid)
                            self.ready_at.pop(rid, None)
                            target['terminal'] = 'consumed'
                    except OSError as error:
                        self.last = {'id': job_key, 'status': 'failed', 'error': str(error)[:250]}
            self.changed.notify_all()

    def _enqueue(self, key, text, expected, prepared_id, automatic=False, prepared=None):
        existing = self.history.get(key)
        if existing:
            if existing['text'] != text:
                raise ValueError('This speech request ID already has different text.')
            return {'queued': existing['status'] == 'queued', **existing}
        self._record(key, text, 'queued')
        self.jobs.append({'key': key, 'text': text, 'track': expected, 'device': self.current_device,
                          'generation': self.generation, 'preparedId': prepared_id,
                          'automatic': automatic, 'prepared': prepared})
        self.changed.notify_all()
        return {'queued': True, 'id': key, 'status': 'queued'}

    def submit(self, request_id, text):
        text = short_text(text)
        if len(request_id) < 8 or not text:
            raise ValueError('Speech needs a request ID and text.')
        with self.changed:
            if not self.available or not self.enabled:
                return {'queued': False, 'error': 'Croatian speech is unavailable; the text stays on screen.'}
            uid = self.occurrence_uid(self.current)
            if uid is None or not self.playing(self.current) or self._position(self.current) is None:
                return {'queued': False, 'error': 'A fresh playing Spotify occurrence with a verified UID is required for a dedication.'}
            return self._enqueue('manual:' + request_id, text, uid, 'manual:' + request_id)

    def _valid(self, job):
        with self.lock:
            return (self.enabled and not self.stop.is_set() and self.playing(self.current)
                    and self._position(self.current) is not None
                    and self.track_key(self.current) == job['track']
                    and self.current_device == job['device'] and self.generation == job['generation'])

    def _run(self):
        while not self.stop.is_set():
            with self.changed:
                if not self.jobs and not self.prepares:
                    self.changed.wait(1)
                    continue
                job = self.jobs.popleft() if self.jobs else None
                preparation = self.prepares.popitem(last=False) if not job and self.prepares else None
                if preparation:
                    self.preparing[preparation[0]] = preparation[1][1]
            if preparation:
                rid, (text, token) = preparation
                ready = None
                try:
                    ready = self.voice.prepare(text)
                    with self.changed:
                        target = self.targets.get(rid)
                        if (not self.stop.is_set() and self.enabled and target is not None
                                and target['token'] == token and not target.get('terminal')):
                            self.ready[rid] = ready
                            self.ready_at[rid] = self.clock()
                            ready = None
                        self.changed.notify_all()
                except Exception as error:
                    with self.changed:
                        target = self.targets.get(rid)
                        if target is not None and target['token'] == token and self.enabled:
                            self.prepare_errors[rid] = str(error)[:250]
                        self.changed.notify_all()
                finally:
                    if ready is not None:
                        ready.close()
                    with self.changed:
                        if self.preparing.get(rid) == token:
                            self.preparing.pop(rid, None)
                        self.changed.notify_all()
                continue
            ready = job.get('prepared')
            evidence = None
            try:
                if not self._valid(job):
                    raise SpeechCancelled('The bound playback was cancelled.')
                if ready is None:
                    if job['automatic']:
                        raise SpeechCancelled('The dedication was not prepared before its occurrence.')
                    ready = self.voice.prepare(job['text'])
                with self.lock:
                    if not self._valid(job):
                        raise SpeechCancelled('Playback changed while speech was prepared.')
                    progress = self._position(self.current)
                    if progress is None:
                        raise SpeechCancelled('The playback observation expired before speech started.')
                    if job['automatic'] and progress > 15000:
                        raise SpeechCancelled('The opening of the requested song has passed.')
                    self._record(job['key'], job['text'], 'starting')

                def on_started():
                    with self.lock:
                        if not self._valid(job):
                            raise SpeechCancelled('Playback changed before speech started.')
                        self._record(job['key'], job['text'], 'speaking')

                evidence = self.mixer.play(ready, lambda: self._valid(job), self.stop, on_started=on_started)
                if (not isinstance(evidence, dict) or evidence.get('kind') != 'process_exit'
                        or type(evidence.get('returnCode')) is not int or evidence['returnCode'] != 0):
                    raise SpeechError('The speech mixer returned no successful process completion evidence.')
                evidence = {**evidence, 'audibleVerification': 'unperformed'}
                status, error = 'completed', None
            except SpeechCancelled as failure:
                status, error = 'cancelled', str(failure)
            except Exception as failure:
                status, error = 'failed', str(failure)
            finally:
                if ready is not None:
                    ready.close()
            with self.changed:
                self._finish(job, status, error, evidence if status == 'completed' else None)
                self.changed.notify_all()

    def close(self):
        self.stop.set()
        with self.changed:
            self._cancel_jobs('Speech was stopped.')
            for rid in list(self.targets):
                self._discard_preparation(rid)
            self.changed.notify_all()
        if self.thread:
            self.thread.join()
