# NoteCap

An Audio Unit for Ableton Live that listens to audio (a guitar, keys or a mic),
recognises the chord being played, and sends it as **quantized MIDI** that a
MIDI track can record. Built with JUCE. Also builds as VST3 and a Standalone
app.

See [PLAN.md](PLAN.md) for the full project plan and design decisions.

## How it fits into Live

Live can't route MIDI *out of* AU plugins. NoteCap therefore publishes its
chords on its own **CoreMIDI virtual port** (`NoteCap Out 1`, `NoteCap Out 2`,
…). Live sees that port like any hardware MIDI input.

```
Audio track "Guitar"  (input: your interface / mic, Monitor: In or Auto)
        │ sidechain
        ▼
MIDI track "Chords"   [ Wavetable (or any instrument) ] → [ NoteCap ]
  MIDI From: NoteCap Out 1   (armed)
```

1. **Preferences → Link, Tempo & MIDI:** once NoteCap has been loaded, turn on
   **Track** for the input port `NoteCap Out 1`.
2. Create a MIDI track with any instrument, then add **NoteCap** *after* the
   instrument. It's an audio effect and passes the synth's audio through.
3. In NoteCap's device header, open the **Sidechain** section and pick the
   audio track you want to listen to.
4. Set the MIDI track's **MIDI From** to `NoteCap Out 1` and arm it.
5. Play. The detected chord appears in NoteCap and the instrument plays it on
   the grid. Press record to capture it.
6. Recommended: set **Edit → Record Quantization** to the same grid as NoteCap.

If Live's sidechain selector doesn't feed NoteCap, use the fallback: put
NoteCap on the **audio track** itself, set *Source* to **Main input**, and keep
the MIDI track's MIDI From on `NoteCap Out 1`.

## Controls

| Group | Control | What it does |
|---|---|---|
| Detect | Source | Sidechain (default) or the main input |
| | Gate | Input below this level counts as silence and releases the chord |
| | Sensitivity | Higher detects more readily; lower needs a clearer chord |
| | Min change | How long a chord change without a new strum must last (legato playing) |
| | 7th / Sus / Dim-aug | Chord vocabulary. Triads are always on |
| | Retrigger | Re-send the chord when the same chord is strummed again |
| Quantize | Grid | Off, 1 bar … 1/16, plus triplets |
| | Mode | **Nearest**: the grid line closest to the strum. **Next line**: always the next line after detection. Always audibly on the grid, but can be one line late |
| | Swing | Delays every other grid line |
| | Length | Hold until the next chord, or a fixed 1/2/4/8 grid steps |
| Output | Voicing / Octave | Close, open, or bass + chord. Octave uses Live's naming (C3 = middle C) |
| | Velocity | From the input level, or fixed |
| | Channel | MIDI channel 1–16 |
| | Port offset | Shifts the virtual port's timestamps to calibrate timing (see below) |
| | Virtual MIDI port | Publish `NoteCap Out n` |
| | Test chords | Ignores audio and plays C F G Am on every grid line. Use it to check routing and timing |

**On quantizing in real time:** a chord can only be recognised about 110–200 ms
after the strum, so a chord can't be moved *earlier*. In *Nearest* mode, if the
chord's grid line hasn't arrived yet, the chord is scheduled for it. If the line
has passed, the chord is sent at once and its virtual-port timestamp is
back-dated to the grid line. This matters most at fine grids (1/16) and high
tempos. Coarse grids (1/4, 1/2, bar) usually wait for the line.

## Phase 0 checklist (verify in your Live 12)

These are the host behaviours the design depends on. They can only be checked
inside Live:

- [ ] **Port visible:** after loading NoteCap, `NoteCap Out 1` appears in
      Preferences → Link, Tempo & MIDI (restart Live if not), and a MIDI track
      can select it as *MIDI From*.
- [ ] **Routing + timing (Test chords):** turn on *Test chords*, Grid 1/4, press
      play and record 8 bars on the armed MIDI track. Chords should be C F G Am
      on every beat. Zoom in: are they exactly on the beat? If they're
      consistently early or late, note the offset and set **Port offset** to
      the opposite value, then re-check.
- [ ] **Sidechain:** set Source to Sidechain, choose the guitar track in
      NoteCap's sidechain selector, and play. The Status panel should show
      `Input: sidechain` with a moving meter, not "sidechain not connected".
- [ ] **Back-dating:** Grid 1/4, Mode Nearest, Live's Record Quantization
      *off*. Strum on the beat and record. Are the chords on the beat (Live
      honours timestamps) or ~150 ms late (it doesn't)? If late, turn Record
      Quantization on.
- [ ] Save the set, close Live, reopen: is the track still receiving from the
      same `NoteCap Out n`?

## Building

Requires Xcode and a JUCE 8 checkout at `~/Development/JUCE` (no brew, no cmake).

```sh
# regenerate the Xcode project after editing NoteCap.jucer
~/Development/JUCE/extras/Projucer/Builds/MacOSX/build/Release/Projucer.app/Contents/MacOS/Projucer \
    --resave NoteCap.jucer

# build AU + VST3 + Standalone (copies to ~/Library/Audio/Plug-Ins/)
xcodebuild -project Builds/MacOSX/NoteCap.xcodeproj -target "NoteCap - All" -configuration Release build

# validate the AU
auval -v aufx NtCp ARut
```

If Live doesn't list it after a rescan, run `killall -9 AudioComponentRegistrar`
and restart Live.

## Installer

```sh
Tools/package.sh              # Release build -> dist/NoteCap-<version>.pkg
Tools/package.sh --no-build   # package the existing Release build
```

The installer offers AU and VST3 choices and installs to
`/Library/Audio/Plug-Ins/…`. It also refreshes the AU cache. CI builds the same
package on every push (the *NoteCap-installer* artifact).

Without extra setup the plugins are **ad-hoc signed**. That works on the Mac
that built them, but Gatekeeper blocks the package on other Macs. To
distribute it, get **Developer ID Application** and **Developer ID Installer**
certificates (Apple Developer Program), then:

```sh
xcrun notarytool store-credentials notecap --apple-id you@example.com --team-id TEAMID
DEVELOPER_ID_APP="Developer ID Application: Your Name (TEAMID)" \
DEVELOPER_ID_INSTALLER="Developer ID Installer: Your Name (TEAMID)" \
NOTARY_PROFILE=notecap Tools/package.sh
```

This signs with the hardened runtime, notarizes and staples the package.

**Dev builds and the installer don't mix:** Xcode builds copy the plugins into
`~/Library/Audio/Plug-Ins/`, and the installer puts them in `/Library/…`. With
both present, Live may load either one. Remove one of the two.

## Tests

```sh
Tests/run_tests.sh          # core DSP: classifier, quantizer, voicing, engine + randomised benchmark
Tests/run_integration.sh    # real processor + fake transport + CoreMIDI round trip (needs a Release build)
```

The integration test also covers these hardening checks:
- no heap allocation inside `processBlock`;
- no stuck notes when the virtual port closes while a chord is held, with both
  a real-time clock and a clock running ahead of it;
- recovery from NaN/Inf input;
- four instances processing at once on separate threads, with no crosstalk
  between their ports.

The engine benchmark renders 300 synthetic strummed-guitar chords at 44.1, 48
and 96 kHz, with random tuning and strum speeds. Current result: **98.7%
majmin** / 92% exact-label accuracy, about 130 ms mean decision latency.

## Evaluating on your own recordings

```sh
Tools/eval/build.sh
Tools/eval/build/notecap_eval take.wav              # prints a .lab of detected chords
Tools/eval/build/notecap_eval take.wav take.lab     # + majmin accuracy vs a reference
```

Reference `.lab` files use `start end label` lines with Harte labels
(`A:min`, `G:7`, `N`). You can make them by hand or export from Sonic
Visualiser.

## Layout

```
Source/core/     pure C++ DSP (no JUCE): chroma (NNLS), onset, classifier, engine, quantizer, voicing
Source/midi/     CoreMIDI virtual source + sender thread
Source/          PluginProcessor (scheduling, transport), PluginEditor (UI)
Tests/           unit tests, engine benchmark, integration test, editor snapshot
Tools/eval/      offline evaluator for WAV + .lab
```
