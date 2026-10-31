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
    s = rb.score("jev", items, verdicts)
    assert (s.reaction_ok, s.false_fires, s.neutral, s.category_ok, s.errors) == (1, 1, 1, 2, 1)
    assert (s.pct(0.5), s.pct(0.9)) == (200, 300)
    assert abs(s.cost_per_1000 - 500 * 0.042 / 1e6 * 1000) < 1e-12
    t = rb.table([s, rb.score("keywords", items, [{"pick": "happy", "ms": 0}] * 3)], {"openai": "OPENAI_API_KEY not set"})
    assert "| jev | 1/3 (33%) | 1/1 | 2/3 (67%) | 200 | 300 | 500 | 0.0210 | 1 |" in t
    assert "| keywords | 1/3 (33%) | 1/1 | — |" in t
    assert "| openai | skipped: OPENAI_API_KEY not set |" in t


def test_local_gate_and_prices_match_the_relay():
    src = (rb.RELAY_DIR / "src" / "core" / "reflex.ts").read_text()
    gate = re.search(r"gate: \{ local: ([0-9.]+)", src)
    assert gate and float(gate.group(1)) == rb.LOCAL_GATE
    price = re.search(r"PRICE_PER_MTOK[^=]*= \{ jev: ([0-9.]+), openai: ([0-9.]+) \}", src)
    assert price and (float(price.group(1)), float(price.group(2))) == (rb.PRICE_PER_MTOK["jev"], rb.PRICE_PER_MTOK["openai"])
    assert rb.gate_local({"pick": "sad", "conf": 0.2}) == {"pick": "none", "raw": "sad", "conf": 0.2}
    assert rb.gate_local({"pick": "sad", "conf": 0.5})["pick"] == "sad"
