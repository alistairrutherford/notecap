# NoteCap: Project Plan

A macOS Audio Unit plugin for Ableton Live that listens to audio (another track or a mic), works out which chord is being played, and outputs quantized MIDI chords that Live can record onto a MIDI track.

---

## 1. Goals

| # | Requirement | Acceptance criterion |
|---|-------------|----------------------|
| R1 | Loads as an AU plugin on a MIDI track in Live | Passes `auval`. Shows up in Live's Plug-ins browser. Can be inserted on a MIDI track. |
| R2 | Listens to audio from another track or a mic | The user picks the source track in Live's sidechain selector. A mic works through an audio track whose input is the mic. |
| R3 | Identifies the chord being played | Offline accuracy of at least 80% (MIREX "majmin" weighted recall) on the test set. The chord appears in the UI within about 150 ms of the strum. |
| R4 | Sends the chords as MIDI to the MIDI track so they can be recorded | Arming the MIDI track and pressing record produces a clip with the chord notes in it. |
| R5 | Quantizes chords in real time | With the grid set to 1/4, every recorded chord note-on lands exactly on a 1/4 grid line. |

**Out of scope for v1:** Windows, AUv3/iOS, polyphonic note transcription (exact voicings), key detection, and chords beyond 7ths.

---

## 2. Ableton Live constraints that shape the design

Live's plugin hosting has three limits that affect this plugin directly. The architecture below is built around them.

1. **Live does not support AU MIDI effects (`aumi`).** An AU in Live can only be an instrument (`aumu`) or an audio effect (`aufx`).
2. **Live cannot route MIDI output from AU plugins.** It offers "MIDI From → *track* → *plugin*" routing only for VST plugins. MIDI that an AU sends to the host is effectively lost.
3. **Live records the MIDI that arrives at a track's input, not the MIDI a plugin produces on that track.** Even with VST, the plugin's MIDI output has to be routed into a track's input before it can be recorded.
4. **Sidechain support for AU plugins in Live is inconsistent.** Some forum reports say sidechain works only for VST2. This must be tested in Phase 0 against the user's Live version.

### Chosen approach: virtual CoreMIDI output

The plugin creates its own **CoreMIDI virtual source** (for example, "NoteCap Out 1"). Live treats it like any hardware MIDI port. This avoids limits 1–3:

```
 ┌───────────── Audio track: "Guitar" (input = mic / interface) ─────────────┐
 │  monitoring: In/Auto                                                      │
 └───────────────────────────────┬───────────────────────────────────────────┘
                                 │ sidechain (audio)
 ┌───────────── MIDI track: "Chords"  (armed, MIDI From = "NoteCap Out 1") ──┐
 │  [Any instrument, e.g. Wavetable]  →  [NoteCap (AU audio effect)]         │
 │         ▲                                   │  audio passes through       │
 │         │  MIDI in (recorded to clip)       │                             │
 └─────────┼───────────────────────────────────┼─────────────────────────────┘
           └────────── CoreMIDI virtual port ◄─┘  quantized, time-stamped chords
```

- NoteCap is built as an **AU audio effect** with a main input bus and a **sidechain input bus**. It passes the main audio through unchanged. That lets it sit on the MIDI track after a real instrument, so the track records the chords and also plays them.
- Chords go out through the virtual port with **absolute host-time timestamps**, calculated from Live's transport. Quantized notes are scheduled in advance, so they reach Live on the grid.
- The MIDI track's input is set to the NoteCap port, so what Live records on that track is exactly what the plugin emits.

### Fallbacks (decided in Phase 0)

| If Phase 0 finds… | Fallback |
|---|---|
| AU sidechain doesn't reach the plugin in Live | **Mode B:** put NoteCap on the *source audio track* and analyse the main input (no sidechain needed). MIDI still goes to the Chords track through the virtual port. |
| Live ignores CoreMIDI timestamps, making quantization jittery | Send each event as close to its due time as possible from a high-priority MIDI thread, and also turn on Live's **Record Quantization**, which snaps recorded notes after the fact. |
| Virtual ports can't be created from inside the plugin | Ship a VST3 build as well and use Live's "MIDI From → NoteCap track → NoteCap" routing (the plugin then needs its own track). |

JUCE builds AU, VST3 and Standalone versions from one codebase, so keeping VST3 available as a fallback costs almost nothing.

---

## 3. Technology

| Area | Choice | Why |
|---|---|---|
| Framework | **JUCE 8.0.14** (C++17), **Projucer + xcodebuild** (matching Juno106AU; no cmake/brew) | Standard for AU/VST3. Includes sidechain buses, `AudioPlayHead` transport info, and a UI toolkit. |
| Formats | AUv2 (primary), VST3 and Standalone (dev/fallback) | AUv2 is what Live hosts. Standalone makes DSP debugging quicker. |
| MIDI out | CoreMIDI `MIDISourceCreateWithProtocol` / `MIDIReceived` | Virtual port with host-time timestamps. |
| DSP | Own implementation on Apple vDSP / Accelerate; the core has no JUCE dependency | Avoids GPL/AGPL licences (Chordino, Essentia). Fast and real-time safe. |
| Tests | Plain clang test scripts (`Tests/run_tests.sh`, `run_integration.sh`), a synthetic-guitar benchmark, and the `Tools/eval` CLI (majmin scoring built in) | Chord accuracy is measured, not guessed. No extra dependencies. |
| Validation | `auval`, `pluginval` (strictness 10) | Catches host-compatibility bugs early. |
| Datasets | GuitarSet (CC BY 4.0, guitar with chord labels), plus our own recordings | Guitar is the most likely input source. |

---

## 4. Architecture

```
Audio thread (processBlock)                          Worker/MIDI threads
─────────────────────────────                        ───────────────────
sidechain/main in ─► InputConditioner (mono, HPF, gate, resample 22.05k)
                        │
                        ▼
                   AnalysisRingBuffer ──────────────► ChordEngine (worker thread)
                                                        ├ STFT / log-freq spectrum
                                                        ├ TuningEstimator
                                                        ├ ChromaExtractor (+ bass chroma)
                                                        ├ ChangeDetector (flux + chroma distance)
                                                        ├ ChordClassifier (templates)
                                                        └ ChordTracker (online HMM / hysteresis)
                                                              │ ChordEvent{chord, onsetSample, confidence}
                        ┌─────────────────────────────────────┘ (lock-free FIFO)
                        ▼
                   TransportSnapshot (ppq, bpm, isPlaying, hostTime) ─┐
                        │                                             │
                        ▼                                             ▼
                   Quantizer ──► VoicingGenerator ──► MidiScheduler ──► VirtualMidiOut (MIDI thread)
                                                        (note-on/off with host timestamps)
main in ─────────────────────────────────────────────────────────────► main out (pass-through)
```

### Module list

| Module | Responsibility |
|---|---|
| `core/` (no JUCE dependency, unit-testable) | `ChromaExtractor`, `ChangeDetector`, `ChordClassifier`, `ChordTracker`, `ChordVocabulary`, `Quantizer`, `VoicingGenerator` |
| `plugin/` | `NoteCapProcessor` (bus layout: main stereo + sidechain stereo), parameters via `AudioProcessorValueTreeState`, state save/restore |
| `midi/` | `VirtualMidiOut` (CoreMIDI port lifecycle, unique naming per instance), `MidiScheduler` (lock-free queue → MIDI thread) |
| `ui/` | `NoteCapEditor`: chord display, confidence meter, chroma bars, input meter, settings |
| `tools/eval` | CLI that runs `core/` over WAV files, writes `.lab` files, and scores them with `mir_eval` |

### Real-time safety rules
- `processBlock` never allocates, never locks, and never calls CoreMIDI. It only moves data through lock-free FIFOs.
- FFT buffers and all state are allocated in `prepareToPlay`.
- MIDI goes out on a dedicated thread. Because events carry timestamps, the thread-handoff delay doesn't affect timing.
- *As built:* analysis runs **on the audio thread**, not a worker. The cost is bounded and small: one 4096-point and one 1024-point FFT plus a 73-note NNLS fit every 256 decimated samples. This keeps timing deterministic and makes offline runs bit-identical to the plugin (tested across block sizes).

---

## 5. Chord detection design

**Pipeline (v1, signal-processing based):**
1. **Condition the input:** sum to mono, apply a high-pass filter at about 60 Hz and a noise gate (user threshold), then resample to 22.05 kHz.
2. **Spectrum:** 8192-point STFT with a 1024-sample hop (about 46 ms hop), mapped to a log-frequency spectrum at 3 bins per semitone covering about A1 to C7.
3. **Tuning:** estimate the deviation from A440 (±50 cents) with slow smoothing, so detuned guitars still work.
4. **Chroma:** fold into 12 pitch classes, attenuating partials so harmonics don't produce false notes. Build a separate **bass chroma** from the low band to detect inversions. *As built:* a non-negative least-squares fit of the peak-picked semitone spectrum to 12-harmonic note profiles, independently implemented from the NNLS-chroma idea (Mauch & Dixon 2010). Chroma is limited to E2–C6, because higher partials (7th, 9th and 11th harmonics) otherwise read as extended chords.
5. **Change detection:** combine spectral flux with the cosine distance between successive chroma frames to mark candidate chord onsets.
6. **Classification:** score each frame against chord templates weighted by harmonic content. Vocabulary: maj, min, 7, maj7, m7, dim, aug, sus2, sus4, plus N (no chord). The user can restrict it.
7. **Tracking:** an online HMM with fixed-lag Viterbi (a lag of 1–2 frames) or a hysteresis rule, plus a minimum chord duration. This stops the output flickering.
8. **Onset back-dating:** each emitted event carries the time of the detected *onset*, not the detection time. The quantizer needs this.
9. *Added during implementation:* **previous-chord removal.** The note activations from just before an onset are subtracted, in proportion to the share of the window that precedes the onset. Early frames otherwise mistake the old chord's ringing notes for 7ths. There is also a **decision margin**: close calls such as Am vs Am7 wait up to 8 more frames. Later **corrections** reuse the segment ID. The processor updates the pending chord in place, or, if the chord has already been sent, sends only the notes that differ.

**Latency budget:** about 100–150 ms from strum to decision. At 120 BPM an 1/8 note lasts 250 ms, so the decision is normally ready before the next grid line.

**v2 option:** a small CNN on CQT frames, trained on GuitarSet, run with RTNeural or Core ML. It would be added behind the same `ChordClassifier` interface only if v1 accuracy falls short.

---

## 6. Real-time quantization design

A real-time quantizer can only delay events. It cannot move them earlier. The design works within that:

| Setting | Options |
|---|---|
| Grid | Off, 1 bar, 1/2, 1/4, 1/8, 1/16, plus triplet variants. Swing 0–75% |
| Mode | **Next grid:** the chord plays at the next grid line after its onset. **Nearest (catch window):** if the onset is within *X*% of a grid line *after* it, emit immediately; otherwise wait for the next line. |
| Change policy | *Hold:* the chord sounds until the next quantized change. *Fixed length:* note length equals N grid steps. *Gate:* the note ends when the input goes silent. |
| No transport | When Live is stopped: emit unquantized, or use an internal clock at the last known tempo |

**Scheduling:** at each block, record `ppqPosition`, `bpm` and the host time at block start. For a target grid position *p*, compute `hostTime = blockHostTime + (p − blockPpq) × 60/bpm`. The note-on and the previous chord's note-off are then scheduled with that timestamp. Tempo changes cause the pending schedule to be recalculated.

**Recommended Live setup:** also turn on *Edit → Record Quantization* at the same grid. The plugin keeps real-time playback on the grid, and Live makes the recorded clip exact even in "Nearest" edge cases.

**Safety:** send All-Notes-Off on transport stop, bypass, parameter changes that invalidate held notes, and plugin destruction.

---

## 7. MIDI output and voicing

- **Voicing styles:** root-position close voicing, spread (root an octave down), or triad plus bass note (uses bass chroma for inversions and slash chords). User setting for octave (C2–C5).
- **Velocity:** fixed, or taken from input RMS at the onset (with a curve setting).
- **Channel:** 1–16. An optional "chord root only" output channel for bass lines.
- **Port naming:** "NoteCap Out *n*", unique per instance and stored in plugin state, so Live's track routing still works after a project is reopened.

---

## 8. UI (v1)

- A large current-chord label (e.g. `Am7/G`) with a confidence bar, and the previous chord shown faded.
- A 12-bar chroma display and an input level meter with a gate threshold marker.
- **Source:** Sidechain or Main input. **Detection:** sensitivity, minimum duration, vocabulary checkboxes, tuning readout.
- **Quantize:** grid, mode, catch window, swing, change policy.
- **Output:** voicing style, octave, velocity mode, channel, port name, and a "Send test chord" button.
- A status line such as "Port NoteCap Out 1 active · Transport playing · 120 BPM".

---

## 9. Milestones

| Phase | Duration | Deliverable | Exit criteria |
|---|---|---|---|
| **0. Feasibility spikes** | 3–5 days | Throwaway JUCE plugin | Tested in the user's Live version: (a) an AU effect with a sidechain bus appears in Live's sidechain selector and receives audio; (b) the in-plugin CoreMIDI virtual port appears in Live's MIDI preferences and can be recorded; (c) scheduled timestamps land accurately (measured in a recorded clip). **Choose Mode A, B or VST3.** |
| **1. Skeleton, end to end** | 1 week | CMake + JUCE project; AU/VST3/Standalone; pass-through; virtual port that sends a fixed C-major chord on every quantized beat | Live records perfectly quantized test chords. `auval` and `pluginval` pass. This proves routing and timing before any DSP work. |
| **2. Offline chord engine** | 2–3 weeks | `core/` library, evaluation CLI, GuitarSet scoring | At least 80% majmin weighted recall. Unit tests for templates, tuning and tracking. |
| **3. Real-time integration** | 1–2 weeks | Engine on a worker thread, lock-free FIFOs, onset back-dating | Stable at 32–2048 sample buffers and 44.1/48/96 kHz. Under 5% CPU on an M-series core. No dropouts. |
| **4. Quantizer and voicing** | 1 week | All quantize modes, change policies and voicings | Unit tests on synthetic transports (including tempo changes and loop wrap). Recording test in Live passes R5. |
| **5. UI** | 1–2 weeks | Editor from §8 | All parameters automatable. State restores correctly after reopening a Live set. |
| **6. Hardening and release** | 1 week | Multi-instance, edge cases, codesign and notarized `.pkg` installer | Four instances run at once without port collisions. Survives Live's plugin rescan. Clean install on a fresh Mac. |

**Total: about 8–11 weeks** for one developer working part time or full time.

---

## 10. Testing strategy

- **Unit tests (Catch2):** synthetic sine and sawtooth chords → correct label; detuned input → correct label; Quantizer maths across grids, swing, tempo change and loop points.
- **Offline accuracy:** `tools/eval` on GuitarSet comping tracks plus our own recordings (acoustic guitar, electric clean/driven, piano, mic in the room). Report majmin, sevenths and segmentation scores for each release.
- **Timing tests:** a Live project with a click-synced audio clip of strummed chords. Record the output and measure the offset from the grid for each note (target: 0 ticks with timestamps honoured).
- **Host validation:** `auval -v aufx NtCp Manu`, and `pluginval` at strictness 10, in CI (GitHub Actions macOS runner).
- **Manual Live checklist:** save/reopen, freeze/flatten, bypass, deleting the device while notes are held, changing the audio device.

---

## 11. Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| AU sidechain not delivered in the user's Live version | Medium | High | Phase 0 spike; Mode B (plugin on the audio track) |
| Live ignores CoreMIDI timestamps | Medium | Medium | Send events at their due time from a high-priority thread, plus Live Record Quantization |
| Accuracy with distorted guitar, vocals or a dense mix | High | Medium | Tunable gate and vocabulary; recommend a clean DI or isolated source; v2 ML classifier |
| Detection latency longer than a grid step at fast tempos or fine grids | Medium | Medium | Document limits; "Next grid" mode stays correct but late; recommend 1/4 or coarser for chords |
| Feedback loop (plugin output → synth → back into analysis) | Low | Medium | Analyse only the sidechain in Mode A; warn in the UI if the source is Main on a MIDI track |
| GPL licensing from borrowed DSP code | Low | High | Implement our own algorithms; review licences of any dependencies |
| Codesigning and notarization problems on Apple Silicon | Low | Medium | Set up a Developer ID early (Phase 1) and sign in CI |

---

## 12. Proposed repository layout

```
notecap/
├── CMakeLists.txt
├── cmake/                  # JUCE via FetchContent / CPM
├── src/
│   ├── core/               # pure C++ DSP + quantizer (no JUCE)
│   ├── midi/               # CoreMIDI virtual port + scheduler
│   ├── plugin/             # AudioProcessor, parameters, state
│   └── ui/                 # Editor + components
├── tests/                  # Catch2 unit tests
├── tools/eval/             # offline CLI + mir_eval scripts
├── data/                   # (gitignored) datasets, test recordings
├── live/                   # Ableton test sets for timing/routing checks
└── .github/workflows/      # build, auval, pluginval
```

---

## 13. Open questions

1. **Live version and edition:** 11 or 12, and Suite or not? Suite includes Max for Live, which would allow a Live-native alternative front end later.
2. **Main input sources:** guitar, piano/keys, voice, or full mixes? This sets the priorities for tuning the detection.
3. **Personal use or distribution?** This decides whether Developer ID signing, notarization and an installer are needed in v1.
4. **Output:** a fixed voicing of the detected chord, or an attempt to reproduce the notes actually played (much harder, v2)?
5. **Mac:** Apple Silicon, Intel, or both (universal binary)?

---

## 14. Status (2026-09-23)

| Phase | State | Notes |
|---|---|---|
| 0. Feasibility | **Built in, awaiting your Live test** | *Test chords* mode, the *Port offset* calibration control and the status panel cover spikes (a)–(c). The checklist is in README.md. |
| 1. Skeleton | **Done** | Projucer project; AU (`aufx`, main + sidechain buses, MIDI out) / VST3 / Standalone. `auval` passes. CoreMIDI virtual port with stable per-instance names. |
| 2. Offline engine | **Done (synthetic data)** | 98.7% majmin / 92% exact on a 300-strum randomised Karplus-Strong benchmark (97% on held-out seeds). Real recordings are still needed: use `Tools/eval`. |
| 3. Real-time integration | **Done** | Block-size independent. Mean decision latency is about 130 ms. |
| 4. Quantizer + voicing | **Done** | Nearest / Next-line, swing, triplets, Hold / fixed lengths; close / open / bass + chord voicings. Integration-tested sample-accurately against a fake 120 BPM transport. |
| 5. UI | **First version done** | Chord, pitch-class and status displays, plus all parameters. Snapshot: `Tests/snapshot_editor.cpp`. |
| 6. Hardening | **Mostly done** | See the Phase 6 log below. `pluginval` passes at strictness 10. Remaining: Developer ID signing and notarization (needs certificates), and checks only possible in Live (plugin rescan, fresh-Mac install). |

**Next steps:** run the Phase 0 checklist in Live 12, then record 5–10 real takes
(with `.lab` references) to measure accuracy on real guitar/keys and retune if
needed.

### Phase 6 log

**Fixed**
- **Audio-thread allocation:** `ChordClassifier::classify()` called
  `Chord::pitchClasses()` (a `std::vector`) on every frame. It now iterates the
  static interval table.
- **Stuck notes on the virtual port:** closing the port while a chord was held
  (port turned off, plugin deleted, set reloaded onto another port) left notes
  on at the receiver. The sender thread now tracks every note it has sent and
  releases them on close. CoreMIDI holds future-stamped events until their
  time and drops pending ones when a source is disposed. So the releases go
  out immediately and again after the latest scheduled event, with up to a
  250 ms wait before disposal.
- **Port lifecycle race:** the timer (message thread) and
  `setStateInformation` (any thread) could open and close the port at the same
  time. They're now serialised with a lock. Removed a FIFO `reset()` that could
  race with the audio thread.
- **Bad input:** NaN/Inf samples are zeroed before the IIR filters, where they
  would otherwise latch forever. A non-finite tempo or song position from the
  host is ignored.

**Added**
- Integration tests for the fixes above, plus four concurrent instances with
  channel-tagged output and no crosstalk.
- `Tools/package.sh`: builds a universal (arm64 + x86_64) `.pkg` with AU/VST3
  choices, non-relocatable bundles and an AU-cache refresh. It signs,
  notarizes and staples when Developer ID variables are set. CI builds and
  uploads it on every push.

**Open**
- A Developer ID Application/Installer certificate. This Mac only has an
  *Apple Development* identity, so packages are ad-hoc signed for now.
- ~~`pluginval`~~ **Passed (v1.0.4, strictness 10)**, including parameter
  thread safety, fuzzing, state restoration and bus-layout tests.
  - VST3: clean, no warnings.
  - AU: passes with two warnings that come from the AU format, not from
    NoteCap: "current program is -1" (the AU convention for no factory
    preset), and "disabling non-main buses failed" (an AU host can't disable
    an input bus; Live never disables the sidechain).
  - Not yet run in CI.
- Checks that need Live: plugin rescan, and installing on a clean Mac.
