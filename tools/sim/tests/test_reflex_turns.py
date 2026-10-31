"""molty-sim reflex-turns (molty_sim/reflex_turns.py): the turn set, the summary and the table offline,
and one fast end-to-end run of the relay's own ReflexSession with the keywords engine (no key)."""

import shutil

import pytest

from molty_sim import reflex_turns as rt


def test_the_turn_set():
    turns = rt.load()
    by = {k: [t for t in turns if t.kind == k] for k in rt.KINDS}
    assert all(len(v) >= 4 for v in by.values()), {k: len(v) for k, v in by.items()}
    assert any(t.id == "whats-up" for t in by["react"])  # Juan's real short turn
    assert all(len(t.text.split()) >= 30 for t in by["explain"])  # long, like Juan's
    assert all(t.wpm == 150 and t.end_ms == 600 for t in turns)


def test_config_parse():
    c = rt.parse_config("after=jev:lively")
    assert (c.label, c.engine, c.liveliness, c.relay_dir) == ("after", "jev", "lively", rt.RELAY_DIR)
    c = rt.parse_config("before=jev@/x/services/relay")
    assert (c.engine, c.liveliness, str(c.relay_dir)) == ("jev", None, "/x/services/relay")
    with pytest.raises(ValueError):
        rt.parse_config("jev")


def test_summary_and_table():
    results = [
        {"id": "a", "kind": "react", "words": 7, "speech_ms": 2400, "fires": [{"at": 900, "pick": "curious", "soft": True}, {"at": 2500, "pick": "happy"}],
         "decisions": [{"n": 3, "fired": "ok"}, {"n": 6, "fired": "ok"}, {"n": 7, "fired": "closed"}]},
        {"id": "b", "kind": "react", "words": 5, "speech_ms": 1600, "fires": [], "decisions": [{"n": 3, "fired": "none"}]},
        {"id": "c", "kind": "explain", "words": 40, "speech_ms": 16000, "fires": [{"at": 4000, "pick": "curious"}], "decisions": [{"n": 3, "fired": "ok"}]},
        {"id": "d", "kind": "neutral", "words": 6, "speech_ms": 2000, "fires": [{"at": 1000, "pick": "curious"}], "decisions": [{"n": 3, "fired": "ok"}]},
    ]
    row = rt.summarize("x", results)
    assert (row.react_fires, row.react_hit, row.first_ms, row.closed) == (1.0, "1/2", 900, 1)
    assert (row.explain_fires, row.explain_gap_s, row.neutral_false) == (1.0, 12.0, "1 on 1/1")
    assert row.calls == 1.5
    t = rt.table([row])
    assert "| x | 1.0 | 1/2 | 900 | 1 | 1.0 | 12.0 | 1 on 1/1 | 1.5 |" in t
    assert "curious~@900, happy@2500" in rt.detail("x", results)


@pytest.mark.skipif(shutil.which("node") is None, reason="node not installed")
def test_a_fast_run_through_the_relay_session():
    # 1200 wpm (50 ms a word): the real tool and the real ReflexSession, in a second
    turns = [rt.Turn("hi", "react", "oh my god I just got the job and it is amazing", 1200, 100), rt.Turn("milk", "neutral", "I need to buy milk and eggs today", 1200, 100)]
    res = {r["id"]: r for r in rt.run_config(rt.Config("k", "keywords", "lively"), turns)}
    assert [d["n"] for d in res["hi"]["decisions"]] == [3, 6, 9, 12]  # lively: three words, then every three
    assert res["hi"]["fires"][0]["pick"] == "surprised" and res["hi"]["fires"][0]["at"] < 200
    assert res["milk"]["fires"] == []
