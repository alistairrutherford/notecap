"""NoteCap in Live: chord detection end to end, with no audio files.

    "NoteCap Chords"  (MIDI)  Chordism, Test pattern on: C major triads, one per beat
                              (C Dm Em F G Am Bdim, an eighth note each)
    "NoteCap Synth"   (MIDI)  MIDI From: NoteCap Chords / Chordism, monitoring In
                              [ Drift ] -> [ NoteCap, Source: Main input, Grid Off ]
    "NoteCap Detect"  (MIDI)  MIDI From: NoteCap Synth / NoteCap, armed: records NoteCap's chords

Each beat's first recorded chord is compared (as pitch classes) with what Chordism played, and
its delay after the beat is reported: that is NoteCap's detection latency on a real synth
through Live. Skips if Chordism isn't installed. Chordism has more than 64 parameters, so
Live's Options.txt needs -_PluginAutoPopulateThreshold=128 (see Chordism's LiveBridge README).

Settings (environment variables): NOTECAP_PLUGIN, NOTECAP_INSTRUMENT (as the routing test),
    NOTECAP_DETECT_TEMPO   BPM (default 60: a 1 s beat, 500 ms chords)
    NOTECAP_DETECT_BEATS   beats to record (default 14: the seven chords twice)
    NOTECAP_DETECT_MIN     share of beats that must be right (default 0.85)
"""

import os
import statistics

import pytest

from livebridge import livelog
from rig import INSTRUMENT, NOTECAP, device, prepare_song, record, route_from_plugin, save_report, \
    set_param, track

TEMPO = float(os.environ.get("NOTECAP_DETECT_TEMPO", "60"))
BEATS = int(os.environ.get("NOTECAP_DETECT_BEATS", "14"))
MIN_RIGHT = float(os.environ.get("NOTECAP_DETECT_MIN", "0.85"))
CHORDISM = "Chordism"

# C major triads by scale degree, as pitch classes; Chordism's test pattern plays slot b mod 7 on beat b.
NAMES = ["C", "Dm", "Em", "F", "G", "Am", "Bdim"]
PCS = [{0, 4, 7}, {2, 5, 9}, {4, 7, 11}, {5, 9, 0}, {7, 11, 2}, {9, 0, 4}, {11, 2, 5}]

pytestmark = pytest.mark.live


@pytest.fixture(scope="module")
def rig(live):
    if not live.call("browser_search", query=CHORDISM, category="plugins", format="vst3", limit=5):
        pytest.skip("Chordism (VST3) is not installed")
    prepare_song(live, TEMPO)

    chords = track(live, "NoteCap Chords", "midi", [(CHORDISM, "plugins")])
    synth = track(live, "NoteCap Synth", "midi", [(INSTRUMENT, "instruments"), (NOTECAP, "plugins")])
    rec = track(live, "NoteCap Detect", "midi")

    cd = device(live, chords, CHORDISM)
    shown = {"Chordism": {
        "Enable": set_param(live, chords, cd, "Enable", True),
        "Key": set_param(live, chords, cd, "Key", "C"),
        "Scale": set_param(live, chords, cd, "Scale", "Major"),
        "Chord Group": set_param(live, chords, cd, "Chord Group", "Triads"),
        "Split": set_param(live, chords, cd, "Split", 60, (0, 127)),
        "Chord Octave": set_param(live, chords, cd, "Chord Octave", 0, (-3, 3)),
    }}
    set_param(live, chords, cd, "Mute CG", False)
    set_param(live, chords, cd, "Mute SS", False)

    nd = device(live, synth, NOTECAP)
    shown["NoteCap"] = {
        "Source": set_param(live, synth, nd, "Source", "Main input"),
        "Grid": set_param(live, synth, nd, "Grid", "Off"),
        "7th Chords": set_param(live, synth, nd, "7th Chords", False),
        "Sus Chords": set_param(live, synth, nd, "Sus Chords", False),
        "Dim/Aug Chords": set_param(live, synth, nd, "Dim/Aug Chords", True),
        "Test Chords": set_param(live, synth, nd, "Test Chords", False),
    }

    route_from_plugin(live, synth, chords, CHORDISM)
    live.call("set_track", track=synth, arm=False, monitoring="in")   # plays Chordism without being armed
    routing = route_from_plugin(live, rec, synth, NOTECAP)
    set_param(live, chords, cd, "Test pattern", True)
    yield {"chords": chords, "synth": synth, "rec": rec, "shown": shown, "routing": routing["current"]}
    set_param(live, chords, cd, "Test pattern", False)
    live.call("stop_transport")


def test_detects_chordism_chords(live, rig, report):
    clip, notes = record(live, rig["rec"], float(BEATS))
    audio = livelog.audio_settings(live.call("ping")["live_version"])

    lines, right, latencies = [], 0, []
    for b in range(BEATS):
        # The first chord NoteCap sent after beat b (its notes share one start time).
        in_beat = [n for n in notes if b <= n["song_time"] < b + 1]
        t0 = min((n["song_time"] for n in in_beat), default=None)
        chord = {n["pitch"] % 12 for n in in_beat if t0 is not None and abs(n["song_time"] - t0) < 1e-6}
        ok = chord == PCS[b % 7]
        right += ok
        lat = (t0 - b) * 60000.0 / TEMPO if t0 is not None else None
        if ok and lat is not None:
            latencies.append(lat)
        lines.append("beat %2d  Chordism %-5s  NoteCap %-16s %8s  %s" % (
            b, NAMES[b % 7], sorted(chord) if chord else "-", "%.0f ms" % lat if lat is not None else "",
            "ok" if ok else "WRONG"))

    share = right / BEATS
    text = "\n== NoteCap detecting Chordism's C major triads through %s (%.0f BPM%s) ==\n" % (
        INSTRUMENT, TEMPO, ", %d Hz, buffer %s" % (audio["sample_rate"], audio.get("output_buffer"))
        if audio.get("sample_rate") else "")
    text += "\n".join(lines)
    text += "\nright: %d of %d beats (%.0f%%)" % (right, BEATS, share * 100)
    if latencies:
        text += "; detection latency: median %.0f ms, max %.0f ms" % (statistics.median(latencies), max(latencies))
    text += "\nsettings: %s" % rig["shown"]
    report(text)
    save_report("detection-chordism", {"shown": rig["shown"], "routing": rig["routing"], "audio": audio,
                                       "clip": clip, "right": right, "beats": BEATS, "latencies_ms": latencies})

    assert notes, "NoteCap sent nothing (is Drift sounding on the Synth track?)"
    assert share >= MIN_RIGHT, "only %d of %d beats detected correctly" % (right, BEATS)
