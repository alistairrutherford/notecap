"""NoteCap in Live: settings survive (1) duplicating the track and (2) saving and reopening the set.

1. test_duplicate_keeps_state (automatic): track "NoteCap State" gets distinctive NoteCap
   settings; duplicate_track makes Live save NoteCap's state and load it into a new instance,
   whose settings must match. The copy is then deleted.

2. Save and reopen (two stages; the Live Object Model can't save a set):
       NOTECAP_STATE=prepare uv run pytest tests/test_state_live.py -k reopen
   sets the same settings plus "NoteCap StateRec" (MIDI From: NoteCap State / NoteCap) and Test
   chords on, and writes reports/state-expected.json. Then save the set (Cmd-S), and reopen it:
       scripts/restart_live.sh "path/to/the set.als"
       NOTECAP_STATE=verify uv run pytest tests/test_state_live.py -k reopen
   checks every NoteCap setting and the MIDI From routing, then records a bar to show the route works.
"""

import json
import os

import pytest

from rig import NOTECAP, NOTECAP_CHOICES as NC, REPORT_DIR, device, record, route_from_plugin, set_param, track

EXPECTED = os.path.join(REPORT_DIR, "state-expected.json")
STAGE = os.environ.get("NOTECAP_STATE", "")

pytestmark = pytest.mark.live


def _distinctive(live, t, d):
    """Settings far from the defaults, so a reload that drops them can't pass by accident."""
    set_param(live, t, d, "Source", "Sidechain", choices=NC["Source"])
    set_param(live, t, d, "Grid", "1/8T", choices=NC["Grid"])
    set_param(live, t, d, "Quantize Mode", "Next line", choices=NC["Quantize Mode"])
    set_param(live, t, d, "Length", "2 steps", choices=NC["Length"])
    set_param(live, t, d, "Voicing", "Open", choices=NC["Voicing"])
    set_param(live, t, d, "Velocity Mode", "Fixed", choices=NC["Velocity Mode"])
    set_param(live, t, d, "Octave", 2, (1, 5))
    set_param(live, t, d, "MIDI Channel", 5, (1, 16))
    set_param(live, t, d, "Velocity", 77, (1, 127))
    set_param(live, t, d, "Swing", 30, (0, 100))
    set_param(live, t, d, "Gate", -40, (-80, -20))
    set_param(live, t, d, "7th Chords", False)
    set_param(live, t, d, "Sus Chords", True)


def _displays(live, t, d):
    params = live.call("list_parameters", track=t, device=d)["parameters"]
    return {p["name"]: p["display"] for p in params if p["name"] not in ("Device On", "Bypass")}


@pytest.fixture(scope="module")
def state_track(live):
    live.call("stop_transport")
    t = track(live, "NoteCap State", "audio", [(NOTECAP, "plugins")])
    return t, device(live, t, NOTECAP)


def test_duplicate_keeps_state(live, state_track, report):
    t, d = state_track
    _distinctive(live, t, d)
    before = _displays(live, t, d)
    copy = live.call("duplicate_track", track=t)["index"]
    try:
        after = live.wait_until(lambda: (lambda x: x if x.get("Grid") else None)(
            _displays(live, copy, device(live, copy, NOTECAP))), 10, what="the copy's parameters")
    finally:
        live.call("delete_track", track=copy)
    diff = {k: (before[k], after.get(k)) for k in before if before[k] != after.get(k)}
    report("\n== NoteCap state through duplicate_track ==\n%d settings compared; differences: %s"
           % (len(before), diff or "none"))
    assert not diff, "settings changed in the duplicate: %s" % diff


def test_reopen(live, state_track, report):
    t, d = state_track
    if STAGE == "prepare":
        _distinctive(live, t, d)
        set_param(live, t, d, "Test Chords", True)
        rec = track(live, "NoteCap StateRec", "midi")
        routing = route_from_plugin(live, rec, t, NOTECAP)["current"]
        os.makedirs(REPORT_DIR, exist_ok=True)
        with open(EXPECTED, "w") as f:
            json.dump({"params": _displays(live, t, d), "routing": routing}, f, indent=2)
        report("\n== NoteCap state: prepared. Save the set (Cmd-S), reopen it, then run with "
               "NOTECAP_STATE=verify ==\nexpected: %s" % EXPECTED)
    elif STAGE == "verify":
        if not os.path.exists(EXPECTED):
            pytest.fail("no %s: run with NOTECAP_STATE=prepare first" % EXPECTED)
        with open(EXPECTED) as f:
            expected = json.load(f)
        got = _displays(live, t, d)
        diff = {k: (v, got.get(k)) for k, v in expected["params"].items() if got.get(k) != v}
        rec = track(live, "NoteCap StateRec", "midi")
        routing = live.call("get_input_routing", track=rec)["current"]
        _, notes = record(live, rec, 4.0)
        set_param(live, t, d, "Test Chords", False)
        report("\n== NoteCap state after reopening the set ==\nsettings: %d checked, differences: %s\n"
               "MIDI From: %s / %s (expected %s / %s); %d notes recorded in 4 beats"
               % (len(expected["params"]), diff or "none", routing["type"], routing["channel"],
                  expected["routing"]["type"], expected["routing"]["channel"], len(notes)))
        assert not diff, "settings lost on reopen: %s" % diff
        assert routing == expected["routing"], "MIDI From routing lost on reopen"
        assert notes, "the reopened MIDI From route recorded nothing"
    else:
        pytest.skip("two-stage test: run with NOTECAP_STATE=prepare, save and reopen the set, "
                    "then NOTECAP_STATE=verify (see the docstring)")
