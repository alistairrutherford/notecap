# LiveBridge: Live-in-the-loop tests

Drives Ableton Live 12 from tests and from Claude. It covers only what needs the real host;
offline checks stay in `Tests/run_tests.sh`, `Tests/run_integration.sh` and `pluginval`.
Taken from Chordism's LiveBridge (2026-10-21) and renamed **NoteCapBridge** on port **9872**,
so it can run alongside Chordism's bridge (9871) in the same Live session.

```
pytest / MCP server / CLI  ──TCP 127.0.0.1:9872──▶  NoteCapBridge Remote Script (inside Live)
   (livebridge/)               JSON lines            polled from Live's update tick → LOM
```

| Path | What it is |
|---|---|
| `RemoteScript/NoteCapBridge/` | The Live control surface. `server.py` is the socket (no Live imports), `commands.py` the LOM commands. |
| `install_remote_script.sh` | Symlinks it into `~/Music/Ableton/User Library/Remote Scripts/`. |
| `livebridge/client.py` | Python client, plus helpers (load and wait, record N beats, sample meters). |
| `livebridge/timing.py` | Offset of each note from the beat. |
| `livebridge/livelog.py` | Sample rate and buffer size, read from Live's `Log.txt`. |
| `livebridge/mcp_server.py` | MCP server (stdio) with the same commands as tools. Registered in the repo's `.mcp.json` as `live-bridge`. |
| `tests/test_offline.py` | Protocol and commands against a fake LOM, and the timing math. No Live needed; runs in CI. |
| `tests/rig.py` | Shared set-up for the Live tests: tracks, parameters by name, MIDI From routing, recording. |
| `tests/test_routing_live.py` | NoteCap's *Test chords* on an audio track, recorded on a MIDI track through *MIDI From*: C F G Am on every beat, each exactly on time. |
| `tests/test_detection_live.py` | Detection end to end: Chordism's test pattern → Drift → NoteCap → recorded chords, compared beat by beat with what Chordism played, with NoteCap's detection latency. |
| `tests/test_sidechain_live.py` | The same chords heard through NoteCap's **sidechain**: NoteCap on a silent audio track, its sidechain fed from the synth track (routed in the saved set with `livebridge/als.py`, since the LOM can't). |
| `livebridge/als.py` | Edits a saved set where the LOM can't reach: routes a plug-in's sidechain in a copy of the `.als`. |
| `tests/test_record_quantize_live.py` | NoteCap's late chords (Grid 1/4, Nearest) recorded with Live's **Record Quantization** off (they arrive ~125 ms late) and at 1/4 (exactly on the beat). |
| `tests/test_state_live.py` | NoteCap's settings survive **duplicating the track** (automatic), and **saving and reopening the set** (two stages around one Cmd-S). |
| `scripts/restart_live.sh` | Quits Live and reopens it with a set, then waits for the bridge. |

## Setup

Needs [uv](https://docs.astral.sh/uv/). Live 12 runs Remote Scripts on its own Python 3.11.

```sh
cd LiveBridge
uv sync                      # .venv with mcp and pytest
./install_remote_script.sh   # symlink (use --copy if Live ignores the link)
```

Then restart Live and, in **Settings → Link, Tempo & MIDI → Control Surface**, pick
**NoteCapBridge** in an empty row. Leave its Input and Output on *None*. Live shows
*"NoteCapBridge listening on port 9872"* in the status bar, and the script logs to
`~/Library/Preferences/Ableton/Live 12.x.x/Log.txt` (search for `NoteCapBridge:`).

**Plug-in parameters.** The bridge reaches a plug-in only through the parameters on Live's
device panel, which Live fills by itself only for plug-ins with at most 64 parameters. NoteCap
has 18, so it needs nothing. The detection test also drives **Chordism**, which has more than
64: it needs `-_PluginAutoPopulateThreshold=128` in
`~/Library/Preferences/Ableton/Live 12.x.x/Options.txt` (see Chordism's LiveBridge README).

Check it:

```sh
uv run python -m livebridge ping
uv run python -m livebridge get_state
```

## Running the tests

```sh
uv run pytest tests/test_offline.py            # no Live needed
uv run pytest tests/test_routing_live.py       # Test chords through MIDI From, timing
uv run pytest tests/test_detection_live.py     # Chordism -> Drift -> NoteCap -> recorded chords
uv run pytest tests/test_sidechain_live.py     # the same, heard through NoteCap's sidechain
uv run pytest tests/test_record_quantize_live.py   # late chords snapped by Record Quantization
uv run pytest tests/test_state_live.py         # settings survive duplicate_track
# Save and reopen (the LOM can't save a set): prepare, Cmd-S, reopen, verify
NOTECAP_STATE=prepare uv run pytest tests/test_state_live.py -k reopen
scripts/restart_live.sh "path/to/the set.als"
NOTECAP_STATE=verify uv run pytest tests/test_state_live.py -k reopen
```

The live tests build their tracks (`NoteCap Src`, `NoteCap Rec`, `NoteCap Chords`,
`NoteCap Synth`, `NoteCap Detect`) the first time and reuse them afterwards, so run them in a
scratch set. They skip when the bridge doesn't answer. The report is printed at the end of the
run (with or without `-s`). Each run is also saved as JSON in `reports/` (gitignored), with the
whole clip. Settings are environment variables, listed at the top of each test file.

## The whole Phase 0 run

One Cmd-S is the only manual step (the LOM has no save):

```sh
uv run pytest tests/test_routing_live.py tests/test_detection_live.py \
              tests/test_record_quantize_live.py tests/test_state_live.py -k "not reopen"
NOTECAP_STATE=prepare uv run pytest tests/test_state_live.py -k reopen
uv run pytest tests/test_sidechain_live.py --setup-only      # builds the sidechain tracks
# in Live: File > Save Live Set As... (it goes in a "<name> Project" folder)
uv run python -m livebridge.als "path/to/set.als" --track "NoteCap Side" --source "NoteCap Synth"
scripts/restart_live.sh "path/to/set sidechain.als"
NOTECAP_STATE=verify uv run pytest tests/test_state_live.py -k reopen
uv run pytest tests/test_sidechain_live.py
```

`restart_live.sh` checks the set exists before it quits Live.

## Restarting Live (new plug-in builds)

Live keeps a plug-in binary loaded until it quits, and loads Remote Scripts only at startup.
After rebuilding NoteCap or editing the Remote Script:

```sh
LiveBridge/scripts/restart_live.sh "path/to/test set.als"
```

## Manual steps (not in the Live Object Model)

- **Saving a set.** The LOM has no save command (Live's `_MxDCore/LomTypes` lists none), so the
  reopen test needs one Cmd-S between its two stages. `duplicate_track` gives a fully automatic
  state round trip through Live in the meantime.
- **A device's Sidechain chooser.** The LOM doesn't reach it: `Device.audio_inputs` (Live's
  `DeviceIO`) is empty for plug-ins, and for Live's own Compressor, in 12.4.6. It seems to cover
  only Max for Live devices' extra buses. Live stores the choice in the set, though, so
  `python -m livebridge.als` routes it in a copy of a saved set (`SideChain` → `Target
  AudioIn/Track.<id>/PostFxOut`), and the test opens that copy.
- **Audio buffer size and sample rate** (Settings → Audio). The reports read the current values
  from Live's log.
- **Plug-in rescans**, and the **count-in** preference (count-in doesn't change the results: the
  recorder waits for song time).
- **Plug-in windows.** The LOM can't open a plug-in editor or click in it.

## Socket protocol

TCP on `127.0.0.1:9872`, localhost only. One JSON object per line, UTF-8, `\n` terminated, in
each direction. Several clients may connect. Requests on one connection are answered in order.

```json
{"id": 7, "cmd": "set_tempo", "args": {"bpm": 120}}
{"id": 7, "ok": true, "result": {"tempo": 120.0}}
{"id": 8, "ok": false, "error": "CommandError: no track named 'Foo'; have ['1-MIDI', '2-MIDI']", "traceback": "..."}
```

`id` is any JSON value and is echoed back. `args` is optional. Errors never close the
connection; a line that isn't JSON gets an error reply with `id: null`.

**Threading.** The listener and client sockets are non-blocking and are serviced only in
`update_display`, Live's main-thread tick (about every 100 ms). Each tick accepts, reads, runs
up to 64 commands and writes. So every LOM call runs on the main thread, and one call takes up
to ~100 ms. Nothing blocks in Live. Commands that Live finishes later (loading a device) return
straight away, and the client polls (`load_device_and_wait`).

**Addressing.** `track`: 0-based index, a name, or `"master"`. `device` and `parameter`: 0-based
index or a name. Names match exactly (ignoring case) first, then by a unique substring.

### Commands

| Command | Args | Result |
|---|---|---|
| `ping` | | `{pong, live_version}` |
| `list_commands` | | command names |
| `get_state` | `include_devices=true` | tempo, signature, is_playing, current_song_time, record_mode, session_record, is_counting_in, arrangement_overdub, loop, metronome, midi_recording_quantization, `tracks[]` (index, name, is_midi, arm, monitoring, mute, solo, input/output routing, devices), master meter, selected_track |
| `set_tempo` | `bpm` | `{tempo}` |
| `start_transport` | `from_beat?`, `resume=false` | `{is_playing, current_song_time}` |
| `stop_transport` | | same |
| `set_song_properties` | any of `current_song_time, loop, loop_start, loop_length, metronome, arrangement_overdub, record_mode, session_record, signature_numerator, signature_denominator, back_to_arranger, midi_recording_quantization` (`"none"` or a `RecordingQuantization` name) | the new values |
| `create_midi_track` | `index=-1`, `name?` | the track |
| `create_audio_track` | `index=-1`, `name?` | the track (added for NoteCap, an audio effect) |
| `duplicate_track` | `track` | the copy (Live saves each plug-in's state and loads it into the copy) |
| `delete_track` | `track` | `{deleted, track_count}` |
| `get_track` | `track` | the track |
| `set_track` | `track`, `name? arm? monitoring? ("in"/"auto"/"off") mute? solo?` | the track |
| `arm_track` | `track`, `arm=true` | `{track, arm}` |
| `browser_search` | `query`, `category="plugins"`, `format?`, `limit=50` | `[{name, path, uri, format, exact, is_device}]` |
| `load_device` | `name`, `track`, `category="plugins"`, `format?` | `{track, device_count_before, loaded, other_matches}`. The device appears a moment later. |
| `load_plugin` | `name`, `track`, `format?` | same as `load_device` with category `plugins` |
| `list_devices` | `track` | `[{index, name, class_name, class_display_name, type, is_active, parameter_count}]` |
| `list_parameters` | `track`, `device` | `{device, parameters: [{index, name, value, min, max, is_quantized, is_enabled, display, value_items?}]}` |
| `get_parameter` | `track`, `device`, `parameter` | the parameter |
| `set_parameter` | `track`, `device`, `parameter`, `value` (number, `true`/`false` → max/min, or a value item) | the parameter |
| `get_routing` | `track`, `direction="input"` | `{current: {type, channel}, available_types: [{display_name, category, track_index}], available_channels: [{display_name}]}` |
| `set_routing` | `track`, `direction`, `type?` or `source_track?`, `channel?` | as `get_routing` |
| `get_input_routing` / `set_input_routing` | as above, input only | |
| `start_recording` | `mode="arrangement"`, `from_beat?`; or `mode="session"` with `track?`, `scene?`, `length_beats?` | the record state |
| `stop_recording` | `mode`, `stop_transport=true` | the record and transport state |
| `clear_arrangement_clips` | `track` | `{deleted}` |
| `clear_session_clips` | `track` | `{deleted}` |
| `get_clip_notes` | `track`, `source="auto"` (`arrangement`/`session`), `clip_index?`, `scene?` | clip info (start_time, markers, loop, length) and `notes[]` (pitch, start_time, duration, velocity, mute, probability, and `song_time` for arrangement clips) |
| `get_meters` | `tracks?` (default all) | `[{track, name, left, right, level}]`, master last |
| `get_presets` | `track`, `device` | `{device, presets[], selected}`: Live's preset list for a plug-in (for a JUCE VST3, only "Default") |
| `set_preset` | `track`, `device`, `preset` (index or name) | as `get_presets`, after selecting it |
| `load_clip_file` | `name`, `category="user_library"` | loads a MIDI file from the browser as a double-click would: Live imports it as a session clip on a new MIDI track; returns `track_count_before` and the browser item |
| `get_cpu` | | `{average, peak}`: Live's CPU meter (`Application.average_process_usage` / `peak_process_usage`, in percent: about 0.6 for an empty set in 12.4.6), `null` where Live lacks them |
| `delete_device` | `track`, `device` | `{track, deleted, devices}` |

**Plug-in lookup.** `load_device` walks `application().browser.<category>` (for plugins, the
Plug-Ins tree). It loads onto the track by selecting it and calling `browser.load_item`.
An exact name wins over a substring match. If a plug-in is listed in more than one format, the
order is VST3, then AU, then VST, unless `format` says otherwise. The format comes from the
browser folder above the item ("VST3", "Audio Units", "VST"), or else from its URI.

**MIDI From another track's plug-in.** Set the routing type with `source_track` (matched by the
routing type's `attached_object`, so track names don't matter). Then set `channel` to the plug-in's
name, which Live lists next to "Pre FX" and "Post FX".

**"Most recent clip".** In the arrangement, the MIDI clip that starts latest. In Session View,
the slot last recorded through this bridge, or else the recording or playing slot, or else the
last filled one. Note times are in clip beats. `song_time = clip.start_time + start_time - start_marker`.

## MCP server

`.mcp.json` in the repo root registers it:

```json
{"mcpServers": {"live-bridge": {"command": "uv",
  "args": ["run", "--directory", "LiveBridge", "python", "-m", "livebridge.mcp_server"]}}}
```

Its tools are the commands above, plus `load_device` (which waits for the device),
`measure_note_timing` (offsets of the latest clip on a track, in beats, ms and samples),
`get_audio_settings` (sample rate, buffer sizes and latencies from Live's log), and
`bridge_command` (any command by name). It works with the `mcp` SDK 1.x (`FastMCP`) and 2.x (`MCPServer`). Claude Code asks to
approve the server the first time it starts in this folder.
