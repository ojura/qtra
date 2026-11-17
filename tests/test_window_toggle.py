"""Real Tab grabs and native draft restoration on an isolated X display."""
from __future__ import annotations

import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import Mock, patch

import test_party_stage as stage_test

sys.path.insert(0, str(stage_test.ROOT / 'build/party-deps'))
from Xlib import X, Xatom, XK, display, error, protocol
from party_dj import window_toggle


class WindowManager:
    """Honor the two EWMH messages used by the real helper on private Xvfb."""
    def __init__(self, name):
        self.display = display.Display(name)
        self.root = self.display.screen().root
        self.stopped = threading.Event()
        self.root.change_attributes(event_mask=X.SubstructureNotifyMask | X.SubstructureRedirectMask)
        self.support = self.root.create_window(0, 0, 1, 1, 0, X.CopyFromParent)
        self.support.set_wm_name('Private qtra window manager')
        self.support.change_property(self.display.intern_atom('_NET_WM_NAME'), self.display.intern_atom('UTF8_STRING'),
                                     8, b'Private qtra window manager')
        check = self.display.intern_atom('_NET_SUPPORTING_WM_CHECK')
        self.root.change_property(check, Xatom.WINDOW, 32, [self.support.id])
        self.support.change_property(check, Xatom.WINDOW, 32, [self.support.id])
        self.root.change_property(self.display.intern_atom('_NET_SUPPORTED'), Xatom.ATOM, 32,
                                  [self.display.intern_atom(name) for name in ('_NET_WM_STATE', '_NET_WM_STATE_FULLSCREEN', '_NET_ACTIVE_WINDOW')])
        self.display.sync()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        while not self.stopped.is_set():
            if not self.display.pending_events():
                select.select([self.display.fileno()], [], [], .05)
                continue
            event = self.display.next_event()
            if event.type == X.MapRequest:
                event.window.map(); self.display.flush()
                continue
            if event.type == X.ConfigureRequest:
                fields = ((X.CWX, 'x'), (X.CWY, 'y'), (X.CWWidth, 'width'), (X.CWHeight, 'height'),
                          (X.CWBorderWidth, 'border_width'), (X.CWSibling, 'sibling'), (X.CWStackMode, 'stack_mode'))
                event.window.configure(**{name: getattr(event, name) for mask, name in fields if event.value_mask & mask})
                self.display.flush()
                continue
            if event.type != X.ClientMessage:
                continue
            try:
                name = self.display.get_atom_name(event.client_type)
                if name == '_NET_WM_STATE':
                    action, first, second, *_ = event.data[1]
                    prop = event.window.get_full_property(event.client_type, Xatom.ATOM)
                    states = set(prop.value if prop is not None else [])
                    for atom in (first, second):
                        if not atom:
                            continue
                        if action == 1 or (action == 2 and atom not in states):
                            states.add(atom)
                        else:
                            states.discard(atom)
                    event.window.change_property(event.client_type, Xatom.ATOM, 32, sorted(states))
                elif name == '_NET_ACTIVE_WINDOW':
                    event.window.map()
                    event.window.configure(stack_mode=X.Above)
                    event.window.set_input_focus(X.RevertToParent, X.CurrentTime)
                    take_focus = self.display.intern_atom('WM_TAKE_FOCUS')
                    if take_focus in event.window.get_wm_protocols():
                        event.window.send_event(protocol.event.ClientMessage(window=event.window,
                            client_type=self.display.intern_atom('WM_PROTOCOLS'),
                            data=(32, [take_focus, event.data[1][1], 0, 0, 0])))
                    self.root.change_property(event.client_type, Xatom.WINDOW, 32, [event.window.id])
                self.display.sync()
            except error.XError:
                pass

    def close(self):
        self.stopped.set(); self.thread.join(2)
        self.display.close()


@unittest.skipUnless(stage_test.can_draw(), 'a private display is required')
class WindowToggle(unittest.TestCase):
    def setUp(self):
        self.native = stage_test.PartyStage()
        self.addCleanup(self.native.doCleanups)
        class PreparedDisplay(stage_test.OwnDisplay):
            def start(self):
                return self.number or super().start()
        prepared = PreparedDisplay()
        self.assertIsNotNone(prepared.start())
        self.native.addCleanup(prepared.stop)
        self.manager = WindowManager(prepared.number)
        self.addCleanup(self.manager.close)
        with patch.object(stage_test, 'OwnDisplay', return_value=prepared):
            self.native.setUp()
        self.x = display.Display(self.native.environment['DISPLAY'])
        self.addCleanup(self.x.close)
        self.root = self.x.screen().root
        self.windows = []
        self.spotify = self.window('Spotify')
        self.other = self.window('Unrelated test editor')
        self.data = self.native.folder / 'window-toggle'
        self.data.mkdir()
        self.socket = self.native.folder / 'agent.sock'
        self.log = (self.data / 'test.log').open('w+')
        self.addCleanup(self.log.close)
        script = stage_test.ROOT / 'tools/party_dj/window_toggle.py'
        self.process = subprocess.Popen([sys.executable, str(script), 'serve', '--data', str(self.data), '--socket', str(self.socket)],
                                        env=self.native.environment, stdout=self.log, stderr=self.log)
        self.addCleanup(self.stop)
        config = {'program': sys.executable, 'script': str(script), 'socket': str(self.socket), 'data': str(self.data)}
        self.native.run_stage(stateFile=str(self.native.state_file), windowToggle=config, fullscreen=True)
        self.eventually(lambda: self.status().get('windows') == [self.spotify.id], 'the Spotify-only grab did not appear')

    def stop(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(3)
            except subprocess.TimeoutExpired:
                self.process.kill(); self.process.wait()

    def window(self, title):
        window = self.root.create_window(10, 10, 700, 500, 0, X.CopyFromParent,
                                         event_mask=X.KeyPressMask | X.KeyReleaseMask)
        window.set_wm_name(title)
        window.set_wm_class(title.lower(), title)
        window.change_property(self.x.intern_atom('_NET_WM_PID'), Xatom.CARDINAL, 32, [os.getpid()])
        window.change_property(self.x.intern_atom('WM_STATE'), self.x.intern_atom('WM_STATE'), 32, [1, 0])
        window.map(); self.windows.append(window)
        self.root.change_property(self.x.intern_atom('_NET_CLIENT_LIST'), Xatom.WINDOW, 32, [item.id for item in self.windows])
        self.x.sync()
        return window

    def status(self):
        try:
            return window_toggle.request(self.data, 'status')
        except (OSError, ValueError, RuntimeError):
            return {}

    def eventually(self, check, message, seconds=6):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                self.log.seek(0)
                self.fail('helper exited: ' + self.log.read())
            if check():
                return
            time.sleep(.04)
        self.fail(message + ': ' + str(self.status()))

    def native_property(self, name, object_name='mainWindow'):
        return self.native.client.request('object.get', {'objectName': object_name, 'property': name})

    def native_focused(self):
        focused = self.x.get_input_focus().focus
        for unused in range(12):
            if not hasattr(focused, 'id') or focused.id == self.root.id:
                return False
            prop = focused.get_full_property(self.x.intern_atom('_NET_WM_PID'), Xatom.CARDINAL)
            if prop is not None and list(prop.value) == [self.native.process.pid]:
                return True
            focused = focused.query_tree().parent
        return False

    def key(self, window, key):
        subprocess.run(['xdotool', 'windowfocus', str(window.id), 'key', '--clearmodifiers', key],
                       env=self.native.environment, check=True)

    def received_tab(self, window):
        received = False
        while self.x.pending_events():
            event = self.x.next_event()
            received |= event.type == X.KeyPress and event.window.id == window.id and event.detail == self.x.keysym_to_keycode(XK.string_to_keysym('Tab'))
        return received

    def test_tab_round_trip_scope_relaunch_and_release(self):
        draft = {'text': 'Ne šalji ovu poruku.\nSamo promijeni prikaz.', 'cursor': 12, 'anchor': 5}
        self.native.run_stage(composerDraft=draft)
        saved = self.native.run_stage(getComposerDraft=True)['composerDraft']
        self.native.key('Tab')
        self.eventually(lambda: self.native_property('visible') is False, 'native Tab did not switch away')
        self.assertIn(self.x.intern_atom('_NET_WM_STATE_FULLSCREEN'),
                      self.spotify.get_full_property(self.x.intern_atom('_NET_WM_STATE'), Xatom.ATOM).value)
        self.key(self.spotify, 'Tab')
        self.eventually(lambda: self.native_property('visible') and self.native_focused(), 'Spotify Tab did not return focus to qtra')
        self.assertTrue(self.native_property('fullScreen'))
        self.assertEqual(self.native.run_stage(getComposerDraft=True)['composerDraft'], saved)
        subprocess.run(['xdotool', 'type', '--clearmodifiers', '--delay', '0', 'X'], env=self.native.environment, check=True)
        self.eventually(lambda: self.native.composer_text() == saved['text'][:5] + 'X' + saved['text'][12:],
                        'real typing did not reach the restored composer')
        self.native.run_stage(composerDraft=saved)
        self.assertFalse(self.native.server.posts)
        self.assertFalse(self.native.server.requests)

        self.key(self.other, 'Tab')
        self.eventually(lambda: self.received_tab(self.other), 'an unrelated application lost Tab')
        self.assertEqual(self.x.get_input_focus().focus.id, self.other.id)
        self.key(self.spotify, 'shift+Tab')
        self.eventually(lambda: self.received_tab(self.spotify), 'Shift+Tab was captured in Spotify')
        self.assertEqual(self.x.get_input_focus().focus.id, self.spotify.id)

        missing = self.socket.with_suffix('.away')
        self.socket.rename(missing)
        try:
            self.eventually(lambda: self.status().get('windows') == [], 'an unavailable qtra owner retained Tab')
            self.key(self.spotify, 'Tab')
            self.eventually(lambda: self.received_tab(self.spotify), 'Tab was swallowed while qtra was unavailable')
        finally:
            missing.rename(self.socket)
        self.eventually(lambda: self.status().get('windows') == [self.spotify.id], 'the qtra owner recovery did not rebind Tab')
        self.native.run_stage(restore=True)
        self.eventually(lambda: self.status().get('windows') == [], 'releasing the native return action retained Tab')
        self.native.run_stage(stateFile=str(self.native.state_file), composerDraft=saved, fullscreen=True)
        self.eventually(lambda: self.status().get('windows') == [self.spotify.id], 'reloading the native stage did not restore the binding')

        old = self.spotify.id
        self.windows.remove(self.spotify); self.spotify.destroy(); self.x.sync()
        self.root.change_property(self.x.intern_atom('_NET_CLIENT_LIST'), Xatom.WINDOW, 32, [item.id for item in self.windows]); self.x.sync()
        self.eventually(lambda: old not in self.status().get('windows', []), 'the destroyed Spotify grab remained')
        self.native.key('Tab')
        self.eventually(lambda: bool(self.native.run_stage()['windowToggleError']), 'missing Spotify did not report an error')
        self.assertTrue(self.native_property('visible'))
        self.assertEqual(self.native.run_stage(getComposerDraft=True)['composerDraft'], saved)
        self.spotify = self.window('Spotify')
        self.eventually(lambda: self.status().get('windows') == [self.spotify.id], 'a relaunched Spotify window was not discovered')
        self.native.key('Tab')
        self.eventually(lambda: self.native_property('visible') is False, 'Tab did not reach the replacement Spotify window')
        self.key(self.spotify, 'Tab')
        self.eventually(lambda: self.native_property('visible') and self.native_focused(), 'return from replacement Spotify failed')
        self.assertEqual(self.native.run_stage(getComposerDraft=True)['composerDraft'], saved)
        subprocess.run(['xdotool', 'key', '--clearmodifiers', 'Tab'], env=self.native.environment, check=True)
        self.eventually(lambda: self.native_property('visible') is False, 'Tab after returning required another focus action')
        subprocess.run(['xdotool', 'key', '--clearmodifiers', 'Tab'], env=self.native.environment, check=True)
        self.eventually(lambda: self.native_property('visible') and self.native_focused(), 'the second focus-preserving round trip failed')
        self.assertEqual(window_toggle.start(self.data, self.socket)['pid'], self.process.pid, 'start spawned a duplicate helper')
        window_toggle.request(self.data, 'stop')
        self.process.wait(3)
        self.key(self.spotify, 'Tab')
        deadline = time.monotonic() + 2
        while not self.received_tab(self.spotify):
            if time.monotonic() >= deadline:
                self.fail('stopping the helper did not release Tab')
            time.sleep(.03)
        self.assertFalse((self.data / 'window-toggle.sock').exists())


class WindowToggleOwnership(unittest.TestCase):
    def test_post_spawn_status_rejects_a_different_owner(self):
        with tempfile.TemporaryDirectory(prefix='qtra-toggle-owner-') as directory:
            data = Path(directory)
            child = Mock()
            with patch.object(window_toggle, 'request', side_effect=[ConnectionRefusedError(), {'ok': True, 'agentSocket': '/other.sock'}]), \
                 patch.object(window_toggle.subprocess, 'Popen', return_value=child):
                with self.assertRaisesRegex(RuntimeError, 'different qtra socket'):
                    window_toggle.start(data, Path('/expected.sock'))

    def test_same_owner_starter_waits_for_the_lock_winner(self):
        with tempfile.TemporaryDirectory(prefix='qtra-toggle-owner-') as directory:
            data = Path(directory)
            child = Mock()
            child.poll.return_value = 0
            ready = {'ok': True, 'agentSocket': '/expected.sock', 'pid': 123}
            with patch.object(window_toggle, 'request', side_effect=[ConnectionRefusedError(), ConnectionRefusedError(), ready]), \
                 patch.object(window_toggle.subprocess, 'Popen', return_value=child):
                self.assertEqual(window_toggle.start(data, Path('/expected.sock')), ready)


if __name__ == '__main__':
    unittest.main()
