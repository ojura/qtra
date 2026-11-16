#!/usr/bin/env python3
"""Start or inspect the local party service and its native qtra stage."""
from __future__ import annotations

import argparse
import fcntl
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
DEFAULT_DATA = Path.home() / '.local/state/larics-dj'


def health(port):
    try:
        with urllib.request.urlopen(f'http://127.0.0.1:{port}/health', timeout=1) as response:
            return json.load(response)
    except (OSError, ValueError):
        return None


def owned_server(data, expected_port=None):
    try:
        pid = int((data / 'server.pid').read_text())
        process = Path('/proc') / str(pid)
        if process.stat().st_uid != os.getuid():
            raise RuntimeError(f'PID {pid} belongs to another user; it will not be signalled.')
        command = (process / 'cmdline').read_bytes().split(b'\0')
        script = str(ROOT / 'server.py').encode()
        correct_script = script in command or (b'tools/party_dj/server.py' in command and (process / 'cwd').resolve() == REPO)
        if not correct_script:
            raise RuntimeError(f'PID {pid} belongs to another command; it will not be signalled.')
        arguments = [part.decode() for part in command if part]
        def option(name, default):
            if name in arguments:
                return arguments[arguments.index(name) + 1]
            return next((arg.split('=', 1)[1] for arg in arguments if arg.startswith(name + '=')), default)
        actual_data = Path(option('--data', str(DEFAULT_DATA))).expanduser()
        if not actual_data.is_absolute():
            actual_data = (process / 'cwd').resolve() / actual_data
        if actual_data.resolve() != data:
            raise RuntimeError(f'PID {pid} uses a different state directory; it will not be signalled.')
        actual_port = int(option('--port', '8787'))
        if expected_port is not None and actual_port != expected_port:
            raise RuntimeError(f'The recorded service uses port {actual_port}. Use start --restart to change it, or choose a separate --data directory.')
        return pid
    except (FileNotFoundError, ProcessLookupError):
        return None


def stop_server(data, port):
    pid = owned_server(data)
    if pid is None:
        return {'stopped': False, 'reason': 'The recorded service is not running.'}
    os.kill(pid, signal.SIGINT)
    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        process = Path('/proc') / str(pid)
        try:
            finished = (process / 'stat').read_text().rsplit(')', 1)[1].split()[0] == 'Z'
        except FileNotFoundError:
            finished = True
        if finished:
            (data / 'server.pid').unlink(missing_ok=True)
            return {'stopped': True, 'pid': pid}
        time.sleep(.15)
    raise RuntimeError('The service is still shutting down. Inspect server.log before retrying.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', nargs='?', choices=['start', 'status', 'stop'], default='start')
    parser.add_argument('--data', type=Path, default=DEFAULT_DATA)
    parser.add_argument('--port', type=int, default=8787)
    parser.add_argument('--request-url', help='Guest-facing URL when a specific address is required.')
    parser.add_argument('--restart', action='store_true')
    parser.add_argument('--service-only', action='store_true')
    parser.add_argument('--fullscreen', action='store_true')
    parser.add_argument('--panel', action='store_true')
    args = parser.parse_args()
    data = args.data.expanduser().resolve()
    data.mkdir(parents=True, exist_ok=True)
    with (data / 'launch.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        pid = owned_server(data, None if args.restart or args.action == 'stop' else args.port)
        if args.action == 'status':
            print(json.dumps({'pid': pid, 'service': health(args.port), 'stateFile': str(data / 'state.json')}, indent=2))
            return
        if args.action == 'stop':
            print(json.dumps(stop_server(data, args.port), indent=2))
            return
        if args.restart and pid:
            stop_server(data, args.port)
            pid = None
        reused = pid is not None
        if reused and health(args.port) is None:
            raise RuntimeError('The recorded service is not responding. Use start --restart and inspect server.log.')
        if pid is None:
            if health(args.port) is not None:
                raise RuntimeError(f'Port {args.port} already has a service without this PID record. Check it before starting another.')
            env = os.environ.copy()
            deps = REPO / 'build/party-deps'
            if deps.is_dir():
                env['PYTHONPATH'] = str(deps) + (os.pathsep + env['PYTHONPATH'] if env.get('PYTHONPATH') else '')
            with (data / 'server.log').open('a') as log:
                child = subprocess.Popen([sys.executable, '-u', str(ROOT / 'server.py'),
                    '--data', str(data), '--port', str(args.port)], cwd=REPO,
                    stdout=log, stderr=subprocess.STDOUT, env=env, start_new_session=True)
            pid = child.pid
            (data / 'server.pid').write_text(str(pid))
            try:
                deadline = time.monotonic() + 10
                while health(args.port) is None:
                    if child.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError(f'The request service did not start. Read {data / "server.log"}.')
                    time.sleep(.15)
                if child.poll() is not None:
                    raise RuntimeError(f'The new service exited. Read {data / "server.log"}.')
            except BaseException:
                if child.poll() is None:
                    child.send_signal(signal.SIGINT)
                    try:
                        child.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        child.terminate()
                        try:
                            child.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            child.kill()
                            child.wait()
                if (data / 'server.pid').read_text().strip() == str(child.pid):
                    (data / 'server.pid').unlink()
                raise
        if args.request_url:
            request = urllib.request.Request(f'http://127.0.0.1:{args.port}/api/admin/configure',
                data=json.dumps({'requestUrl': args.request_url}).encode(),
                headers={'Content-Type': 'application/json', 'Authorization': 'Bearer ' + (data / 'admin-token').read_text().strip()})
            with urllib.request.urlopen(request, timeout=10) as response:
                json.load(response)
        result = {'pid': pid, 'reused': reused, 'service': health(args.port), 'stateFile': str(data / 'state.json')}
        if not args.service_only:
            command = [sys.executable, str(REPO / 'tools/launch_party_stage.py'),
                '--state-file', str(data / 'state.json'), '--runtime-dir', str(data),
                '--base-url', f'http://127.0.0.1:{args.port}']
            if args.fullscreen:
                command.append('--fullscreen')
            if args.panel:
                command.append('--panel')
            process = subprocess.run(command, cwd=REPO, capture_output=True, text=True)
            if process.returncode:
                raise RuntimeError('The request service is running, but the native stage needs attention: ' + process.stderr.strip())
            result['stage'] = json.loads(process.stdout)
        state = json.loads((data / 'state.json').read_text())
        result['requestUrl'] = state['requestUrl']
        print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
