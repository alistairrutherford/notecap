"""NoteCap in Live: listening through its sidechain.

    "NoteCap Chords" / "NoteCap Synth"   Chordism -> Drift, as in the detection test
    "NoteCap Side"      (audio, input: No Input)  [ NoteCap, Source: Sidechain ]
    "NoteCap SideRec"   (MIDI) MIDI From: NoteCap Side / NoteCap, armed

The Side track's own input is silent, so every chord it detects came through the sidechain.

The Live Object Model doesn't reach a device's Sidechain chooser (Device.audio_inputs is empty
for plug-ins and for Live's own Compressor in 12.4.6), but Live stores the choice in the set, so
route it in a copy of the saved set and open that:
    uv run python -m livebridge.als "path/to/set.als" --track "NoteCap Side" --source "NoteCap Synth"
    scripts/restart_live.sh "path/to/set sidechain.als"
(or pick NoteCap Synth / Post FX by hand in the NoteCap Side device header, and save).
Settings: as the detection test (NOTECAP_DETECT_TEMPO, NOTECAP_DETECT_BEATS, NOTECAP_DETECT_MIN).
"""

import os
import statistics

import pytest

from livebridge.client import BridgeError
from rig import NOTECAP, chordism_chain, chordism_pattern, detect_params, device, record, route_from_plugin, \
    save_report, score_triads, track

TEMPO = float(os.environ.get("NOTECAP_DETECT_TEMPO", "60"))
BEATS = int(os.environ.get("NOTECAP_DETECT_BEATS", "14"))
MIN_RIGHT = float(os.environ.get("NOTECAP_DETECT_MIN", "0.85"))

pytestmark = pytest.mark.live


@pytest.fixture(scope="module")
def rig(live):
    chain = chordism_chain(live, TEMPO)
    detect_params(live, chain["synth"], chain["notecap"])      # the Synth's own NoteCap stays quiet-ish
    side = track(live, "NoteCap Side", "audio", [(NOTECAP, "plugins")])
    try:
        live.call("set_input_routing", track=side, type="No Input")
    except BridgeError:
        pass   # older Live wording; the track isn't armed, so its input is unmonitored anyway
    nd = device(live, side, NOTECAP)
    shown = detect_params(live, side, nd, source="Sidechain")

    rec = track(live, "NoteCap SideRec", "midi")
    route_from_plugin(live, rec, side, NOTECAP)
    chordism_pattern(live, chain, True)
    yield dict(chain, side=side, rec=rec, shown=shown)
    chordism_pattern(live, chain, False)
    live.call("stop_transport")


def test_detects_through_sidechain(live, rig, report):
    clip, notes = record(live, rig["rec"], float(BEATS))
    s = score_triads(notes, BEATS, TEMPO)
    share = s["right"] / BEATS
    text = "\n== NoteCap via its sidechain (NoteCap Synth), %.0f BPM ==\n" % TEMPO
    text += "\n".join(s["lines"]) + "\nright: %d of %d beats (%.0f%%)" % (s["right"], BEATS, share * 100)
    if s["offsets_ms"]:
        text += "; latency median %.0f ms" % statistics.median(s["offsets_ms"])
    report(text)
    save_report("sidechain", {"shown": rig["shown"], "clip": clip,
                              "right": s["right"], "beats": BEATS})
    assert notes, ("nothing detected through the sidechain. Is it routed? The Live Object Model can't route it: "
                   "save the set, run `python -m livebridge.als SET --track 'NoteCap Side' --source 'NoteCap Synth'`, "
                   "and reopen the copy with scripts/restart_live.sh")
    assert share >= MIN_RIGHT, "only %d of %d beats right through the sidechain" % (s["right"], BEATS)
