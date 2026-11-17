"""File-only Croatian voice rehearsal; the CLI supplies the outer process/I/O guard."""
from __future__ import annotations

from array import array
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import threading
import wave

from speech import CroatianVoice, short_text


DEFAULT_TEXT = 'Antonio, ovaj refren je za tebe. Pozdrav cijeloj ekipi.'
SAMPLE_RATE = 48000
DUCK_SECONDS = .48
RESTORE_SECONDS = .88
DUCK_DB = -10.0
DUCK_GAIN = 10 ** (DUCK_DB / 20)


class RenderError(RuntimeError):
    def __init__(self, message, *, stage='render', return_code=None, stderr=None):
        super().__init__(message)
        self.stage = stage
        self.return_code = return_code
        if isinstance(stderr, bytes):
            stderr = stderr.decode('utf-8', errors='replace')
        self.stderr = str(stderr).strip()[:1000] if stderr is not None else None

    def as_dict(self):
        result = {'stage': self.stage, 'error': str(self)}
        if self.return_code is not None:
            result['returnCode'] = self.return_code
        if self.stderr:
            result['stderr'] = self.stderr
        return result


def analyze_pcm(samples, rate=SAMPLE_RATE):
    """Measure signed PCM16; full-scale samples are reported as clipping indicators."""
    if not samples or type(rate) is not int or rate <= 0:
        raise RenderError('PCM needs samples and a positive integer sample rate.', stage='analysis')
    if any(type(value) is not int or not -32768 <= value <= 32767 for value in samples):
        raise RenderError('Expected signed 16-bit PCM samples.', stage='analysis')
    count = len(samples)
    power = sum(value * value for value in samples) / count
    rms = math.sqrt(power) / 32768
    peak = max(abs(value) for value in samples) / 32768
    clipped = sum(value in (-32768, 32767) for value in samples)
    nonzero = sum(value != 0 for value in samples)
    return {'sampleRate': rate, 'channels': 1, 'sampleWidthBits': 16, 'samples': count,
            'durationSeconds': count / rate, 'rmsDbfs': 20 * math.log10(rms) if rms else None,
            'peakDbfs': 20 * math.log10(peak) if peak else None, 'peakLinear': peak,
            'clippedSamples': clipped, 'clippedFraction': clipped / count,
            'clippingDefinition': 'Samples at the PCM16 endpoints -32768 or 32767.',
            'nonzeroSamples': nonzero, 'nonzeroFraction': nonzero / count,
            'nonSilent': bool(nonzero)}


def read_voice_pcm(path):
    try:
        with wave.open(str(path), 'rb') as source:
            if (source.getnchannels() != 1 or source.getsampwidth() != 2
                    or source.getframerate() != SAMPLE_RATE or source.getcomptype() != 'NONE'):
                raise RenderError('Prepared voice must be 48 kHz mono PCM16 WAV.', stage='analysis')
            count = source.getnframes()
            if not 0 < count <= 12 * SAMPLE_RATE:
                raise RenderError('Prepared voice duration must be above zero and at most 12 seconds.', stage='analysis')
            data = source.readframes(count)
            if len(data) != count * 2:
                raise RenderError('Prepared voice WAV has truncated PCM data.', stage='analysis')
    except (OSError, EOFError, wave.Error) as error:
        raise RenderError(f'Cannot read the prepared voice WAV: {error}', stage='analysis') from error
    samples = array('h')
    samples.frombytes(data)
    if sys.byteorder != 'little':
        samples.byteswap()
    return samples


def write_pcm(path, samples):
    data = array('h', samples)
    if sys.byteorder != 'little':
        data.byteswap()
    with wave.open(str(path), 'wb') as target:
        target.setnchannels(1)
        target.setsampwidth(2)
        target.setframerate(SAMPLE_RATE)
        target.writeframes(data.tobytes())


def _power_ratio_db(numerator, denominator):
    return 10 * math.log10(numerator / denominator) if numerator > 0 and denominator > 0 else None


def mix_with_bed(voice):
    """Mix a newly synthesized three-tone bed and cosine fades entirely in memory."""
    analyze_pcm(voice)
    pre = round(.75 * SAMPLE_RATE)
    duck_frames = round(DUCK_SECONDS * SAMPLE_RATE)
    restore_frames = round(RESTORE_SECONDS * SAMPLE_RATE)
    speech_start = pre + duck_frames
    restore_start = speech_start + len(voice)
    post_start = restore_start + restore_frames
    total = post_start + pre
    mixed = array('h')
    powers = {name: [0, 0] for name in ('pre', 'hold', 'post')}
    limited = 0
    last_gain = 1.0
    maximum_step = 0.0
    minimum_gain = 1.0
    for index in range(total):
        if index < pre:
            gain = 1.0
        elif index < speech_start:
            phase = (index - pre) / duck_frames
            gain = 1 + (DUCK_GAIN - 1) * (.5 - .5 * math.cos(math.pi * phase))
        elif index < restore_start:
            gain = DUCK_GAIN
        elif index < post_start:
            phase = (index - restore_start) / restore_frames
            gain = DUCK_GAIN + (1 - DUCK_GAIN) * (.5 - .5 * math.cos(math.pi * phase))
        else:
            gain = 1.0
        maximum_step = max(maximum_step, abs(gain - last_gain))
        minimum_gain = min(minimum_gain, gain)
        last_gain = gain
        seconds = index / SAMPLE_RATE
        bed = round(32768 * (.09 * math.sin(2 * math.pi * 220 * seconds)
                            + .045 * math.sin(2 * math.pi * 330 * seconds)
                            + .0225 * math.sin(2 * math.pi * 440 * seconds)))
        ducked = round(bed * gain)
        region = ('pre' if index < pre else 'hold' if speech_start <= index < restore_start
                  else 'post' if index >= post_start else None)
        if region:
            powers[region][0] += ducked * ducked
            powers[region][1] += bed * bed
        sample = ducked + (voice[index - speech_start] if speech_start <= index < restore_start else 0)
        if sample < -32768 or sample > 32767:
            limited += 1
        mixed.append(max(-32768, min(32767, sample)))
    voice_power = sum(value * value for value in voice)
    envelope = {'kind': 'raised-cosine acoustic gain', 'targetGainDb': DUCK_DB,
                'duckSeconds': DUCK_SECONDS, 'restoreSeconds': RESTORE_SECONDS,
                'duckStartSeconds': pre / SAMPLE_RATE, 'speechStartSeconds': speech_start / SAMPLE_RATE,
                'speechEndSeconds': restore_start / SAMPLE_RATE, 'restoreEndSeconds': post_start / SAMPLE_RATE,
                'minimumGain': minimum_gain, 'finalGain': last_gain, 'maximumStep': maximum_step,
                'measuredPreGainDb': _power_ratio_db(*powers['pre']),
                'measuredHoldGainDb': _power_ratio_db(*powers['hold']),
                'measuredPostGainDb': _power_ratio_db(*powers['post']),
                'voiceToDuckedBedRmsDb': _power_ratio_db(voice_power, powers['hold'][0]),
                'limitedSamples': limited,
                'bed': {'source': 'original synthetic tones', 'frequenciesHz': [220, 330, 440]}}
    return mixed, envelope


class VoiceRenderer:
    """Validate the existing voice adapter before permitting two file-only programs.

    The caller installs an audit hook before importing this module and calls
    allow_spawn for each subprocess.Popen event. Fake executors do the same in
    pure tests. A pending permission belongs to one launching thread and one
    spawn only. The caller owns private_dir cleanup and explicit exports.
    """
    def __init__(self, private_dir, *, run=None, voice_factory=None):
        requested = Path(private_dir)
        if not requested.is_absolute() or requested.is_symlink() or not requested.is_dir():
            raise RenderError('Use an existing private absolute directory.', stage='setup')
        self.directory = requested.resolve(strict=True)
        self._executor = subprocess.run if run is None else run
        self._state_lock = threading.Lock()
        self._pending = None
        self._rendered = False
        self.approved_spawns = 0
        self.spoken_text = None
        factory = CroatianVoice if voice_factory is None else voice_factory
        self.voice = factory(self.directory, run=self._run)
        if not self.voice.available:
            raise RenderError('Croatian file rendering needs espeak-ng and ffmpeg.', stage='setup')
        self._executables = {'espeak': self._executable(self.voice.command),
                             'ffmpeg': self._executable(self.voice.ffmpeg)}
        self.voice.command = self._executables['espeak']
        self.voice.ffmpeg = self._executables['ffmpeg']
        self._environment = {'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8', 'TMPDIR': str(self.directory)}

    @property
    def executables(self):
        return dict(self._executables)

    @staticmethod
    def _executable(value):
        if not isinstance(value, str) or not Path(value).is_absolute():
            raise RenderError('The voice executable needs an absolute path.', stage='setup')
        try:
            resolved = Path(value).resolve(strict=True)
        except OSError as error:
            raise RenderError(f'Voice executable is unavailable: {error}', stage='setup') from error
        if not resolved.is_file() or not os.access(resolved, os.X_OK):
            raise RenderError(f'Voice executable is not an executable file: {resolved}', stage='setup')
        return str(resolved)

    def _private_wave(self, value, name):
        if not isinstance(value, str):
            raise RenderError('WAV paths must be absolute strings.', stage='command')
        path = Path(value)
        if (not path.is_absolute() or path.name != name or path.parent.parent != self.directory
                or not path.parent.name.startswith('voice-') or not path.parent.is_dir()
                or path.is_symlink() or path.parent.is_symlink() or path.resolve() != path):
            raise RenderError('The command must use its owned voice-* WAV files.', stage='command')
        return path

    @staticmethod
    def _raw_duration(path):
        try:
            with wave.open(str(path), 'rb') as raw:
                if (raw.getnchannels() != 1 or raw.getsampwidth() != 2 or raw.getcomptype() != 'NONE'
                        or not 8000 <= raw.getframerate() <= 192000):
                    raise RenderError('Raw synthesis must be mono PCM16 WAV.', stage='command')
                count, rate = raw.getnframes(), raw.getframerate()
                if not 0 < count / rate <= 11.5:
                    raise RenderError('Raw synthesis has an invalid conversion duration.', stage='command')
                if len(raw.readframes(count)) != count * 2:
                    raise RenderError('Raw synthesis WAV is truncated.', stage='command')
                return count / rate
        except (OSError, EOFError, wave.Error) as error:
            raise RenderError(f'Cannot validate raw synthesis: {error}', stage='command') from error

    def _command(self, argv, options):
        if not isinstance(argv, (list, tuple)) or not argv or any(not isinstance(part, str) for part in argv):
            raise RenderError('The renderer requires an explicit argument vector.', stage='command')
        args = list(argv)
        common = {'capture_output', 'timeout', 'check'}
        if (options.get('capture_output') is not True or options.get('check') is not True
                or type(options.get('timeout')) is not int or options['timeout'] != 15):
            raise RenderError('The command needs the fixed capture, check and timeout options.', stage='command')
        if args[0] == self._executables['espeak']:
            if (len(args) != 12 or args[:10] != [args[0], '-v', 'hr', '-s', '180', '-p', '43', '-a', '110', '-w']
                    or args[-1] != '--stdin' or set(options) != common | {'input', 'text'}
                    or options.get('text') is not True):
                raise RenderError('Only the reviewed espeak WAV/stdin command is allowed.', stage='command')
            self._private_wave(args[10], 'raw.wav')
            text = options['input']
            if not isinstance(text, str) or not text or '\x00' in text or text != short_text(text):
                raise RenderError('espeak input must be the normalized dedication text.', stage='command')
            return args, 'synthesis', text
        if args[0] == self._executables['ffmpeg']:
            if len(args) != 14 or set(options) != common:
                raise RenderError('Only the reviewed ffmpeg file conversion is allowed.', stage='command')
            raw = self._private_wave(args[6], 'raw.wav')
            ready = self._private_wave(args[13], 'voice.wav')
            if raw.parent != ready.parent:
                raise RenderError('The conversion input and output must be siblings.', stage='command')
            duration = self._raw_duration(raw)
            filters = ('highpass=f=80,lowpass=f=8500,loudnorm=I=-19:TP=-3:LRA=6,'
                       f'afade=t=in:st=0:d=0.03,afade=t=out:st={max(0, duration-.08):.3f}:d=0.08')
            expected = [args[0], '-nostdin', '-loglevel', 'error', '-y', '-i', str(raw),
                        '-af', filters, '-ar', '48000', '-ac', '1', str(ready)]
            if args != expected:
                raise RenderError('The ffmpeg flags and duration-derived filters must match exactly.', stage='command')
            safe = [args[0], '-nostdin', '-loglevel', 'error', '-y', '-protocol_whitelist', 'file',
                    '-i', str(raw), '-af', filters, '-ar', '48000', '-ac', '1',
                    '-c:a', 'pcm_s16le', '-f', 'wav', str(ready)]
            return safe, 'conversion', None
        raise RenderError('This executable is outside the renderer allowlist.', stage='command')

    def active_launch_on_thread(self):
        with self._state_lock:
            return self._pending is not None and self._pending['thread'] == threading.get_ident()

    def allow_spawn(self, executable, argv, cwd, env):
        with self._state_lock:
            pending = self._pending
            if (pending is None or pending['thread'] != threading.get_ident() or pending['consumed']
                    or not isinstance(argv, (list, tuple)) or not isinstance(env, dict)):
                return False
            if (executable != pending['argv'][0] or tuple(argv) != pending['argv']
                    or cwd != pending['cwd'] or env != pending['env']):
                return False
            pending['consumed'] = True
            self.approved_spawns += 1
            return True

    def _run(self, argv, **options):
        safe, stage, spoken = self._command(argv, options)
        actual = {**options, 'cwd': str(self.directory), 'env': dict(self._environment),
                  'close_fds': True, 'shell': False}
        if stage == 'conversion':
            actual['input'] = b''
        with self._state_lock:
            if self._pending is not None:
                raise RenderError('A renderer command is already active.', stage='command')
            pending = {'thread': threading.get_ident(), 'argv': tuple(safe), 'cwd': actual['cwd'],
                       'env': dict(actual['env']), 'consumed': False}
            self._pending = pending
        try:
            result = self._executor(safe, **actual)
            if not pending['consumed']:
                raise RenderError('The executor did not pass the process audit gate.', stage=stage)
            if type(getattr(result, 'returncode', None)) is not int or result.returncode != 0:
                raise RenderError('The rendering process did not report a successful exit.', stage=stage,
                                  return_code=getattr(result, 'returncode', None),
                                  stderr=getattr(result, 'stderr', None))
            if spoken is not None:
                self.spoken_text = spoken
            return result
        except subprocess.CalledProcessError as error:
            raise RenderError(f'{stage} process exited with status {error.returncode}.', stage=stage,
                              return_code=error.returncode, stderr=error.stderr) from error
        except subprocess.TimeoutExpired as error:
            raise RenderError(f'{stage} process exceeded its 15-second limit.', stage=stage,
                              stderr=error.stderr) from error
        except OSError as error:
            raise RenderError(f'{stage} process could not start: {error}', stage=stage) from error
        finally:
            with self._state_lock:
                self._pending = None

    def render(self, text=DEFAULT_TEXT):
        if not isinstance(text, str) or not text.strip():
            raise RenderError('Supply an original dedication text.', stage='input')
        with self._state_lock:
            if self._rendered:
                raise RenderError('Create a new renderer for another rehearsal.', stage='input')
            self._rendered = True
        prepared = None
        try:
            prepared = self.voice.prepare(text)
            source = self._private_wave(str(prepared.path), 'voice.wav')
            samples = read_voice_pcm(source)
            voice_stats = analyze_pcm(samples)
            if not voice_stats['nonSilent']:
                raise RenderError('The prepared voice contains only digital silence.', stage='analysis')
            voice_path = self.directory / 'voice.wav'
            mix_path = self.directory / 'mix.wav'
            if voice_path.exists() or voice_path.is_symlink() or mix_path.exists() or mix_path.is_symlink():
                raise RenderError('Rehearsal output filenames must be unused.', stage='output')
            shutil.copyfile(source, voice_path)
            mixed, envelope = mix_with_bed(samples)
            write_pcm(mix_path, mixed)
            return {'status': 'rendered', 'sourceText': text, 'spokenText': self.spoken_text,
                    'shortened': self.spoken_text != ' '.join(text.split()),
                    'executables': self.executables, 'approvedProcessSpawns': self.approved_spawns,
                    'voice': voice_stats, 'mix': analyze_pcm(mixed), 'envelope': envelope,
                    'audibleVerification': 'unperformed', 'pronunciationVerification': 'unperformed',
                    'files': {'voice': str(voice_path), 'mix': str(mix_path)}}
        finally:
            if prepared is not None:
                prepared.close()
