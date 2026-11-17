"""Recovery decisions use a fake desktop/controller; no live browser is opened."""
from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

import spotify_reconnect as recovery


def process(pid, executable, arguments=(), parent=1):
    return {'pid': pid, 'ppid': parent, 'started': pid, 'exe': executable, 'args': list(arguments)}


def status(state):
    return {'state': state, 'connected': state == 'connected', 'pending': 0,
            'prepared_pages': [], 'consent_diagnostics': {'events': [], 'last_seq': 0}}


class FakeSystem:
    def __init__(self, states, processes=(), native=False):
        self.states, self.index, self.time = states, 0, 0.0
        self.current_processes = list(processes)
        self.native = native
        self.actions, self.calls = [], []

    def clock(self):return self.time
    def sleep(self, seconds):self.time += seconds
    def processes(self):return self.current_processes
    def check_children(self):pass
    def probe(self, url):
        if url == recovery.NATIVE:
            return [{'type': 'page', 'webSocketDebuggerUrl': 'ws://native'}] if self.native else None
        result = self.states[min(self.index, len(self.states) - 1)]
        self.index += 1
        return result
    def http(self, url, body=None, timeout=3):
        self.calls.append((url, body))
        return {'ok': True}
    def start_daemon(self):
        self.actions.append({'action': 'start_daemon'})
        self.current_processes.append(process(10, '/usr/bin/python3', ['/skill/cdp_daemon.py']))
    def start_browser(self):
        self.actions.append({'action': 'start_chrome'})
        self.current_processes.append(process(11, '/opt/google/chrome/chrome', ['Google Chrome']))
    def verify(self, native, deadline, emit):
        self.calls.append(('verify', native))
        return {'mode': 'native' if native else 'native-via-web-controller',
                'track': {'title': 'Current song', 'progressMs': 5000}}


class RecoveryDecisions(unittest.TestCase):
    def test_native_endpoint_needs_no_browser_or_daemon(self):
        system = FakeSystem([None], native=True)
        result = recovery.ensure(system)
        self.assertEqual(result['mode'], 'native')
        self.assertEqual(system.actions, [])
        self.assertEqual(system.calls, [('verify', True)])

    def test_connected_daemon_is_reused_even_if_process_metadata_is_unavailable(self):
        system = FakeSystem([status('connected')])
        self.assertTrue(recovery.ensure(system)['ok'])
        self.assertEqual(system.actions, [])
        self.assertEqual(system.calls, [('verify', False)])

    def test_expired_native_verification_cannot_start_dependencies_after_deadline(self):
        system = FakeSystem([None], native=True)
        def expired(native, deadline, emit):
            system.time = deadline + 1
            raise recovery.ReconnectError('verification_timeout', 'Native did not answer.')
        system.verify = expired
        with self.assertRaises(recovery.ReconnectError) as caught:
            recovery.ensure(system, timeout=1)
        self.assertEqual(caught.exception.code, 'verification_timeout')
        self.assertEqual(system.actions, [])

    def test_desktop_crash_starts_one_daemon_and_one_browser(self):
        system = FakeSystem([None, status('no_chrome'), status('waiting_for_allow'), status('connected')])
        self.assertTrue(recovery.ensure(system)['ok'])
        self.assertEqual([a['action'] for a in system.actions], ['start_daemon', 'start_chrome', 'reconnect_daemon'])
        self.assertEqual(sum(url == recovery.DAEMON + '/reconnect' for url, body in system.calls), 1)

    def test_existing_browser_with_rewritten_title_is_not_duplicated(self):
        main = process(20, '/opt/google/chrome/chrome', ['/opt/google/chrome/chrome --profile-directory=Default'])
        renderer = process(21, '/opt/google/chrome/chrome', ['Chrome renderer --type=renderer'], parent=20)
        system = FakeSystem([None, status('connected')], [main, renderer])
        recovery.ensure(system)
        self.assertEqual([a['action'] for a in system.actions], ['start_daemon'])
        self.assertEqual(recovery.browser_processes([main, renderer]), [main])

    def test_unresponsive_existing_daemon_is_not_replaced(self):
        system = FakeSystem([None], [process(4, '/usr/bin/python3', ['/skill/cdp_daemon.py'])])
        with self.assertRaises(recovery.ReconnectError) as caught:
            recovery.ensure(system)
        self.assertEqual(caught.exception.code, 'daemon_unresponsive')
        self.assertEqual(system.actions, [])

    def test_old_failed_status_does_not_abort_the_new_consent_attempt(self):
        main = process(20, '/opt/google/chrome/chrome', ['Chrome'])
        system = FakeSystem([status('failed'), status('failed'), status('failed'),
                             status('waiting_for_allow'), status('connected')], [main])
        self.assertTrue(recovery.ensure(system)['ok'])
        self.assertEqual(system.actions, [{'action': 'reconnect_daemon'}])

    def test_failed_new_consent_attempt_reports_diagnostics_without_dialog_loop(self):
        main = process(20, '/opt/google/chrome/chrome', ['Chrome'])
        system = FakeSystem([status('failed'), status('failed'), status('waiting_for_allow'), status('failed')], [main])
        with self.assertRaises(recovery.ReconnectError) as caught:
            recovery.ensure(system)
        self.assertEqual(caught.exception.code, 'debugging_consent_required')
        self.assertEqual(caught.exception.details['state'], 'failed')
        self.assertEqual(system.actions, [{'action': 'reconnect_daemon'}])

    def test_unexpected_port_owner_is_not_replaced(self):
        system = FakeSystem([{'server': 'something else'}])
        with self.assertRaises(recovery.ReconnectError) as caught:
            recovery.ensure(system)
        self.assertEqual(caught.exception.code, 'unexpected_daemon')
        self.assertEqual(system.actions, [])


class LocalRecovery(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.folder = Path(self.directory.name)
        self.system = recovery.LocalSystem(self.folder, self.folder)

    def test_fresh_manager_environment_replaces_dead_display_fields(self):
        manager = Mock(returncode=0, stdout=b'DISPLAY=:1\nXAUTHORITY=/run/new-auth\nDBUS_SESSION_BUS_ADDRESS=unix:path=/run/new-bus\n')
        with patch.object(recovery.subprocess, 'run', return_value=manager), \
                patch.object(recovery, 'display_available', return_value=True), \
                patch.dict(recovery.os.environ, {'DISPLAY': ':9', 'XAUTHORITY': '/gone', 'WAYLAND_DISPLAY': 'gone-wayland'}):
            environment, source = recovery.desktop_environment([])
        self.assertEqual(environment['DISPLAY'], ':1')
        self.assertEqual(environment['XAUTHORITY'], '/run/new-auth')
        self.assertNotIn('WAYLAND_DISPLAY', environment)
        self.assertEqual(source, 'systemd-user')

    def test_browser_start_reuses_the_existing_profile_in_a_regular_window(self):
        chrome = self.folder / '.config/google-chrome'
        (chrome / 'Profile 1').mkdir(parents=True)
        (chrome / 'Local State').write_text(json.dumps({'profile': {'last_used': 'Profile 1'}}))
        self.system.launch = Mock()
        with patch.object(recovery.Path, 'home', return_value=self.folder), \
                patch.object(recovery.shutil, 'which', return_value='/usr/bin/google-chrome'):
            self.system.start_browser()
        arguments, label = self.system.launch.call_args.args
        self.assertEqual(label, 'chrome')
        self.assertEqual(arguments, ['/usr/bin/google-chrome', '--profile-directory=Profile 1', 'https://open.spotify.com/'])

    def test_missing_profile_cannot_create_a_new_signed_out_profile(self):
        self.system.launch = Mock()
        with patch.object(recovery.Path, 'home', return_value=self.folder):
            with self.assertRaises(recovery.ReconnectError) as caught:
                self.system.start_browser()
        self.assertEqual(caught.exception.code, 'profile_missing')
        self.system.launch.assert_not_called()

    def test_preparation_failure_preserves_details_and_releases_its_session(self):
        import spotify_native
        client = Mock(session=None)
        calls = []
        detail = {'error': {'kind': 'browser_state_unavailable', 'stage': 'browser_state', 'message': 'Helper absent'}}
        def request(url, body=None, timeout=3):
            calls.append((url, body))
            if url.endswith('/targets'):return [{'type': 'page', 'url': 'https://open.spotify.com/', 'targetId': 'existing'}]
            if url.endswith('/attach'):return {'sessionId': 'owned'}
            if url.endswith('/prepare'):raise recovery.ReconnectError('http_error', 'prepare failed', detail)
            return {'result': {}}
        self.system.http = request
        with patch.object(spotify_native, 'SpotifyClient', return_value=client):
            with self.assertRaises(recovery.ReconnectError) as caught:
                self.system.verify(False, self.system.clock() + 5, lambda *a: None)
        self.assertEqual(caught.exception.code, 'page_preparation_failed')
        self.assertEqual(caught.exception.details, detail)
        self.assertEqual([url.rsplit('/', 1)[-1] for url, body in calls], ['targets', 'attach', 'prepare', 'release', 'cdp'])
        self.assertEqual(calls[-1][1]['method'], 'Target.detachFromTarget')
        client.status.assert_not_called()

    def test_fresh_playback_verification_uses_status_only(self):
        import spotify_native
        first = {'nowPlaying': {'uri': 'spotify:track:' + 'a' * 22, 'title': 'Real song',
                 'playing': True, 'progressMs': 1000, 'observedAt': 1000000}}
        second = {'nowPlaying': {**first['nowPlaying'], 'progressMs': 2000}}
        client = Mock(mode='native', session=None)
        client.status.side_effect = [first, second]
        self.system.sleep = lambda unused: None
        with patch.object(spotify_native, 'SpotifyClient', return_value=client), \
                patch.object(recovery.time, 'time', return_value=1000):
            result = self.system.verify(True, self.system.clock() + 5, lambda *a: None)
        self.assertEqual(result['track']['title'], 'Real song')
        self.assertEqual(result['track']['progressMs'], 2000)
        self.assertEqual(client.status.call_count, 2)
        client.control.assert_not_called()
        client.enqueue.assert_not_called()
        client.close.assert_called_once()

    def test_old_or_invalid_track_samples_are_rejected(self):
        for track in ({'uri': 'track', 'title': 'Old', 'progressMs': 0, 'observedAt': 1000},
                      {'uri': 'track', 'title': 'Bad', 'progressMs': float('nan'), 'observedAt': 10000}):
            with self.assertRaises(recovery.ReconnectError):
                recovery.track_observation({'nowPlaying': track}, 10000)
        record = recovery.track_observation({'observedAt': 10000, 'nowPlaying': {'uri': 'track', 'title': 'Paused', 'progressMs': 0}}, 10000)
        self.assertEqual(record['observedAt'], 10000)


if __name__ == '__main__':
    unittest.main()
