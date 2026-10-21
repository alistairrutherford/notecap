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
