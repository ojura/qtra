#!/usr/bin/env python3
"""Restore the existing Spotify controller after a desktop or browser restart.

Only the missing CDP daemon and one Chrome browser may be started. Spotify's
process, transport, devices, queue and volume are never changed by this helper.
"""
from __future__ import annotations

import argparse
import fcntl
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
DEPS = REPO / 'build/party-deps'
if DEPS.is_dir():
    sys.path.insert(0, str(DEPS))

DAEMON = 'http://127.0.0.1:7799'
NATIVE = 'http://127.0.0.1:9224/json/list'
DESKTOP_KEYS = ('DISPLAY', 'XAUTHORITY', 'WAYLAND_DISPLAY', 'XDG_RUNTIME_DIR',
                'DBUS_SESSION_BUS_ADDRESS', 'XDG_SESSION_TYPE', 'XDG_CURRENT_DESKTOP')


class ReconnectError(RuntimeError):
    def __init__(self, code, message, details=None):
        super().__init__(message)
        self.code, self.details = code, details


def http_json(url, body=None, timeout=3):
    request = urllib.request.Request(url, data=None if body is None else json.dumps(body).encode(),
                                     headers={'Content-Type': 'application/json'})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return json.load(response)
    except urllib.error.HTTPError as error:
        raw = error.read().decode(errors='replace')
        try: detail = json.loads(raw)
        except ValueError: detail = {'message': raw[:1200]}
        raise ReconnectError('http_error', f'{urlsplit(url).path} returned HTTP {error.code}', detail) from error


def process_inventory(proc=Path('/proc'), uid=None):
    """Executable identity survives Chrome's rewritten argv[0] and comm."""
    uid = os.getuid() if uid is None else uid
    result = []
    for directory in proc.iterdir():
        if not directory.name.isdigit():
            continue
        try:
            if directory.stat().st_uid != uid:
                continue
            executable = os.readlink(directory / 'exe').removesuffix(' (deleted)')
            arguments = [part.decode(errors='replace') for part in (directory / 'cmdline').read_bytes().split(bytes([0])) if part]
            stat = (directory / 'stat').read_text().rsplit(')', 1)[1].split()
            if stat[0] == 'Z':
                continue
            result.append({'pid': int(directory.name), 'ppid': int(stat[1]),
                           'started': int(stat[19]), 'exe': executable, 'args': arguments})
        except (OSError, ValueError, IndexError):
            continue
    return result


def browser_processes(processes):
    chrome = [p for p in processes if Path(p['exe']).name in ('chrome', 'google-chrome', 'chromium', 'chromium-browser')]
    identifiers = {p['pid'] for p in chrome}
    return [p for p in chrome if p['ppid'] not in identifiers
            and not re.search(r'(?:^|\s)--type(?:=|\s)', ' '.join(p['args']))]


def daemon_processes(processes):
    return [p for p in processes if Path(p['exe']).name.startswith('python')
            and any(Path(arg).name == 'cdp_daemon.py' for arg in p['args'])]


def selected_environment(raw):
    values = dict(part.decode(errors='replace').split('=', 1)
                  for part in raw.split(bytes([0])) if b'=' in part)
    return {key: values[key] for key in DESKTOP_KEYS if values.get(key)}


def display_available(environment):
    runtime = environment.get('XDG_RUNTIME_DIR', f'/run/user/{os.getuid()}')
    wayland = environment.get('WAYLAND_DISPLAY')
    if wayland and (Path(runtime) / wayland).exists():
        return True
    display = re.fullmatch(r':(\d+)(?:\.\d+)?', environment.get('DISPLAY', ''))
    if not display or not Path('/tmp/.X11-unix', 'X' + display[1]).exists():
        return False
    authority = environment.get('XAUTHORITY')
    return not authority or Path(authority).is_file()


def desktop_environment(processes):
    """Use the current session manager, then a surviving desktop application."""
    candidates = []
    try:
        manager = subprocess.run(['systemctl', '--user', 'show-environment'],
                                 capture_output=True, timeout=5, check=False)
        if manager.returncode == 0:
            candidates.append(('systemd-user', selected_environment(manager.stdout.replace(b'\n', bytes([0])))))
    except (OSError, subprocess.TimeoutExpired):
        pass
    desktop_names = {'gnome-shell', 'gnome-session-binary', 'qt_runtime_cube'}
    for process in sorted(processes, key=lambda p: p['started'], reverse=True):
        if Path(process['exe']).name not in desktop_names:
            continue
        try:
            candidates.append((f"process:{process['pid']}", selected_environment(
                Path('/proc', str(process['pid']), 'environ').read_bytes())))
        except OSError:
            pass
    for source, fields in candidates:
        if display_available(fields):
            environment = os.environ.copy()
            for key in DESKTOP_KEYS:
                environment.pop(key, None)
            environment.update(fields)
            return environment, source
    raise ReconnectError('desktop_unavailable', 'No live desktop display was found. Sign into the desktop and rerun the helper.')


def daemon_diagnostics(status):
    if not isinstance(status, dict):
        return {'state': 'unavailable'}
    events = status.get('consent_diagnostics', {}).get('events', [])
    useful = [event for event in events if event.get('event') not in
              ('scan_started', 'scan_finished', 'accessibility_health_started', 'accessibility_health_finished')]
    return {'state': status.get('state'), 'connected': bool(status.get('connected')),
            'diag': status.get('diag', ''), 'pending': status.get('pending', 0),
            'preparedPageCount': len(status.get('prepared_pages', [])),
            'consentEvents': useful[-12:]}


def track_observation(status, sampled_at):
    track = status.get('nowPlaying') if isinstance(status, dict) else None
    if not isinstance(track, dict) or not track.get('title') or not track.get('uri'):
        raise ReconnectError('no_active_track', 'Spotify is reachable but has not reported an active track.')
    stamp = track.get('observedAt', status.get('observedAt'))
    if not isinstance(stamp, (int, float)) or not math.isfinite(stamp) or not -2000 <= sampled_at - stamp <= 5000:
        raise ReconnectError('stale_observation', 'Spotify returned an old or invalid playback observation.')
    progress = track.get('progressMs')
    if isinstance(progress, bool) or not isinstance(progress, (int, float)) or not math.isfinite(progress) or progress < 0:
        raise ReconnectError('invalid_observation', 'Spotify returned an invalid playback position.')
    return {**{key: track.get(key) for key in ('uri', 'uid', 'title', 'artist', 'playing', 'progressMs', 'durationMs')},
            'observedAt': stamp}


class LocalSystem:
    def __init__(self, data, skill_root):
        self.data = data
        self.skill_root = skill_root.resolve()
        self.children = []
        self.http = http_json
        self.clock = time.monotonic
        self.sleep = time.sleep
        self.actions = []

    def processes(self):
        return process_inventory()

    def probe(self, url):
        try:
            return self.http(url)
        except (OSError, ValueError, ReconnectError):
            return None

    def launch(self, arguments, label):
        environment, source = desktop_environment(self.processes())
        logfile = self.data / f'spotify-reconnect-{label}.log'
        with logfile.open('ab') as log:
            child = subprocess.Popen(arguments, env=environment, cwd=str(self.skill_root),
                                     stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        self.children.append((label, child, logfile))
        self.actions.append({'action': f'start_{label}', 'pid': child.pid, 'desktopSource': source,
                             'log': str(logfile)})

    def start_daemon(self):
        script = self.skill_root / 'cdp_daemon.py'
        if not script.is_file() or not (self.skill_root / 'clear_modals.py').is_file():
            raise ReconnectError('daemon_missing', f'The installed cdp-daemon files were not found in {self.skill_root}.')
        self.launch([sys.executable, '-u', str(script)], 'daemon')

    def start_browser(self):
        directory = Path.home() / '.config/google-chrome'
        try:
            profile = json.loads((directory / 'Local State').read_text()).get('profile', {}).get('last_used', 'Default')
        except (OSError, ValueError):
            profile = 'Default'
        if not isinstance(profile, str) or not profile or Path(profile).name != profile or not (directory / profile).is_dir():
            raise ReconnectError('profile_missing', 'The existing Chrome profile was not found; the helper will not create a replacement profile.')
        binary = shutil.which('google-chrome') or shutil.which('google-chrome-stable')
        if not binary:
            raise ReconnectError('chrome_missing', 'Google Chrome is not installed on this desktop.')
        # A regular window is required: --no-startup-window exited after this
        # desktop crash. Do not add a debugging port to Chrome's default profile.
        self.launch([binary, f'--profile-directory={profile}', 'https://open.spotify.com/'], 'chrome')

    def check_children(self):
        processes = self.processes()
        for label, child, log in self.children:
            result = child.poll()
            present = browser_processes(processes) if label == 'chrome' else daemon_processes(processes)
            # Chrome's launcher can exit after handing the request to its browser.
            if result is not None and not present:
                raise ReconnectError(f'{label}_exited', f'{label} exited with code {result}; read {log}.')

    def verify(self, native, deadline, emit):
        from spotify_native import SpotifyClient, SpotifyError
        client = SpotifyClient()
        session = None
        session_owned_by_client = False
        try:
            if not native:
                targets = self.http(DAEMON + '/targets', timeout=20)
                pages = [target for target in targets if target.get('type') == 'page'
                         and urlsplit(target.get('url', '')).hostname == 'open.spotify.com']
                if pages:
                    target = pages[0]['targetId']
                else:
                    created = self.http(DAEMON + '/cdp', {'method': 'Target.createTarget',
                        'params': {'url': 'https://open.spotify.com/', 'background': True}, 'timeout_seconds': 15}, timeout=20)
                    target = created['result']['targetId']
                    self.actions.append({'action': 'create_spotify_controller_tab', 'targetId': target})
                session = self.http(DAEMON + '/attach', {'targetId': target}, timeout=20)['sessionId']
                # Preserve the complete managed-preparation error for diagnostics.
                try:
                    self.http(DAEMON + '/prepare', {'sessionId': session, 'readiness': 'dom', 'timeout_seconds': 15}, timeout=20)
                except ReconnectError as error:
                    raise ReconnectError('page_preparation_failed',
                        'The Spotify page could not be prepared. Check the installed browser-state helper and the preparation diagnostic.', error.details) from error
                client.session = session
                session_owned_by_client = True
                client.mode = 'native-via-web-controller'
                client.last_native_check = time.monotonic()
            previous = None
            last_error = None
            while self.clock() < deadline:
                try:
                    current = track_observation(client.status(), int(time.time() * 1000))
                    if previous:
                        same = (current.get('uid') or current['uri']) == (previous.get('uid') or previous['uri'])
                        if not (same and current['playing'] and previous['playing']
                                and current['progressMs'] <= previous['progressMs']):
                            return {'mode': client.mode, 'track': current, 'previousTrack': previous,
                                    'verifiedAt': int(time.time() * 1000)}
                        last_error = ReconnectError('playback_not_advancing', 'The reported playing track is not advancing; its controller may be stale.')
                    previous = current
                except SpotifyError as error:
                    if 'Sign into Spotify' in str(error):
                        raise ReconnectError('spotify_sign_in_required', 'Sign into Spotify in the existing Chrome profile, then rerun the helper.') from error
                    last_error = ReconnectError('spotify_loading', str(error))
                except ReconnectError as error:
                    last_error = error
                emit('verifying_playback', {'detail': str(last_error) if last_error else 'Waiting for a second current playback observation.'})
                self.sleep(min(1, max(0, deadline - self.clock())))
            raise last_error or ReconnectError('verification_timeout', 'Spotify did not provide two current playback observations before the deadline.')
        finally:
            client.close()
            if session and not session_owned_by_client:
                # Preparation can fail before the session is assigned to client.
                for path, body in [('/release', {'sessionId': session, 'timeout_seconds': 3}),
                                   ('/cdp', {'method': 'Target.detachFromTarget', 'params': {'sessionId': session}, 'timeout_seconds': 3})]:
                    try: self.http(DAEMON + path, body, timeout=5)
                    except (OSError, ValueError, ReconnectError): pass


def inspect(system):
    targets = system.probe(NATIVE)
    native = isinstance(targets, list) and any(isinstance(target, dict) and target.get('type') == 'page'
        and target.get('webSocketDebuggerUrl') for target in targets)
    processes = system.processes()
    daemon = system.probe(DAEMON + '/status')
    if daemon is not None and (not isinstance(daemon, dict) or 'connected' not in daemon or 'state' not in daemon):
        raise ReconnectError('unexpected_daemon', 'Port 7799 answered with an unexpected service; no replacement was started.')
    return {'nativeAvailable': native, 'browserPids': [p['pid'] for p in browser_processes(processes)],
            'daemonPids': [p['pid'] for p in daemon_processes(processes)],
            'daemon': daemon_diagnostics(daemon)}, daemon


def ensure(system, timeout=180, emit=lambda phase, detail: None):
    deadline = system.clock() + timeout
    initial, daemon = inspect(system)
    emit('inspected', initial)
    if system.clock() >= deadline:
        raise ReconnectError('connection_timeout', 'The inspection consumed the wait budget; no recovery process was started.', initial)
    if initial['nativeAvailable']:
        try:
            verified = system.verify(True, min(deadline, system.clock() + 15), emit)
            return {'ok': True, **verified, 'actions': system.actions}
        except ReconnectError as error:
            emit('native_unavailable', {'code': error.code, 'detail': str(error)})
            if system.clock() >= deadline:
                raise
    if daemon and daemon.get('connected'):
        verified = system.verify(False, deadline, emit)
        return {'ok': True, **verified, 'actions': system.actions}
    if daemon is None:
        if initial['daemonPids']:
            raise ReconnectError('daemon_unresponsive', 'The existing CDP daemon is not responding; it was not replaced.', {'pids': initial['daemonPids']})
        system.start_daemon()
    if not browser_processes(system.processes()):
        system.start_browser()
    reconnected = False
    reconnect_sequence = 0
    saw_new_attempt = False
    last_state = None
    while system.clock() < deadline:
        system.check_children()
        daemon = system.probe(DAEMON + '/status')
        if daemon and daemon.get('connected'):
            verified = system.verify(False, deadline, emit)
            return {'ok': True, **verified, 'actions': system.actions}
        state = (daemon or {}).get('state', 'starting')
        if state != last_state:
            emit('waiting_for_browser', daemon_diagnostics(daemon))
            last_state = state
        if state in ('failed', 'no_chrome', 'disconnected') and not reconnected:
            reconnect_sequence = (daemon or {}).get('consent_diagnostics', {}).get('last_seq', 0)
            system.http(DAEMON + '/reconnect', {}, timeout=5)
            system.actions.append({'action': 'reconnect_daemon'})
            reconnected = True
        elif state == 'failed' and reconnected and (saw_new_attempt or any(
                event.get('seq', 0) > reconnect_sequence and event.get('event') == 'upgrade_finished'
                for event in daemon.get('consent_diagnostics', {}).get('events', []))):
            raise ReconnectError('debugging_consent_required',
                'Chrome debugging consent was not completed. Use Allow in Chrome, or enable remote debugging in chrome://inspect, then rerun.', daemon_diagnostics(daemon))
        if reconnected and state in ('connecting', 'waiting_for_allow'):
            saw_new_attempt = True
        system.sleep(min(.5, max(0, deadline - system.clock())))
    raise ReconnectError('connection_timeout',
        'The controller did not become ready. Check Chrome remote debugging and its Allow dialog, then rerun.', daemon_diagnostics(daemon))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['ensure', 'inspect'], nargs='?', default='ensure')
    parser.add_argument('--data', type=Path, default=Path.home() / '.local/state/larics-dj')
    parser.add_argument('--skill-root', type=Path, default=Path.home() / '.claude/skills/cdp-daemon')
    parser.add_argument('--timeout', type=float, default=180)
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error('--timeout must be a positive number of seconds')
    system = LocalSystem(args.data.resolve(), args.skill_root.expanduser())
    def progress(phase, details):
        print(json.dumps({'phase': phase, **details}, ensure_ascii=False), file=sys.stderr, flush=True)
    try:
        if args.action == 'inspect':
            result, unused = inspect(system)
            print(json.dumps(result, ensure_ascii=False, indent=2))
            return 0
        args.data.mkdir(parents=True, exist_ok=True)
        with (args.data / 'spotify-reconnect.lock').open('a') as lock:
            try: fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise ReconnectError('reconnect_in_progress', 'Another reconnect helper is already running; wait for its result.')
            result = ensure(system, args.timeout, progress)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0
    except (ReconnectError, OSError, ValueError) as error:
        result = {'ok': False, 'code': getattr(error, 'code', 'local_error'), 'error': str(error),
                  'details': getattr(error, 'details', None), 'actions': system.actions}
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
