"""NoteCap in Live: single-notes mode on a known melody, with no audio files.

    "NoteCap Melody"  (MIDI)  a session clip with the melody below -> [ Drift ] -> [ NoteCap,
                              Mode: Single notes, Source: Main input, Grid Off ]
    "NoteCap Notes"   (MIDI)  MIDI From: NoteCap Melody / NoteCap, armed: records NoteCap's notes

Each melody note must come back with the same pitch, once, shortly after it was played; the
delay is NoteCap's single-note latency through a real synth in Live.

Drift's default patch adds a sine an octave down (Osc 2, Oct -1). That sub-octave is then the
sound's real fundamental, and single-notes mode reports it, as it should; so the test switches
Osc 2 off to play the melody's own pitches.

Settings (environment variables): NOTECAP_PLUGIN, NOTECAP_INSTRUMENT, and
    NOTECAP_NOTES_TEMPO   BPM (default 120: a quarter note per melody note)
    NOTECAP_NOTES_MIN     share of melody notes that must come back right (default 0.9)
"""

import os
import statistics

import pytest

from livebridge import livelog
from rig import INSTRUMENT, NOTECAP, NOTECAP_CHOICES as NC, device, prepare_song, record, route_from_plugin, \
    save_report, set_param, track
from livebridge.client import BridgeError

TEMPO = float(os.environ.get("NOTECAP_NOTES_TEMPO", "120"))
MIN_RIGHT = float(os.environ.get("NOTECAP_NOTES_MIN", "0.9"))

# One per beat, 0.75 beats long: across a guitar's range, with octave jumps and repeated notes.
MELODY = [40, 47, 52, 55, 59, 64, 59, 55, 57, 57, 52, 64, 76, 64, 45, 40]

pytestmark = pytest.mark.live


@pytest.fixture(scope="module")
def rig(live):
    prepare_song(live, TEMPO)
    try:
        live.call("stop_all_clips")
    except BridgeError as e:
        pytest.skip("the bridge in Live is older than this test (restart Live to load it): %s" % e.error)
    melody = track(live, "NoteCap Melody", "midi", [(INSTRUMENT, "instruments"), (NOTECAP, "plugins")])
    rec = track(live, "NoteCap Notes", "midi")
    nd = device(live, melody, NOTECAP)
    if INSTRUMENT == "Drift":
        live.call("set_parameter", track=melody, device="Drift", parameter="Osc 2 On", value="Off")
    shown = {
        "Mode": set_param(live, melody, nd, "Mode", "Single notes", choices=NC["Mode"]),
        "Source": set_param(live, melody, nd, "Source", "Main input", choices=NC["Source"]),
        "Grid": set_param(live, melody, nd, "Grid", "Off", choices=NC["Grid"]),
        "Length": set_param(live, melody, nd, "Length", "Hold", choices=NC["Length"]),
        "Test Chords": set_param(live, melody, nd, "Test Chords", False),
    }
    live.call("create_midi_clip", track=melody, scene=0, length=float(len(MELODY)), name="NoteCap melody",
              notes=[{"pitch": p, "start": float(i), "duration": 0.75, "velocity": 100} for i, p in enumerate(MELODY)])
    route_from_plugin(live, rec, melody, NOTECAP)
    yield {"melody": melody, "rec": rec, "device": nd, "shown": shown}
    live.call("stop_all_clips")
    live.call("clear_arrangement_clips", track=melody)   # arrangement recording copies the session clip there
    live.call("stop_transport")


def test_single_notes_recorded(live, rig, report):
    live.call("fire_clip", track=rig["melody"], scene=0)   # starts with the transport, at beat 0
    clip, notes = record(live, rig["rec"], float(len(MELODY)))
    live.call("stop_all_clips")
    audio = livelog.audio_settings(live.call("ping")["live_version"])

    lines, right, lat, used = [], 0, [], set()
    for b, want in enumerate(MELODY):
        hits = [n for n in notes if b <= n["song_time"] < b + 0.5 and n["pitch"] == want]
        ok = len(hits) == 1
        right += ok
        if hits:
            used.add(id(hits[0]))
            lat.append((hits[0]["song_time"] - b) * 60000.0 / TEMPO)
        others = sorted(n["pitch"] for n in notes if b <= n["song_time"] < b + 1 and n["pitch"] != want)
        lines.append("beat %2d  played %3d  got %-12s %s%s" % (
            b, want, [n["pitch"] for n in hits] or "-", "%.0f ms  " % lat[-1] if hits else "",
            "ok" if ok and not others else ("WRONG" if not ok else "ok, extra %s" % others)))
    extra = len([n for n in notes if id(n) not in used])

    text = "\n== NoteCap single notes from %s (%.0f BPM%s) ==\n" % (
        INSTRUMENT, TEMPO, ", %d Hz, buffer %s" % (audio["sample_rate"], audio.get("output_buffer"))
        if audio.get("sample_rate") else "")
    text += "\n".join(lines)
    text += "\nright: %d of %d notes, %d extra" % (right, len(MELODY), extra)
    if lat:
        text += "; latency median %.0f ms, max %.0f ms" % (statistics.median(lat), max(lat))
    text += "\nsettings: %s" % rig["shown"]
    report(text)
    save_report("single-notes", {"shown": rig["shown"], "audio": audio, "clip": clip, "right": right,
                                 "extra": extra, "latencies_ms": lat})

    assert notes, "NoteCap sent nothing (is Drift sounding on the Melody track?)"
    assert right / len(MELODY) >= MIN_RIGHT, "only %d of %d melody notes came back right" % (right, len(MELODY))
    assert extra <= len(MELODY) // 8, "%d extra notes" % extra
