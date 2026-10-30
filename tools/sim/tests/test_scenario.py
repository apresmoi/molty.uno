from pathlib import Path

import pytest

from molty_sim import scenario
from molty_sim.scenario import ScenarioError, parse, parse_markup

DEFAULT = Path(__file__).resolve().parents[1] / "scenarios" / "default.yaml"


def test_markup_words_and_pauses():
    m = parse_markup("I was thinking {pause 0.7} about the router, um {pause 0.5} and")
    assert m.words == ("I", "was", "thinking", "about", "the", "router,", "um", "and")
    assert m.pauses == ((3, 0.7), (7, 0.5))
    assert m.text == "I was thinking about the router, um and"


def test_markup_leading_trailing_and_adjacent_pauses():
    m = parse_markup("{pause 1} hi {pause 0.5}{pause 0.25}")
    assert m.words == ("hi",)
    assert m.pauses == ((0, 1.0), (1, 0.75))


@pytest.mark.parametrize("bad", ["hi {pause} there", "hi {pause 0} x", "hi {pause 31} x", "hi {wait 1} x"])
def test_markup_rejects(bad):
    with pytest.raises(ScenarioError):
        parse_markup(bad)


def test_parse_defaults_inherit_and_override():
    sc = parse({"defaults": {"lead_s": 1.0, "voices": ["a.wav", "b.wav"]},
                "turns": [{"user": "hello"}, {"user": "there", "lead_s": 0.2, "voice": "c.wav"}, {"silence_s": 5}]})
    t0, t1, t2 = sc.turns
    assert (t0.lead_s, t0.voice, t0.expect, t0.id) == (1.0, "a.wav", "turn_end", "t01")
    assert (t1.lead_s, t1.voice) == (0.2, "c.wav")
    assert (t2.kind, t2.expect, t2.text) == ("silence", "no_speech", "")


@pytest.mark.parametrize("turn", [
    {"user": "a", "silence_s": 3},  # two sources
    {},  # none
    {"user": "a", "bogus": 1},
    {"user": "a", "expect": "maybe"},
    {"user": "a", "listen_ms": 40000},
    {"user": "a", "id": True},  # YAML `yes`
    {"user": "a", "noise": {"kind": "rain"}},
])
def test_parse_rejects(turn):
    with pytest.raises(ScenarioError):
        parse({"turns": [turn]})


def test_llm_turn_is_reserved_not_rejected():
    sc = parse({"turns": [{"user": {"llm": {"persona": "grumpy"}}}]})
    assert sc.turns[0].kind == "llm"


def test_default_suite_loads():
    sc = scenario.load(DEFAULT)
    assert 18 <= len(sc.turns) <= 24
    ids = [t.id for t in sc.turns]
    assert "yes" in ids and "no" in ids
    assert any(t.listen_ms == 0 for t in sc.turns)  # one unlimited turn
    assert sum(t.expect == "no_speech" for t in sc.turns) >= 1
    assert any("2 meters from" in t.text for t in sc.turns) and any(t.text.startswith("Yeah, I think that") for t in sc.turns)
