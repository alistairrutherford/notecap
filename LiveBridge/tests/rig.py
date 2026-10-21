"""Shared set-up for NoteCap's Live tests: find or build tracks, set plug-in parameters by
name, route MIDI From another track's plug-in, record, and save reports."""

import datetime
import json
import os
import time

import pytest

from livebridge.client import BridgeError

NOTECAP = os.environ.get("NOTECAP_PLUGIN", "NoteCap")
INSTRUMENT = os.environ.get("NOTECAP_INSTRUMENT", "Drift")
HERE = os.path.dirname(os.path.abspath(__file__))
REPORT_DIR = os.path.join(os.path.dirname(HERE), "reports")

# Choice lists of NoteCap's parameters, in order (Source/PluginProcessor.cpp, createLayout).
# Live shows no value items for a VST3 plug-in's choice parameters, so a choice is set by its
# normalised position, index / (count - 1), and checked against the display text.
NOTECAP_CHOICES = {
    "Source": ["Sidechain", "Main input"],
    "Grid": ["Off", "1 Bar", "1/2", "1/4", "1/8", "1/16", "1/4T", "1/8T", "1/16T"],
    "Quantize Mode": ["Nearest", "Next line"],
    "Length": ["Hold", "1 step", "2 steps", "4 steps", "8 steps"],
    "Voicing": ["Close", "Open", "Bass + chord"],
    "Velocity Mode": ["From input", "Fixed"],
}

# Chordism's (Source/plugin/Parameters.cpp and its theory tables), for the detection test.
CHORDISM_CHOICES = {
    "Key": ["C", "C#/Db", "D", "Eb", "E", "F", "F#/Gb", "G", "Ab", "A", "Bb", "B"],
    "Scale": ["Major", "Natural Minor", "Harmonic Minor", "Melodic Minor", "Dorian", "Phrygian", "Lydian",
              "Mixolydian", "Locrian", "Harmonic Major", "Phrygian Dominant", "Hungarian Minor", "Double Harmonic"],
    "Chord Group": ["Triads", "Power (1-5)", "Sus2", "Sus4", "6ths", "Sevenths", "Add9", "Ninths", "Elevenths",
                    "Thirteenths", "1st Inversion", "2nd Inversion", "Open Triads", "Drop-2 Sevenths",
                    "Shell (1-3-7)", "Neo-Soul", "Quartal"],
}

# NoteCap's Close voicing at Octave 3 (C3 = MIDI 60), as pitch lists.
CLOSE = {"C": [60, 64, 67], "F": [65, 69, 72], "G": [67, 71, 74], "Am": [69, 72, 76]}


def track(live, name, kind="midi", devices=()):
    """The track called `name`, created (with `devices` loaded in order) if it doesn't exist.
    `devices` is a list of (browser name, category)."""
    state = live.call("get_state")
    t = next((t for t in state["tracks"] if t["name"] == name), None)
    if t is None:
        t = live.call("create_audio_track" if kind == "audio" else "create_midi_track", name=name)
        for dev, category in devices:
            live.load_device_and_wait(dev, t["index"], category, "vst3" if category == "plugins" else None)
    return t["index"]


def device(live, track_index, name):
    devs = live.call("list_devices", track=track_index)
    d = next((d for d in devs if d["name"].lower() == name.lower()), None)
    if d is None:
        pytest.fail("no device %r on track %d; have %s" % (name, track_index + 1, [x["name"] for x in devs]))
    return d["index"]


def set_param(live, track_index, device_index, name, value, int_range=None, choices=None):
    """Set a plug-in parameter by name.

    value: bool (on/off); a str, one of `choices` (the plug-in's list for this parameter, e.g.
    NOTECAP_CHOICES["Grid"]); or a number in the parameter's own units with `int_range=(lo, hi)`
    (plug-in parameters are 0..1 in Live). Returns the parameter's display text after the change.
    """
    p = live.call("get_parameter", track=track_index, device=device_index, parameter=name)
    want_display = None
    if isinstance(value, str):
        if choices is None or value not in choices:
            pytest.fail("set %s = %r: pass the plug-in's choice list (have %s)" % (name, value, choices))
        want_display = value
        value = p["min"] + (p["max"] - p["min"]) * choices.index(value) / (len(choices) - 1)
    elif int_range is not None:
        lo, hi = int_range
        value = p["min"] + (p["max"] - p["min"]) * (value - lo) / (hi - lo)
    try:
        shown = live.call("set_parameter", track=track_index, device=device_index, parameter=name, value=value)["display"]
    except BridgeError as e:
        pytest.fail("could not set %s = %r (%s)" % (name, value, e.error))
    if want_display is not None and shown.strip() != want_display:
        pytest.fail("set %s to %r but Live shows %r" % (name, want_display, shown))
    return shown


def route_from_plugin(live, rec_index, src_index, plugin_name):
    """rec's MIDI From: the src track, then the plug-in in the second chooser."""
    r = live.call("set_input_routing", track=rec_index, source_track=src_index)
    names = [c["display_name"] for c in r["available_channels"]]
    chan = next((n for n in names if n.lower() == plugin_name.lower()), None)
    if chan is None:
        pytest.fail("Live lists no MIDI From channel %r on track %d; channels: %s" % (plugin_name, src_index + 1, names))
    return live.call("set_input_routing", track=rec_index, channel=chan)


def record(live, rec_index, beats):
    """Arm rec, record `beats` beats into the arrangement from beat 0, return the notes in range."""
    live.call("set_track", track=rec_index, arm=True, monitoring="auto")
    live.call("clear_arrangement_clips", track=rec_index)
    time.sleep(0.3)
    clip = live.record_arrangement(rec_index, beats, from_beat=0.0)
    live.call("set_track", track=rec_index, arm=False)
    notes = [n for n in clip["notes"] if -0.5 <= n["song_time"] < beats - 0.5]
    return clip, notes


def prepare_song(live, tempo):
    live.call("stop_transport")
    live.call("set_song_properties", loop=False, arrangement_overdub=False, midi_recording_quantization="none",
              signature_numerator=4, signature_denominator=4)
    live.call("set_tempo", bpm=tempo)


def save_report(name, payload):
    os.makedirs(REPORT_DIR, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    path = os.path.join(REPORT_DIR, "%s-%s.json" % (name, stamp))
    with open(path, "w") as f:
        json.dump(payload, f, indent=2, default=str)
    return path


def by_beat(notes):
    out = {}
    for n in notes:
        out.setdefault(int(round(n["song_time"])), []).append(n)
    return out


# -- the Chordism -> Drift chain (detection, sidechain and Record Quantization tests) ----------

CHORDISM = "Chordism"
# C major triads by scale degree, as pitch classes; Chordism's test pattern plays slot b mod 7 on beat b.
TRIAD_NAMES = ["C", "Dm", "Em", "F", "G", "Am", "Bdim"]
TRIAD_PCS = [{0, 4, 7}, {2, 5, 9}, {4, 7, 11}, {5, 9, 0}, {7, 11, 2}, {9, 0, 4}, {11, 2, 5}]


def chordism_chain(live, tempo):
    """Build (or reuse) the chain and return its track and device indices:

        "NoteCap Chords"  (MIDI)  Chordism: C major triads, one per beat while its Test pattern is on
        "NoteCap Synth"   (MIDI)  MIDI From: NoteCap Chords / Chordism, monitoring In
                                  [ Drift ] -> [ NoteCap ]

    Skips the test if Chordism isn't installed. Chordism has more than 64 parameters, so Live's
    Options.txt needs -_PluginAutoPopulateThreshold=128.
    """
    if not live.call("browser_search", query=CHORDISM, category="plugins", format="vst3", limit=5):
        pytest.skip("Chordism (VST3) is not installed")
    prepare_song(live, tempo)
    chords = track(live, "NoteCap Chords", "midi", [(CHORDISM, "plugins")])
    synth = track(live, "NoteCap Synth", "midi", [(INSTRUMENT, "instruments"), (NOTECAP, "plugins")])
    cd = device(live, chords, CHORDISM)
    shown = {
        "Enable": set_param(live, chords, cd, "Enable", True),
        "Key": set_param(live, chords, cd, "Key", "C", choices=CHORDISM_CHOICES["Key"]),
        "Scale": set_param(live, chords, cd, "Scale", "Major", choices=CHORDISM_CHOICES["Scale"]),
        "Chord Group": set_param(live, chords, cd, "Chord Group", "Triads", choices=CHORDISM_CHOICES["Chord Group"]),
        "Split": set_param(live, chords, cd, "Split", 60, (0, 127)),
        "Chord Octave": set_param(live, chords, cd, "Chord Octave", 0, (-3, 3)),
    }
    set_param(live, chords, cd, "Mute CG", False)
    set_param(live, chords, cd, "Mute SS", False)
    route_from_plugin(live, synth, chords, CHORDISM)
    live.call("set_track", track=synth, arm=False, monitoring="in")   # plays Chordism without being armed
    return {"chords": chords, "chordism": cd, "synth": synth, "notecap": device(live, synth, NOTECAP),
            "chordism_shown": shown}


def chordism_pattern(live, chain, on):
    set_param(live, chain["chords"], chain["chordism"], "Test pattern", bool(on))


def detect_params(live, track_index, device_index, source="Main input", grid="Off", mode="Nearest"):
    """NoteCap settings for detecting Chordism's triads (Bdim needs dim/aug on, no sevenths)."""
    nc = NOTECAP_CHOICES
    return {
        "Source": set_param(live, track_index, device_index, "Source", source, choices=nc["Source"]),
        "Grid": set_param(live, track_index, device_index, "Grid", grid, choices=nc["Grid"]),
        "Quantize Mode": set_param(live, track_index, device_index, "Quantize Mode", mode, choices=nc["Quantize Mode"]),
        "7th Chords": set_param(live, track_index, device_index, "7th Chords", False),
        "Sus Chords": set_param(live, track_index, device_index, "Sus Chords", False),
        "Dim/Aug Chords": set_param(live, track_index, device_index, "Dim/Aug Chords", True),
        "Test Chords": set_param(live, track_index, device_index, "Test Chords", False),
    }


def score_triads(notes, beats, tempo):
    """For each beat, the first chord NoteCap sent in it (its notes share a start time) against
    Chordism's triad. Returns lines, the number right, and each beat's start offset in ms."""
    lines, right, offsets = [], 0, []
    for b in range(beats):
        in_beat = [n for n in notes if b <= n["song_time"] < b + 1]
        t0 = min((n["song_time"] for n in in_beat), default=None)
        chord = {n["pitch"] % 12 for n in in_beat if t0 is not None and abs(n["song_time"] - t0) < 1e-6}
        ok = chord == TRIAD_PCS[b % 7]
        right += ok
        ms = (t0 - b) * 60000.0 / tempo if t0 is not None else None
        if ok and ms is not None:
            offsets.append(ms)
        lines.append("beat %2d  Chordism %-5s  NoteCap %-16s %10s  %s" % (
            b, TRIAD_NAMES[b % 7], sorted(chord) if chord else "-", "%+.1f ms" % ms if ms is not None else "",
            "ok" if ok else "WRONG"))
    return {"lines": lines, "right": right, "offsets_ms": offsets}
