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

    def play(self, prepared, valid, stop):
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
    def __init__(self, data, voice=None, mixer=None):
        self.data = Path(data)
        self.voice = voice or CroatianVoice(self.data / 'speech')
        self.mixer = mixer or SpotifyMixer()
        self.available = bool(self.voice.available and self.mixer.available)
        self.enabled = True
        self.lock = threading.RLock()
        self.changed = threading.Condition(self.lock)
        self.stop = threading.Event()
        self.thread = None
        self.current = {}
        self.initial_track = None
        self.current_seen = time.monotonic()
        self.jobs = collections.deque()
        self.prepares = collections.OrderedDict()
        self.preparing = set()
        self.ready = {}
        self.prepare_errors = {}
        self.last = None
        self.history_file = self.data / 'speech-history.json'
        try:
            self.history = json.loads(self.history_file.read_text())
        except FileNotFoundError:
            self.history = {}
        except (ValueError, OSError):
            # Without the history, automatic repeats cannot be reconciled.
            self.history = {}
            self.available = False
            self.last = {'status': 'unavailable', 'error': 'Speech history cannot be read.'}

    @staticmethod
    def track_key(track):
        return track.get('uid') or track.get('uri') or ''

    def _record(self, key, text, status, error=None):
        item = {'id': key, 'text': text, 'status': status, 'updatedAt': int(time.time()*1000)}
        if error:
            item['error'] = str(error)[:250]
        self.history[key] = item
        temporary = self.history_file.with_suffix('.tmp')
        temporary.write_text(json.dumps(self.history, ensure_ascii=False))
        os.replace(temporary, self.history_file)
        self.last = item
        return item

    def snapshot(self):
        with self.lock:
            return {'available': self.available, 'enabled': self.available and self.enabled, 'voice': 'hr',
                    'speaking': bool(self.last and self.last['status'] == 'speaking'),
                    'pending': len(self.jobs), 'last': self.last}

    def start(self):
        if self.available and self.thread is None:
            self.thread = threading.Thread(target=self._run, name='dedication-audio', daemon=True)
            self.thread.start()

    def set_enabled(self, enabled):
        with self.changed:
            self.enabled = bool(enabled)
            if not self.enabled:
                self.prepares.clear()
            self.changed.notify_all()

    def interrupt(self):
        with self.lock:
            self.current['playing'] = False

    def observe(self, current, requests):
        with self.changed:
            self.current = dict(current or {})
            self.current_seen = time.monotonic()
            if not self.available:
                return
            key = self.track_key(self.current)
            if key and self.initial_track is None:
                self.initial_track = key
            elif key and self.initial_track and key != self.initial_track:
                self.initial_track = ''
            if not self.enabled:
                return
            future = [r for r in requests if r.get('dedication') and r.get('status') != 'playing'][:3]
            wanted = {r['id'] for r in future}
            retained = wanted | {self.current.get('requestId')} | {j['preparedId'] for j in self.jobs}
            for rid in list(self.ready):
                if rid not in retained:
                    self.ready.pop(rid).close()
            for rid in list(self.prepare_errors):
                if rid not in retained:
                    self.prepare_errors.pop(rid, None)
            for rid in list(self.prepares):
                if rid not in wanted:
                    self.prepares.pop(rid, None)
            for r in future:
                text = short_text(r['dedication'])
                if (r['id'] not in self.ready and r['id'] not in self.prepare_errors
                        and r['id'] not in self.preparing):
                    self.prepares[r['id']] = text
            rid, text = self.current.get('requestId'), self.current.get('dedication')
            if (rid and text and key != self.initial_track and self.current.get('playing')
                    and float(self.current.get('progressMs', 0)) <= 12000):
                job_key = f'dedication:{rid}:{key}'
                if job_key not in self.history:
                    try:
                        self._enqueue(job_key, short_text(text), key, rid, automatic=True)
                    except OSError as error:
                        self.last = {'status': 'failed', 'error': str(error)[:250]}
            self.changed.notify_all()

    def _enqueue(self, key, text, expected, prepared_id, automatic=False):
        existing = self.history.get(key)
        if existing:
            if existing['text'] != text:
                raise ValueError('This speech request ID already has different text.')
            return {'queued': existing['status'] == 'queued', **existing}
        self._record(key, text, 'queued')
        self.jobs.append({'key': key, 'text': text, 'track': expected,
                          'preparedId': prepared_id, 'automatic': automatic})
        self.changed.notify_all()
        return {'queued': True, 'id': key, 'status': 'queued'}

    def submit(self, request_id, text):
        text = short_text(text)
        if len(request_id) < 8 or not text:
            raise ValueError('Speech needs a request ID and text.')
        with self.changed:
            if not self.available or not self.enabled:
                return {'queued': False, 'error': 'Croatian speech is unavailable; the text stays on screen.'}
            key = self.track_key(self.current)
            if not key or not self.current.get('playing'):
                return {'queued': False, 'error': 'Start Spotify before speaking a dedication.'}
            return self._enqueue('manual:' + request_id, text, key, 'manual:' + request_id)

    def _valid(self, job):
        with self.lock:
            return (self.enabled and not self.stop.is_set() and bool(self.current.get('playing'))
                    and self.track_key(self.current) == job['track'])

    def _run(self):
        while not self.stop.is_set():
            with self.changed:
                if not self.jobs and not self.prepares:
                    self.changed.wait(1)
                    continue
                job = self.jobs.popleft() if self.jobs else None
                preparation = self.prepares.popitem(last=False) if not job and self.prepares else None
                if preparation:
                    self.preparing.add(preparation[0])
            if preparation:
                rid, text = preparation
                try:
                    ready = self.voice.prepare(text)
                    with self.lock:
                        self.ready[rid] = ready
                except Exception as error:
                    with self.lock:
                        self.prepare_errors[rid] = str(error)[:250]
                finally:
                    with self.lock:
                        self.preparing.discard(rid)
                continue
            ready = None
            try:
                with self.lock:
                    ready = self.ready.pop(job['preparedId'], None)
                    previous_error = self.prepare_errors.pop(job['preparedId'], None)
                if previous_error:
                    raise SpeechError(previous_error)
                if not self._valid(job):
                    raise SpeechCancelled('The requested song has already changed.')
                if ready is None:
                    ready = self.voice.prepare(job['text'])
                with self.lock:
                    progress = float(self.current.get('progressMs', 0)) + (time.monotonic()-self.current_seen)*1000
                    if job['automatic'] and progress > 15000:
                        raise SpeechCancelled('The opening of the requested song has passed.')
                    self._record(job['key'], job['text'], 'speaking')
                self.mixer.play(ready, lambda: self._valid(job), self.stop)
                status, error = 'played', None
            except SpeechCancelled as failure:
                status, error = 'cancelled', str(failure)
            except Exception as failure:
                status, error = 'failed', str(failure)
            finally:
                if ready is not None:
                    ready.close()
            with self.lock:
                try:
                    self._record(job['key'], job['text'], status, error)
                except OSError as failure:
                    self.last = {'status': 'failed', 'error': str(failure)[:250]}

    def close(self):
        self.stop.set()
        with self.changed:
            self.changed.notify_all()
        if self.thread:
            self.thread.join()
        for ready in self.ready.values():
            ready.close()
        self.ready.clear()
