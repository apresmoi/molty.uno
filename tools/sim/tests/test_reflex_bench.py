"""molty-sim reflex-bench (molty_sim/reflex_bench.py): the labelled set, scoring and the table, offline."""

import re

from molty_sim import reflex_bench as rb


def test_the_labelled_set():
    items = rb.load()
    assert len(items) == 40
    by = {r: sum(i.reaction == r for i in items) for r in rb.REACTIONS}
    assert all(n >= 6 for n in by.values()), by  # every reaction, neutral included
    assert {i.category for i in items} == set(rb.CATEGORIES)


def test_score_and_table():
    items = [rb.Item("a", "happy", "other"), rb.Item("b", "none", "request"), rb.Item("c", "sad", "other")]
    verdicts = [
        {"pick": "happy", "cat": "other", "ms": 200, "tokens": 600},
        {"pick": "curious", "cat": "request", "ms": 100, "tokens": 400},
        {"pick": "none", "cat": "question", "ms": 300, "tokens": 500, "error": "timeout"},
    ]
    s = rb.score("local", items, verdicts)
    assert (s.reaction_ok, s.false_fires, s.neutral, s.category_ok, s.errors) == (1, 1, 1, 2, 1)
    assert (s.pct(0.5), s.pct(0.9)) == (200, 300)
    t = rb.table([s, rb.score("keywords", items, [{"pick": "happy", "ms": 0}] * 3)], {"openai": "OPENAI_API_KEY not set"})
    assert "| local | 1/3 (33%) | 1/1 | 2/3 (67%) | 200 | 300 | 500 | 0.0000 | 1 |" in t
    assert "| keywords | 1/3 (33%) | 1/1 | — |" in t
    assert "| openai | skipped: OPENAI_API_KEY not set |" in t


def test_local_gate_and_prices_match_the_relay():
    src = (rb.RELAY_DIR / "src" / "core" / "reflex.ts").read_text()
    for lv in rb.LIVELINESS:
        line = re.search(rf"^  {lv}: \{{(.*)\}},$", src, re.M)
        assert line, lv
        gate = re.search(r"gate: \{ local: ([0-9.]+)", line.group(1))
        assert gate and float(gate.group(1)) == rb.LOCAL_GATE[lv]
        mild = re.search(r"mild: \{ none: ([0-9.]+), min: ([0-9.]+) \}", line.group(1))
        assert (None if not mild else (float(mild.group(1)), float(mild.group(2)))) == rb.MILD[lv]
    assert rb.gate_local({"pick": "sad", "conf": 0.2}) == {"pick": "none", "raw": "sad", "conf": 0.2}
    assert rb.gate_local({"pick": "sad", "conf": 0.5})["pick"] == "sad"
    assert rb.gate_local({"pick": "sad", "conf": 0.28}, "lively")["pick"] == "sad"
    # mild (the relay's reflexGate): none unsure, a reaction plausible → soft
    v = {"pick": "none", "conf": 0.58, "p": {"curious": 0.42, "none": 0.58}}
    assert rb.gate_local(v, "normal") == {**v, "pick": "curious", "soft": True, "raw": "none"}
    assert rb.gate_local(v, "calm")["pick"] == "none"
    assert rb.gate_local({**v, "cat": "request"}, "lively")["pick"] == "none"
