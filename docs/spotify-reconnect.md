# Restore the Spotify controller

Run the recovery helper after a desktop session or browser restart:

```sh
python3 tools/party_dj/spotify_reconnect.py
```

It first checks the native Spotify debugging endpoint at port 9224. When that
endpoint is unavailable, it reuses the CDP daemon at port 7799 and the existing
Chrome profile. A missing daemon is started from the installed `cdp-daemon`
skill. If Chrome has exited, the helper opens one regular Chrome window with the
last-used existing profile and Spotify's controller page. It does not create a
new profile or restart a running browser.

The final JSON reports `ok:true` only after two fresh playback observations. A
playing track must advance or change; a paused track can keep its position. The
result names the current song, artist, URI, position and controller mode. Progress
and connection/consent diagnostics go to stderr. Exit status is 0 for a verified
connection and 1 for an actionable failure.

The helper changes no Spotify playback, queue, volume, audio route, microphone or
output device. It uses the existing Spotify bridge's `status` method and releases
only its own CDP session. The party service reconnects through the same running
daemon and controller tab.

## Inspection and configuration

```sh
python3 tools/party_dj/spotify_reconnect.py inspect
python3 tools/party_dj/spotify_reconnect.py ensure --timeout 180
python3 tools/party_dj/spotify_reconnect.py --skill-root /home/juraj/claude-skills/cdp-daemon
```

`inspect` reads native/daemon availability and process identity without launching
anything or attaching to a page. `--data` selects the party runtime directory;
the default is `~/.local/state/larics-dj`. It holds `spotify-reconnect.lock` during
recovery so a second invocation cannot start another recovery concurrently.
Started-process logs are `spotify-reconnect-daemon.log` and
`spotify-reconnect-chrome.log` in that directory.

Desktop startup uses fresh display/session fields from the systemd user manager,
with current GNOME/qtra processes as a fallback. It validates that a display is
present and discards stale inherited display fields. Chrome process identity
comes from `/proc/<pid>/exe` and process relationships: Chrome can rewrite both
`argv[0]` and `comm`, so `pgrep -x chrome` is insufficient after a session crash.

## Setup and diagnostics

The existing Chrome profile must already be signed into Spotify and have remote
debugging enabled through `chrome://inspect`. Chrome's default profile does not
use a new command-line debugging port. The installed daemon handles its normal
native Allow/Restore prompts and publishes `consent_diagnostics`; the helper
reports the current state and relevant recorded events. It requests one explicit
reconnect for a parked connection, then waits for that attempt rather than opening
repeated consent dialogs. The default 180-second budget accommodates the daemon's
150-second consent acquisition. An unfinished attempt is left running and can be
inspected or completed through the existing Chrome dialog.

Managed page preparation uses `/prepare` with DOM readiness and preserves the
selected browser tab. The approved `browser-state-extension` from the installed
skill must be enabled. If it is absent or invalidated, the helper reports the
complete structured preparation error, including its kind and stage. Follow the
skill's `page-preparation.md` installation/reload method, then rerun. The helper
does not silently install an extension or change its permissions.

Other failures distinguish a missing existing profile, unavailable desktop,
unresponsive existing daemon, failed browser process, Spotify sign-in requirement,
missing active track, old observation and non-advancing playback. A running but
unresponsive daemon is left in place so its other browser clients are preserved.
Check the reported log or supported daemon diagnostic rather than starting
another browser or daemon beside it.

## Verification

```sh
PYTHONPATH=build/party-deps python3 -m unittest discover -s tools/party_dj -p test_spotify_reconnect.py -v
```

The focused suite uses isolated process/controller fixtures. It covers native
preference, existing-browser reuse after title rewriting, missing dependencies,
consent transition handling, fresh desktop fields, profile reuse, preparation
failure cleanup and fresh playback verification. It starts no live browser,
daemon or player and sends no playback commands.
