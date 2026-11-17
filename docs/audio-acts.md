# Native audio acts

`audio_pulse` has three compositions driven by its existing system-audio
capture and 64-band FFT. Changing the style keeps that analyzer, its beat
counters and recent spectrum history running.

| Style | Composition |
| --- | --- |
| `city` | Layered orange and violet spectrum towers, luminous roof caps, windows, reflections and a receding street grid |
| `waterfall` | A rectangular stream of recent spectra, onset lines and a bright live frequency trace |
| `ring` | The original spectrum ring, cube, expanding shockwaves and particle field |

City and waterfall omit the circular geometry and hide the base cube after
each animation step. The party's robots, photographs, captions, now-playing card and Magic box
are independent overlays. For these acts the party service holds its separate
geometry effect at `0`, so the orbiting sphere and Möbius ribbon stay off.

## Control

Build only the module:

```sh
cmake --build build/release --target agent_snippet_audio_pulse --parallel 3
```

Run its snippet on the render executor with a request such as:

```json
{"style":"city","events":true,"stateFile":"/home/juraj/.local/state/larics-dj/state.json"}
```

The optional state file is read every half second. Its `scene.audioStyle` field
selects `city`, `waterfall` or `ring`. Missing style fields keep the selected
style; a malformed file or unknown style keeps the current act and reports
`styleFileError`. Pass `stateFile:""` to return to direct style control.

The default remains `ring` for existing demo callers. Native style requests
reject unknown names. `restore:true`, or the module's release entry point, removes
the visualizer and restores the previous cube-step binding.

Status includes `style`, `circularGeometry`, `cubeScale`, `vertices`, `captureHops`,
`historyRows`, `historyCapacity`, `sensitivity`, `dynamicRangeDb`, `events` and
the capture device. The waterfall retains at most 96 real spectrum frames,
sampled about every 60 ms. It uses spectrum energy rather than a generated
waveform. History is collected during every style, so a later waterfall starts
with the sound that is already playing.

## Verification

```sh
python3 -m unittest discover -s tests -p test_audio_acts.py -v
```

The test opens a private display and a separate qtra process. It reads the live
default audio monitor without changing playback streams or audio routing. It
checks rendered city and waterfall images, capture-hop continuity across style
changes, bounded history, state-file control and fallback, invalid input and
clean release. Set `AUDIO_ACT_CAPTURE_DIR` to retain its two scene captures for
visual inspection.
