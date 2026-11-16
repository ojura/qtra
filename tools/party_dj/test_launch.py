"""Launcher ownership and cleanup checks without real processes or network."""
from __future__ import annotations

from contextlib import ExitStack
from pathlib import Path
import signal
import subprocess
import tempfile
import unittest
from unittest.mock import Mock, patch

import launch


class LaunchTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='larics-launch-test-')
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.data = self.root / 'party'
        self.data.mkdir()
        self.proc = self.root / 'proc'
        self.process = self.proc / '12345'
        self.process.mkdir(parents=True)
        (self.process / 'cwd').symlink_to(launch.REPO, target_is_directory=True)
        original_path = Path
        paths = patch.object(launch, 'Path', side_effect=lambda value: self.proc if value == '/proc' else original_path(value))
        paths.start()
        self.addCleanup(paths.stop)

    def record_process(self, data, port):
        command = ['python3', str(launch.ROOT / 'server.py'), '--data', str(data), '--port', str(port)]
        (self.process / 'cmdline').write_bytes(b'\0'.join(part.encode() for part in command) + b'\0')
        (self.data / 'server.pid').write_text('12345')

    def test_stop_rejects_a_pid_using_another_data_directory(self):
        self.record_process(self.root / 'another-party', 8787)
        with patch.object(launch.os, 'kill') as kill:
            with self.assertRaisesRegex(RuntimeError, 'different state directory'):
                launch.stop_server(self.data, 8787)
        kill.assert_not_called()
        self.assertEqual((self.data / 'server.pid').read_text(), '12345')

    def test_start_rejects_a_recorded_server_on_another_port(self):
        self.record_process(self.data, 8787)
        arguments = ['launch.py', 'start', '--service-only', '--data', str(self.data), '--port', '8788']
        with patch.object(launch.sys, 'argv', arguments), \
             patch.object(launch.subprocess, 'Popen') as spawn, \
             patch.object(launch.os, 'kill') as kill, \
             patch.object(launch, 'health') as health:
            with self.assertRaisesRegex(RuntimeError, 'port 8787.*restart'):
                launch.main()
        spawn.assert_not_called()
        kill.assert_not_called()
        health.assert_not_called()

    def test_failed_start_cleans_only_the_spawned_child_and_matching_pid(self):
        for escalation in (False, True):
            with self.subTest(escalation=escalation), ExitStack() as context:
                child = Mock()
                child.pid = 34567
                child.poll.return_value = None
                if escalation:
                    child.wait.side_effect = [subprocess.TimeoutExpired('fixture', 10),
                                              subprocess.TimeoutExpired('fixture', 5), 0]
                else:
                    child.wait.return_value = 0
                clock = Mock()
                clock.monotonic.side_effect = [0, 11]
                arguments = ['launch.py', 'start', '--service-only', '--data', str(self.data)]
                context.enter_context(patch.object(launch.sys, 'argv', arguments))
                context.enter_context(patch.object(launch, 'owned_server', return_value=None))
                context.enter_context(patch.object(launch, 'health', return_value=None))
                context.enter_context(patch.object(launch, 'time', clock))
                context.enter_context(patch.object(launch.subprocess, 'Popen', return_value=child))
                stage = context.enter_context(patch.object(launch.subprocess, 'run'))
                kill = context.enter_context(patch.object(launch.os, 'kill'))
                with self.assertRaisesRegex(RuntimeError, 'did not start'):
                    launch.main()
                child.send_signal.assert_called_once_with(signal.SIGINT)
                self.assertFalse((self.data / 'server.pid').exists())
                kill.assert_not_called()
                stage.assert_not_called()
                if escalation:
                    child.terminate.assert_called_once_with()
                    child.kill.assert_called_once_with()
                else:
                    child.terminate.assert_not_called()
                    child.kill.assert_not_called()


if __name__ == '__main__':
    unittest.main()
