"""NoteCap in Live: chord detection end to end, with no audio files.

    "NoteCap Chords"  Chordism, Test pattern on: C major triads, one per beat (C Dm Em F G Am Bdim)
    "NoteCap Synth"   MIDI From Chordism; [ Drift ] -> [ NoteCap, Source: Main input, Grid Off ]
    "NoteCap Detect"  MIDI From: NoteCap Synth / NoteCap, armed: records NoteCap's chords

Each beat's first recorded chord is compared (as pitch classes) with what Chordism played, and
its delay after the beat is reported: NoteCap's detection latency on a real synth in Live.
Skips if Chordism isn't installed.

Settings (environment variables): NOTECAP_PLUGIN, NOTECAP_INSTRUMENT, and
    NOTECAP_DETECT_TEMPO   BPM (default 60: a 1 s beat, 500 ms chords)
    NOTECAP_DETECT_BEATS   beats to record (default 14: the seven chords twice)
    NOTECAP_DETECT_MIN     share of beats that must be right (default 0.85)
"""

import os
import statistics

import pytest

from livebridge import livelog
from rig import INSTRUMENT, NOTECAP, chordism_chain, chordism_pattern, detect_params, record, route_from_plugin, \
    save_report, score_triads, track

TEMPO = float(os.environ.get("NOTECAP_DETECT_TEMPO", "60"))
BEATS = int(os.environ.get("NOTECAP_DETECT_BEATS", "14"))
MIN_RIGHT = float(os.environ.get("NOTECAP_DETECT_MIN", "0.85"))

pytestmark = pytest.mark.live


@pytest.fixture(scope="module")
def rig(live):
    chain = chordism_chain(live, TEMPO)
    shown = {"Chordism": chain["chordism_shown"],
             "NoteCap": detect_params(live, chain["synth"], chain["notecap"])}
    rec = track(live, "NoteCap Detect", "midi")
    routing = route_from_plugin(live, rec, chain["synth"], NOTECAP)
    chordism_pattern(live, chain, True)
    yield dict(chain, rec=rec, shown=shown, routing=routing["current"])
    chordism_pattern(live, chain, False)
    live.call("stop_transport")


def test_detects_chordism_chords(live, rig, report):
    clip, notes = record(live, rig["rec"], float(BEATS))
    audio = livelog.audio_settings(live.call("ping")["live_version"])
    s = score_triads(notes, BEATS, TEMPO)
    share = s["right"] / BEATS

    text = "\n== NoteCap detecting Chordism's C major triads through %s (%.0f BPM%s) ==\n" % (
        INSTRUMENT, TEMPO, ", %d Hz, buffer %s" % (audio["sample_rate"], audio.get("output_buffer"))
        if audio.get("sample_rate") else "")
    text += "\n".join(s["lines"])
    text += "\nright: %d of %d beats (%.0f%%)" % (s["right"], BEATS, share * 100)
    if s["offsets_ms"]:
        text += "; detection latency: median %.0f ms, max %.0f ms" % (statistics.median(s["offsets_ms"]),
                                                                       max(s["offsets_ms"]))
    text += "\nsettings: %s" % rig["shown"]
    report(text)
    save_report("detection-chordism", {"shown": rig["shown"], "routing": rig["routing"], "audio": audio,
                                       "clip": clip, "right": s["right"], "beats": BEATS,
                                       "latencies_ms": s["offsets_ms"]})

    assert notes, "NoteCap sent nothing (is Drift sounding on the Synth track?)"
    assert share >= MIN_RIGHT, "only %d of %d beats detected correctly" % (s["right"], BEATS)
