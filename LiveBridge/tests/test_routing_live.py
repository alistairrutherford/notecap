"""NoteCap in Live: routing and timing, with Test Chords (no audio needed).

Track "NoteCap Src" (audio) holds NoteCap with Test Chords on: it plays C, F, G, Am, one per
grid line, ignoring its input. Track "NoteCap Rec" (MIDI, Drift) records it through
MIDI From: NoteCap Src / NoteCap. Every beat must carry the right chord, exactly on the beat,
held until the next one.

Settings (environment variables):
    NOTECAP_PLUGIN       plug-in name in Live's browser     (default "NoteCap")
    NOTECAP_INSTRUMENT   instrument on the Rec track        (default "Drift")
    NOTECAP_BARS         bars to record                     (default 4)
    NOTECAP_TEMPO        BPM                                (default 120)
"""

import os

import pytest

from livebridge import livelog, timing
from rig import CLOSE, INSTRUMENT, NOTECAP, NOTECAP_CHOICES as NC, by_beat, device, prepare_song, record, route_from_plugin, \
    save_report, set_param, track

BARS = int(os.environ.get("NOTECAP_BARS", "4"))
TEMPO = float(os.environ.get("NOTECAP_TEMPO", "120"))
CYCLE = ["C", "F", "G", "Am"]

pytestmark = pytest.mark.live


@pytest.fixture(scope="module")
def rig(live):
    prepare_song(live, TEMPO)
    src = track(live, "NoteCap Src", "audio", [(NOTECAP, "plugins")])
    rec = track(live, "NoteCap Rec", "midi", [(INSTRUMENT, "instruments")])
    dev = device(live, src, NOTECAP)
    shown = {
        "Grid": set_param(live, src, dev, "Grid", "1/4", choices=NC["Grid"]),
        "Length": set_param(live, src, dev, "Length", "Hold", choices=NC["Length"]),
        "Voicing": set_param(live, src, dev, "Voicing", "Close", choices=NC["Voicing"]),
        "Octave": set_param(live, src, dev, "Octave", 3, (1, 5)),
        "MIDI Channel": set_param(live, src, dev, "MIDI Channel", 1, (1, 16)),
        "Test Chords": set_param(live, src, dev, "Test Chords", True),
    }
    yield {"src": src, "rec": rec, "device": dev, "shown": shown}
    set_param(live, src, dev, "Test Chords", False)
    live.call("stop_transport")


def test_test_chords_recorded_on_the_beat(live, rig, report):
    routing = route_from_plugin(live, rig["rec"], rig["src"], NOTECAP)
    beats = BARS * 4.0
    clip, notes = record(live, rig["rec"], beats)
    audio = livelog.audio_settings(live.call("ping")["live_version"])

    rows = timing.note_offsets(notes, TEMPO, grid=1.0, sample_rate=audio.get("sample_rate"))
    summary = timing.summarize(rows, audio.get("output_buffer"))
    got = by_beat(notes)
    lines, wrong, short = [], [], []
    for b in range(int(beats)):
        want = CLOSE[CYCLE[b % 4]]
        pitches = sorted(n["pitch"] for n in got.get(b, []))
        if pitches != want:
            wrong.append(b)
        # Hold: each chord lasts until the next grid line (the last one until the stop).
        if b < beats - 1 and any(abs(n["duration"] - 1.0) > 0.01 for n in got.get(b, [])):
            short.append((b, [n["duration"] for n in got.get(b, [])]))
        lines.append("beat %2d  want %-6s %-14s got %-14s %s" % (b, CYCLE[b % 4], want, pitches,
                                                                   "ok" if pitches == want else "WRONG"))

    text = timing.format_table("NoteCap Test Chords via MIDI From", rows, summary, TEMPO, audio)
    text += "\nplug-in shows: %s" % ", ".join("%s=%s" % kv for kv in rig["shown"].items())
    text += "\ninput: %s / %s\n" % (routing["current"]["type"], routing["current"]["channel"]) + "\n".join(lines)
    report(text)
    save_report("routing-test-chords", {"shown": rig["shown"], "routing": routing["current"], "audio": audio,
                                        "clip": clip, "rows": rows, "summary": summary, "wrong_beats": wrong})

    assert notes, "nothing recorded on the Rec track"
    assert not wrong, "beats with the wrong chord: %s" % wrong
    assert not short, "chords not held for one beat: %s" % short
    assert max(abs(r["offset_ms"]) for r in rows) < 0.1, "MIDI From route not sample-accurate"
