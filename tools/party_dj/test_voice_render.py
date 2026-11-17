"""Pure file-render regressions: fake executors, private PCM and denied live boundaries."""
from array import array
import math
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import unittest
from unittest.mock import patch
import wave

from speech import CroatianVoice
from voice_render import (DEFAULT_TEXT, DUCK_GAIN, RenderError, SAMPLE_RATE, VoiceRenderer,
                          analyze_pcm, mix_with_bed, write_pcm)


class FakeExecutor:
    def __init__(self):
        self.renderer = None
        self.calls = []
        self.probe = None
        self.failure = None
        self.duration = lambda text: .05
        self.silent = False
        self.output_rate = SAMPLE_RATE
        self.return_code = 0
        self.omit_return_code = False

    def __call__(self, argv, **options):
        self.calls.append((list(argv), dict(options)))
        if self.probe:
            self.probe(argv, options)
        renderer = self.renderer
        if not renderer.allow_spawn(argv[0], argv, options['cwd'], options['env']):
            raise AssertionError('Fake spawn was not admitted through the exact pending gate.')
        if renderer.allow_spawn(argv[0], argv, options['cwd'], options['env']):
            raise AssertionError('One pending command admitted a second spawn.')
        if self.failure:
            raise self.failure
        if argv[0] == renderer.executables['espeak']:
            count = round(self.duration(options['input']) * SAMPLE_RATE)
            samples = array('h', [0] * count) if self.silent else array('h',
                (round(5000 * math.sin(2 * math.pi * 600 * index / SAMPLE_RATE)) for index in range(count)))
            write_pcm(argv[10], samples)
        else:
            if self.output_rate == SAMPLE_RATE:
                shutil.copyfile(argv[8], argv[-1])
            else:
                with wave.open(argv[-1], 'wb') as target:
                    target.setnchannels(1)
                    target.setsampwidth(2)
                    target.setframerate(self.output_rate)
                    target.writeframes(array('h', [300, -300] * 100).tobytes())
        return (object() if self.omit_return_code else
                subprocess.CompletedProcess(argv, self.return_code, stdout=b'', stderr=b''))


class VoiceRenderTests(unittest.TestCase):
    def setUp(self):
        super().setUp()
        for name in ('subprocess.run', 'subprocess.Popen', 'socket.socket'):
            boundary = patch(name, side_effect=AssertionError('Forbidden live test boundary: ' + name))
            guarded = boundary.start()
            self.addCleanup(boundary.stop)
            self.addCleanup(guarded.assert_not_called)
        self.temporary = tempfile.TemporaryDirectory(prefix='voice-render-test-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.espeak, self.ffmpeg = self.root / 'espeak-ng', self.root / 'ffmpeg'
        for path in (self.espeak, self.ffmpeg):
            path.write_text('not an executable fixture\n')
            path.chmod(0o700)
        self.executor = FakeExecutor()
        self.renderer = self.new_renderer(self.executor)

    def voice_factory(self, directory, *, run):
        voice = CroatianVoice(directory, run=run)
        voice.command, voice.ffmpeg, voice.available = str(self.espeak), str(self.ffmpeg), True
        return voice

    def new_renderer(self, executor):
        renderer = VoiceRenderer(self.root, run=executor, voice_factory=self.voice_factory)
        executor.renderer = renderer
        return renderer

    def command_paths(self):
        directory = self.root / 'voice-command'
        directory.mkdir(exist_ok=True)
        return directory / 'raw.wav', directory / 'voice.wav'

    def synthesis_command(self):
        raw, unused_ready = self.command_paths()
        return ([str(self.espeak), '-v', 'hr', '-s', '180', '-p', '43', '-a', '110', '-w', str(raw), '--stdin'],
                {'input': DEFAULT_TEXT, 'text': True, 'capture_output': True, 'timeout': 15, 'check': True})

    def conversion_command(self):
        raw, ready = self.command_paths()
        write_pcm(raw, [300, -300] * 2400)
        filters = ('highpass=f=80,lowpass=f=8500,loudnorm=I=-19:TP=-3:LRA=6,'
                   'afade=t=in:st=0:d=0.03,afade=t=out:st=0.020:d=0.08')
        return ([str(self.ffmpeg), '-nostdin', '-loglevel', 'error', '-y', '-i', str(raw),
                 '-af', filters, '-ar', '48000', '-ac', '1', str(ready)],
                {'capture_output': True, 'timeout': 15, 'check': True})

    def test_real_prepare_algorithm_uses_only_reviewed_fake_file_commands(self):
        report = self.renderer.render()
        self.assertEqual(report['status'], 'rendered')
        self.assertEqual(report['sourceText'], DEFAULT_TEXT)
        self.assertEqual(report['spokenText'], DEFAULT_TEXT)
        self.assertFalse(report['shortened'])
        self.assertEqual(report['approvedProcessSpawns'], 2)
        self.assertEqual(report['executables'], {'espeak': str(self.espeak), 'ffmpeg': str(self.ffmpeg)})
        self.assertEqual(report['audibleVerification'], 'unperformed')
        self.assertEqual(report['pronunciationVerification'], 'unperformed')
        self.assertTrue(report['voice']['nonSilent'])
        self.assertEqual(report['voice']['sampleRate'], SAMPLE_RATE)
        self.assertEqual(report['voice']['sampleWidthBits'], 16)
        self.assertEqual(report['voice']['clippedSamples'], 0)
        self.assertEqual(report['envelope']['limitedSamples'], 0)
        for path in report['files'].values():
            self.assertEqual(Path(path).parent, self.root)
            self.assertTrue(Path(path).is_file())
        self.assertEqual(list(self.root.glob('voice-*')), [])
        self.assertFalse(self.renderer.active_launch_on_thread())
        for argv, options in self.executor.calls:
            self.assertEqual(options['cwd'], str(self.root))
            self.assertIs(options['shell'], False)
            self.assertIs(options['close_fds'], True)
            self.assertEqual(options['env'], {'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8', 'TMPDIR': str(self.root)})
        argv, options = self.executor.calls[1]
        self.assertEqual(argv[5:7], ['-protocol_whitelist', 'file'])
        self.assertEqual(argv[-5:-1], ['-c:a', 'pcm_s16le', '-f', 'wav'])
        self.assertEqual(options['input'], b'')

    def test_spawn_permission_is_exact_same_thread_single_use_and_cleared(self):
        def probe(argv, options):
            self.assertTrue(self.renderer.active_launch_on_thread())
            self.assertFalse(self.renderer.allow_spawn('/bin/sh', argv, options['cwd'], options['env']))
            self.assertFalse(self.renderer.allow_spawn(argv[0], argv + ['extra'], options['cwd'], options['env']))
            self.assertFalse(self.renderer.allow_spawn(argv[0], argv, '/', options['env']))
            self.assertFalse(self.renderer.allow_spawn(argv[0], argv, options['cwd'], {**options['env'], 'PULSE_SINK': 'default'}))
            answers = []
            worker = threading.Thread(target=lambda: answers.extend([
                self.renderer.active_launch_on_thread(),
                self.renderer.allow_spawn(argv[0], argv, options['cwd'], options['env'])]))
            worker.start()
            worker.join(2)
            self.assertFalse(worker.is_alive())
            self.assertEqual(answers, [False, False])
        self.executor.probe = probe
        self.renderer.render()
        argv, options = self.executor.calls[-1]
        self.assertFalse(self.renderer.allow_spawn(argv[0], argv, options['cwd'], options['env']))

    def test_rejects_changed_executable_flags_and_nonprivate_synthesis_paths_before_executor(self):
        original, options = self.synthesis_command()
        candidates = []
        for index, value in ((0, '/bin/sh'), (2, 'en'), (8, '200'),
                             (10, 'pipe:1'), (10, 'https://example.invalid/out.wav'),
                             (10, '/dev/stdout'), (10, str(self.root / 'raw.wav'))):
            changed = list(original)
            changed[index] = value
            candidates.append(changed)
        candidates.extend([original + ['--stdout'], original[:-1], ['sh', '-c', 'echo fixture']])
        for argv in candidates:
            with self.subTest(argv=argv), self.assertRaises(RenderError):
                self.renderer._run(argv, **options)
        self.assertEqual(self.executor.calls, [])

    def test_rejects_symlink_synthesis_output_before_executor(self):
        argv, options = self.synthesis_command()
        target = Path(argv[10])
        target.write_bytes(b'fixture')
        is_symlink = Path.is_symlink
        with patch.object(Path, 'is_symlink', autospec=True,
                          side_effect=lambda path: path == target or is_symlink(path)):
            with self.assertRaises(RenderError):
                self.renderer._run(argv, **options)
        self.assertEqual(target.read_bytes(), b'fixture')
        self.assertEqual(self.executor.calls, [])

    def test_rejects_extra_execution_options_and_nonstdin_text_before_executor(self):
        argv, original = self.synthesis_command()
        for change in ({'shell': True}, {'cwd': '/'}, {'env': {'PATH': '/bin'}},
                       {'stdin': 0}, {'input': '\x00'}, {'input': '  extra whitespace  '},
                       {'text': False}, {'timeout': 99}, {'capture_output': False}, {'check': False}):
            with self.subTest(change=change), self.assertRaises(RenderError):
                self.renderer._run(argv, **{**original, **change})
        self.assertEqual(self.executor.calls, [])

    def test_rejects_conversion_filters_protocols_paths_and_extra_flags_before_executor(self):
        original, options = self.conversion_command()
        candidates = []
        for index, value in ((8, 'volume=4'), (8, original[8].replace('st=0.020', 'st=1.020')),
                             (6, 'https://example.invalid/in.wav'), (13, 'pipe:1'),
                             (13, str(self.root / 'voice.wav')), (10, '44100')):
            changed = list(original)
            changed[index] = value
            candidates.append(changed)
        candidates.append(original + ['-protocol_whitelist', 'file,http'])
        for argv in candidates:
            with self.subTest(argv=argv), self.assertRaises(RenderError):
                self.renderer._run(argv, **options)
        self.assertEqual(self.executor.calls, [])

    def test_nonzero_exit_and_timeout_keep_diagnostic_detail_and_clear_permission(self):
        failures = [subprocess.CalledProcessError(7, 'fixture', stderr=b'fixture filter failure'),
                    subprocess.TimeoutExpired('fixture', 15, stderr=b'fixture stalled')]
        for failure in failures:
            with self.subTest(failure=type(failure).__name__):
                executor = FakeExecutor()
                executor.failure = failure
                renderer = self.new_renderer(executor)
                with self.assertRaises(RenderError) as caught:
                    renderer.render()
                detail = caught.exception.as_dict()
                self.assertEqual(detail['stage'], 'synthesis')
                self.assertIn('fixture', detail['stderr'])
                if isinstance(failure, subprocess.CalledProcessError):
                    self.assertEqual(detail['returnCode'], 7)
                self.assertFalse(renderer.active_launch_on_thread())
                self.assertEqual(list(self.root.glob('voice-*')), [])
        self.assertEqual(len(RenderError('failed', stderr='x' * 2000).stderr), 1000)

    def test_false_none_or_missing_exit_code_cannot_report_success(self):
        for name, value in (('false', False), ('none', None), ('missing', None)):
            with self.subTest(exit_code=name):
                executor = FakeExecutor()
                executor.return_code = value
                executor.omit_return_code = name == 'missing'
                renderer = self.new_renderer(executor)
                with self.assertRaisesRegex(RenderError, 'successful exit'):
                    renderer.render()
                self.assertFalse(renderer.active_launch_on_thread())
                self.assertEqual(list(self.root.glob('voice-*')), [])

    def test_missing_audit_gate_cannot_report_success(self):
        def ungated(argv, **options):
            return subprocess.CompletedProcess(argv, 0)
        renderer = VoiceRenderer(self.root, run=ungated, voice_factory=self.voice_factory)
        with self.assertRaisesRegex(RenderError, 'process audit gate'):
            renderer.render()
        self.assertFalse(renderer.active_launch_on_thread())
        self.assertEqual(list(self.root.glob('voice-*')), [])

    def test_renderer_captures_the_patched_executor_at_construction(self):
        with patch('subprocess.run', side_effect=RuntimeError('construction tripwire')) as tripwire:
            renderer = VoiceRenderer(self.root, voice_factory=self.voice_factory)
        with self.assertRaisesRegex(RuntimeError, 'construction tripwire'):
            renderer.render()
        tripwire.assert_called_once()
        self.assertFalse(renderer.active_launch_on_thread())

    def test_croatian_voice_captures_the_patched_executor_at_construction(self):
        with patch('subprocess.run', side_effect=RuntimeError('voice construction tripwire')) as tripwire:
            voice = CroatianVoice(self.root)
        voice.command, voice.ffmpeg, voice.available = str(self.espeak), str(self.ffmpeg), True
        with self.assertRaisesRegex(RuntimeError, 'voice construction tripwire'):
            voice.prepare(DEFAULT_TEXT)
        tripwire.assert_called_once()
        self.assertEqual(list(self.root.glob('voice-*')), [])

    def test_report_records_the_last_successfully_synthesized_shortened_text(self):
        self.executor.duration = lambda text: 12 if len(text.split()) > 5 else .05
        report = self.renderer.render()
        actual_inputs = [options['input'] for argv, options in self.executor.calls
                         if argv[0] == str(self.espeak)]
        self.assertGreater(len(actual_inputs), 1)
        self.assertEqual(report['spokenText'], actual_inputs[-1])
        self.assertNotEqual(report['spokenText'], report['sourceText'])
        self.assertTrue(report['shortened'])

    def test_pcm_measurements_report_silence_known_level_and_full_scale(self):
        stats = analyze_pcm([16384, -16384] * 50)
        self.assertAlmostEqual(stats['rmsDbfs'], -6.020599913279624)
        self.assertAlmostEqual(stats['peakDbfs'], -6.020599913279624)
        self.assertEqual(stats['nonzeroFraction'], 1)
        self.assertEqual(stats['clippedSamples'], 0)
        silence = analyze_pcm([0] * 100)
        self.assertIsNone(silence['rmsDbfs'])
        self.assertIsNone(silence['peakDbfs'])
        self.assertFalse(silence['nonSilent'])
        self.assertEqual(analyze_pcm([32767, -32768, 0])['clippedSamples'], 2)
        for invalid in ([], [40000], [1.5], [True]):
            with self.subTest(invalid=invalid), self.assertRaises(RenderError):
                analyze_pcm(invalid)

    def test_mix_measures_ten_db_duck_smooth_fades_and_restored_bed(self):
        voice = [300, -300] * 2400
        mixed, envelope = mix_with_bed(voice)
        self.assertEqual(envelope['duckSeconds'], .48)
        self.assertEqual(envelope['restoreSeconds'], .88)
        self.assertAlmostEqual(envelope['measuredHoldGainDb'], -10, places=2)
        self.assertEqual(envelope['measuredPreGainDb'], 0)
        self.assertEqual(envelope['measuredPostGainDb'], 0)
        self.assertEqual(envelope['minimumGain'], DUCK_GAIN)
        self.assertEqual(envelope['finalGain'], 1)
        self.assertLess(envelope['maximumStep'], .0001)
        self.assertEqual(envelope['limitedSamples'], 0)
        self.assertEqual(analyze_pcm(mixed)['clippedSamples'], 0)
        expected = round((.75 + .48 + .1 + .88 + .75) * SAMPLE_RATE)
        self.assertEqual(len(mixed), expected)

    def test_mix_reports_limited_samples_for_an_overfull_fixture(self):
        mixed, envelope = mix_with_bed([32767] * 4800)
        self.assertGreater(envelope['limitedSamples'], 0)
        self.assertGreater(analyze_pcm(mixed)['clippedSamples'], 0)

    def test_silent_or_wrong_format_render_fails_and_closes_temporary_voice(self):
        for setting, value, expected in (('silent', True, 'digital silence'),
                                         ('output_rate', 44100, '48 kHz mono PCM16')):
            with self.subTest(setting=setting):
                executor = FakeExecutor()
                setattr(executor, setting, value)
                renderer = self.new_renderer(executor)
                with self.assertRaisesRegex(RenderError, expected):
                    renderer.render()
                self.assertEqual(list(self.root.glob('voice-*')), [])
                self.assertFalse((self.root / 'voice.wav').exists())
                self.assertFalse((self.root / 'mix.wav').exists())


if __name__ == '__main__':
    unittest.main()
