"""NoteCap in Live: Live's Record Quantization snaps late chords onto the grid.

Chordism's chords land exactly on the beat, and NoteCap (Grid 1/4, Nearest) knows each one
~125 ms later, after its grid line has passed, so it sends it at once: late. Recorded twice
into "NoteCap Detect":
    1. Record Quantization off  -> chords start ~125 ms after the beat (late, as designed);
    2. Record Quantization 1/4  -> the same chords start exactly on the beat.
Settings: NOTECAP_DETECT_TEMPO (default 60), NOTECAP_DETECT_BEATS (14), NOTECAP_DETECT_MIN (0.85).
"""

import os
import statistics

import pytest

from rig import NOTECAP, chordism_chain, chordism_pattern, detect_params, record, route_from_plugin, save_report, \
    score_triads, track

TEMPO = float(os.environ.get("NOTECAP_DETECT_TEMPO", "60"))
BEATS = int(os.environ.get("NOTECAP_DETECT_BEATS", "14"))
MIN_RIGHT = float(os.environ.get("NOTECAP_DETECT_MIN", "0.85"))

pytestmark = pytest.mark.live


@pytest.fixture(scope="module")
def rig(live):
    chain = chordism_chain(live, TEMPO)
    shown = detect_params(live, chain["synth"], chain["notecap"], grid="1/4", mode="Nearest")
    rec = track(live, "NoteCap Detect", "midi")
    route_from_plugin(live, rec, chain["synth"], NOTECAP)
    chordism_pattern(live, chain, True)
    yield dict(chain, rec=rec, shown=shown)
    chordism_pattern(live, chain, False)
    live.call("set_song_properties", midi_recording_quantization="none")
    detect_params(live, chain["synth"], chain["notecap"])   # back to Grid Off for the detection test
    live.call("stop_transport")


def _take(live, rig, quantization):
    live.call("set_song_properties", midi_recording_quantization=quantization)
    clip, notes = record(live, rig["rec"], float(BEATS))
    return clip, score_triads(notes, BEATS, TEMPO)


def test_record_quantization_snaps_late_chords(live, rig, report):
    clip_off, off = _take(live, rig, "none")
    clip_q, q = _take(live, rig, "rec_q_quarter")

    late = statistics.median(off["offsets_ms"]) if off["offsets_ms"] else None
    worst_q = max((abs(x) for x in q["offsets_ms"]), default=None)
    text = "\n== Record Quantization and NoteCap's late chords (Grid 1/4, Nearest, %.0f BPM) ==\n" % TEMPO
    text += "-- Record Quantization off --\n" + "\n".join(off["lines"])
    text += "\n-- Record Quantization 1/4 --\n" + "\n".join(q["lines"])
    text += "\noff: %d/%d right, median start %s after the beat" % (
        off["right"], BEATS, "%.0f ms" % late if late is not None else "-")
    text += "\n1/4: %d/%d right, worst start %s from the beat" % (
        q["right"], BEATS, "%.3f ms" % worst_q if worst_q is not None else "-")
    report(text)
    save_report("record-quantize", {"shown": rig["shown"], "off": off, "quarter": q,
                                    "clip_off": clip_off, "clip_quarter": clip_q})

    assert off["right"] / BEATS >= MIN_RIGHT and q["right"] / BEATS >= MIN_RIGHT
    assert late is not None and late > 50, "expected NoteCap's chords to arrive late without quantization"
    assert worst_q is not None and worst_q < 0.1, "Record Quantization 1/4 left chords off the beat"
