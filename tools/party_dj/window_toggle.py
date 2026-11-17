#!/usr/bin/env python3
"""Switch qtra and native Spotify windows without touching music playback."""
from __future__ import annotations

import argparse
import fcntl
import json
import os
from pathlib import Path
import select
import signal
import socket
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'tools'))
sys.path.insert(0, str(REPO / 'build/party-deps'))
from agentctl import AgentClient


class Desktop:
    def __init__(self, agent_socket):
        from Xlib import X, Xatom, XK, display, error, protocol
        self.X, self.Xatom, self.error, self.protocol = X, Xatom, error, protocol
        self.display = display.Display()
        self.root = self.display.screen().root
        self.agent_socket = str(agent_socket)
        self.bound = {}
        self.fullscreen_bound = {}
        self.last_error = ''
        self.tab = self.display.keysym_to_keycode(XK.string_to_keysym('Tab'))
        self.f11 = self.display.keysym_to_keycode(XK.string_to_keysym('F11'))
        numlock = self.display.keysym_to_keycode(XK.string_to_keysym('Num_Lock'))
        num_mask = next((1 << index for index, keys in enumerate(self.display.get_modifier_mapping()) if numlock in keys), 0)
        self.modifiers = sorted({0, X.LockMask, num_mask, X.LockMask | num_mask})

    def atom(self, name):
        return self.display.intern_atom(name)

    def property(self, window, name):
        value = window.get_full_property(self.atom(name), self.X.AnyPropertyType)
        return [] if value is None else list(value.value)

    def spotify_windows(self):
        identifiers = self.property(self.root, '_NET_CLIENT_LIST')
        if not identifiers:
            identifiers = [window.id for window in self.root.query_tree().children]
        result = {}
        for identifier in identifiers:
            window = self.display.create_resource_object('window', int(identifier))
            try:
                if 'spotify' not in tuple(value.casefold() for value in window.get_wm_class() or ()):
                    continue
                pid = self.property(window, '_NET_WM_PID')
                if not pid or (Path('/proc') / str(pid[0])).stat().st_uid != os.getuid():
                    continue
                if not self.property(window, 'WM_STATE'):
                    continue
                result[window.id] = window
            except (self.error.XError, OSError):
                continue
        return result

    def refresh(self):
        try:
            with AgentClient(self.agent_socket, timeout=.5) as client:
                hello = client.request('hello', {})
                ready = hello.get('name') == 'qt-runtime-cube' and hello.get('uid') == os.getuid()
                ready = ready and client.request('object.get', {'objectName': 'partyReturnAction', 'property': 'enabled'})
        except (OSError, RuntimeError):
            ready = False
        windows = self.spotify_windows() if ready else {}
        binding_errors, installed = [], False
        # Each shortcut owns its own grabs: an F11 conflict must leave the
        # already-working Tab return path available.
        for key, label, bindings in self.shortcuts():
            for identifier, window in list(bindings.items()):
                if identifier not in windows:
                    for modifier in self.modifiers:
                        window.ungrab_key(key, modifier, onerror=lambda *_: None)
                    del bindings[identifier]
            for identifier, window in windows.items():
                if identifier in bindings:
                    continue
                errors = []
                for modifier in self.modifiers:
                    window.grab_key(key, modifier, False, self.X.GrabModeAsync, self.X.GrabModeAsync,
                                    onerror=lambda problem, *_: errors.append(problem))
                self.display.sync()
                if errors:
                    for modifier in self.modifiers:
                        window.ungrab_key(key, modifier, onerror=lambda *_: None)
                    binding_errors.append(f'{label} is already bound in the Spotify window.')
                else:
                    bindings[identifier] = window
                    installed = True
        if binding_errors:
            self.last_error = ' '.join(binding_errors)
        elif installed:
            self.last_error = ''
        self.display.flush()
        return windows

    def shortcuts(self):
        return ((self.tab, 'Tab', self.bound), (self.f11, 'F11', self.fullscreen_bound))

    def native(self, method, name='mainWindow'):
        with AgentClient(self.agent_socket, timeout=4) as client:
            return client.request('object.invoke', {'objectName': name, 'method': method})

    def native_property(self, name):
        with AgentClient(self.agent_socket, timeout=4) as client:
            return client.request('object.get', {'objectName': 'mainWindow', 'property': name})

    def message(self, window, name, values):
        event = self.protocol.event.ClientMessage(window=window, client_type=self.atom(name), data=(32, values))
        self.root.send_event(event, event_mask=self.X.SubstructureRedirectMask | self.X.SubstructureNotifyMask)
        self.display.flush()

    def to_spotify(self):
        windows = self.refresh()
        if not windows:
            raise RuntimeError('Spotify nije otvoren.')
        active = self.property(self.root, '_NET_ACTIVE_WINDOW')
        window = windows.get(active[0]) if active else None
        if window is None:
            window = max(windows.values(), key=lambda item: item.get_geometry().width * item.get_geometry().height)
        if window.id not in self.bound:
            raise RuntimeError(self.last_error or 'Spotify Tab binding is unavailable.')
        fullscreen = self.atom('_NET_WM_STATE_FULLSCREEN')
        self.message(window, '_NET_WM_STATE', [1, fullscreen, 0, 2, 0])
        deadline = time.monotonic() + 2
        while fullscreen not in self.property(window, '_NET_WM_STATE'):
            if time.monotonic() >= deadline:
                raise RuntimeError('Spotify nije prešao preko cijelog zaslona.')
            time.sleep(.03)
        self.native('hide')
        try:
            window.map()
            window.configure(stack_mode=self.X.Above)
            self.message(window, '_NET_ACTIVE_WINDOW', [2, self.X.CurrentTime, 0, 0, 0])
            window.set_input_focus(self.X.RevertToParent, self.X.CurrentTime)
            self.display.sync()
            deadline = time.monotonic() + 2
            while not self.focus_is(window):
                if time.monotonic() >= deadline:
                    raise RuntimeError('Spotify nije preuzeo tipkovnicu.')
                time.sleep(.03)
            return {'shown': 'spotify', 'window': window.id, 'fullscreen': True}
        except BaseException:
            self.to_qtra()
            raise

    def focus_is(self, window):
        focused = self.display.get_input_focus().focus
        for unused in range(12):
            if not hasattr(focused, 'id'):
                return False
            if focused.id == window.id:
                return True
            if focused.id == self.root.id:
                return False
            focused = focused.query_tree().parent
        return False

    def to_qtra(self):
        self.display.ungrab_keyboard(self.X.CurrentTime)
        self.display.sync()
        self.native('trigger', 'partyReturnAction')
        deadline = time.monotonic() + 2
        while not self.qtra_has_focus():
            if time.monotonic() >= deadline:
                raise RuntimeError('Qtra je vidljiva, ali nije preuzela tipkovnicu.')
            time.sleep(.03)
        return {'shown': 'qtra', 'fullscreen': bool(self.native_property('fullScreen'))}

    def qtra_has_focus(self):
        with AgentClient(self.agent_socket, timeout=4) as client:
            if client.request('object.get', {'objectName': 'mainWindow', 'property': 'isActiveWindow'}):
                return True
            pid = client.request('hello', {})['pid']
        # X11 focus is authoritative even while Qt's activation notification is
        # still queued. A native Wayland window has no matching X11 ancestor.
        focused = self.display.get_input_focus().focus
        for unused in range(12):
            if not hasattr(focused, 'id') or focused.id == self.root.id:
                return False
            if self.property(focused, '_NET_WM_PID') == [pid]:
                return True
            focused = focused.query_tree().parent
        return False

    def key(self, event):
        if event.type == self.X.KeyPress and event.detail == self.tab and event.window.id in self.bound:
            self.to_qtra()
            self.last_error = ''
        elif event.type == self.X.KeyPress and event.detail == self.f11 and event.window.id in self.fullscreen_bound:
            window = self.fullscreen_bound[event.window.id]
            if self.focus_is(window):
                self.toggle_fullscreen(window)
                self.last_error = ''

    def toggle_fullscreen(self, window):
        fullscreen = self.atom('_NET_WM_STATE_FULLSCREEN')
        before = fullscreen in self.property(window, '_NET_WM_STATE')
        self.message(window, '_NET_WM_STATE', [2, fullscreen, 0, 2, 0])
        deadline = time.monotonic() + 2
        while (fullscreen in self.property(window, '_NET_WM_STATE')) == before:
            if time.monotonic() >= deadline:
                raise RuntimeError('Spotify nije promijenio prikaz preko cijelog zaslona.')
            time.sleep(.03)
        return {'shown': 'spotify', 'window': window.id, 'fullscreen': not before}

    def close(self):
        for key, unused_label, bindings in self.shortcuts():
            for window in bindings.values():
                for modifier in self.modifiers:
                    window.ungrab_key(key, modifier, onerror=lambda *_: None)
        self.display.sync()
        self.display.close()


def request(data, action):
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(8)
        client.connect(str(data / 'window-toggle.sock'))
        client.sendall((json.dumps({'action': action}) + '\n').encode())
        response = b''
        while b'\n' not in response:
            chunk = client.recv(8192)
            if not chunk:
                raise RuntimeError('Window switcher closed its connection.')
            response += chunk
        result = json.loads(response)
        if not result.get('ok'):
            raise RuntimeError(result.get('error', 'Window switch failed.'))
        return result


def start(data, agent_socket):
    def checked(result):
        if result.get('agentSocket') != str(agent_socket):
            raise RuntimeError('This window switcher belongs to a different qtra socket.')
        return result
    try:
        previous = request(data, 'status')
    except (OSError, ValueError, RuntimeError):
        previous = None
    if previous is not None:
        return checked(previous)
    data.mkdir(parents=True, exist_ok=True)
    with (data / 'window-toggle.log').open('ab') as log:
        child = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), 'serve', '--data', str(data),
                                  '--socket', str(agent_socket)], stdout=log, stderr=log, start_new_session=True)
    deadline = time.monotonic() + 5
    while True:
        try:
            result = request(data, 'status')
        except (OSError, ValueError, RuntimeError):
            if child.poll() not in (None, 0) or time.monotonic() >= deadline:
                if child.poll() is None:
                    child.terminate()
                    try:
                        child.wait(2)
                    except subprocess.TimeoutExpired:
                        child.kill(); child.wait()
                raise RuntimeError(f'Window switcher did not start; inspect {data / "window-toggle.log"}.')
            time.sleep(.05)
        else:
            return checked(result)


def serve(data, agent_socket):
    data.mkdir(parents=True, exist_ok=True)
    with (data / 'window-toggle.lock').open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return
        desktop = Desktop(agent_socket)
        control_path = data / 'window-toggle.sock'
        control_path.unlink(missing_ok=True)
        running = True
        def stop(*unused):
            nonlocal running
            running = False
        signal.signal(signal.SIGTERM, stop)
        signal.signal(signal.SIGINT, stop)
        try:
            with socket.socket(socket.AF_UNIX) as control:
                control.bind(str(control_path)); control_path.chmod(0o600); control.listen(4)
                (data / 'window-toggle.pid').write_text(str(os.getpid()) + '\n')
                next_refresh = 0
                while running:
                    if time.monotonic() >= next_refresh:
                        desktop.refresh(); next_refresh = time.monotonic() + 1
                    while desktop.display.pending_events():
                        try:
                            desktop.key(desktop.display.next_event())
                        except (OSError, RuntimeError, desktop.error.XError) as error:
                            desktop.last_error = str(error)
                    ready, _, _ = select.select([control, desktop.display.fileno()], [], [], .25)
                    if control not in ready:
                        continue
                    connection, _ = control.accept()
                    with connection:
                        connection.settimeout(2)
                        try:
                            payload = b''
                            while b'\n' not in payload and len(payload) < 8192:
                                chunk = connection.recv(8192 - len(payload))
                                if not chunk:
                                    raise ValueError('Incomplete window action.')
                                payload += chunk
                            body = json.loads(payload)
                            action = body.get('action')
                            if action == 'spotify':
                                result = desktop.to_spotify()
                                desktop.last_error = ''
                            elif action == 'qtra':
                                result = desktop.to_qtra()
                                desktop.last_error = ''
                            elif action in ('status', 'stop'):
                                result = {'pid': os.getpid(), 'agentSocket': str(agent_socket),
                                          'windows': sorted(desktop.bound), 'fullscreenWindows': sorted(desktop.fullscreen_bound),
                                          'error': desktop.last_error}
                                if action == 'stop':
                                    running = False
                            else:
                                raise ValueError('Unknown window action.')
                            connection.sendall((json.dumps({'ok': True, **result}) + '\n').encode())
                        except (OSError, ValueError, RuntimeError, desktop.error.XError) as error:
                            desktop.last_error = str(error)
                            try:
                                connection.sendall((json.dumps({'ok': False, 'error': str(error)}) + '\n').encode())
                            except OSError:
                                pass
        finally:
            try:
                desktop.close()
            finally:
                control_path.unlink(missing_ok=True)
                (data / 'window-toggle.pid').unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('start', 'serve', 'status', 'stop', 'spotify', 'qtra'))
    parser.add_argument('--data', type=Path, default=Path.home() / '.local/state/larics-dj')
    parser.add_argument('--socket', type=Path, default=Path(os.environ.get('XDG_RUNTIME_DIR', f'/run/user/{os.getuid()}')) / 'larics-dj-qtra.sock')
    args = parser.parse_args()
    data = args.data.expanduser().resolve()
    if args.action == 'serve':
        serve(data, args.socket)
        return
    if args.action in ('start', 'spotify', 'qtra'):
        result = start(data, args.socket)
        if args.action != 'start':
            result = request(data, args.action)
    else:
        result = request(data, args.action)
    print(json.dumps(result, ensure_ascii=False))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, ImportError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
