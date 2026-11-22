"""The talk baseline: labelled turns, the facts agent, the scoreboard, the judge's seam."""

import asyncio
from pathlib import Path

import numpy as np
import pytest

from molty_sim import facts, judge, score
from molty_sim.scenario import ScenarioError, load, parse

SCN = Path(__file__).resolve().parents[1] / "scenarios" / "talk-baseline.yaml"


def test_baseline_scenario_parses_with_labels_and_sheet():
    sc = load(SCN)
    assert 8 <= len(sc.turns) <= 12
    assert {t.label for t in sc.turns} == {"alone", "hand_off", "refuse"}
    assert all(t.truth in sc.facts for t in sc.turns)
    assert sum(1 for t in sc.turns if t.tap) == 1


@pytest.mark.parametrize("bad", [
    {"turns": [{"user": "hi", "label": "alone", "truth": "x"}]},  # no sheet
    {"facts": {"unknown": {"say": "?"}, "idle": {"say": "."}}, "turns": [{"user": "hi", "label": "alone", "truth": "nope"}]},
    {"facts": {"unknown": {"say": "?"}}, "turns": [{"user": "hi"}]},  # idle missing
    {"facts": {"unknown": {"say": "?"}, "idle": {"say": "."}}, "turns": [{"user": "hi", "label": "maybe", "truth": "idle"}]},
    {"turns": [{"user": "hi", "tap": {"from_s": 1, "to_s": 2}}]},  # a tap needs a say before it
])
def test_label_keys_rejected(bad):
    with pytest.raises(ScenarioError):
        parse(bad)


def test_every_scenario_turn_is_answered_by_its_truth():
    """The facts agent picks each turn's truth from its exact words (the transcript of a clean run)."""
    sc = load(SCN)
    for t in sc.turns:
        assert facts.pick(sc.facts, t.text) == t.truth, t.id


def test_correction_wins_and_unknown_and_idle():
    sheet = load(SCN).facts
    assert facts.pick(sheet, "tests on main, wait, actually the other branch") == "tests_lanes"
    assert facts.pick(sheet, "what is the capital of France") == "unknown"
    assert facts.pick(sheet, "") == "idle"
    assert facts.pick(sheet, "um") == "idle"


class FakeRest:
    device = "molty-simtest"

    def __init__(self):
        self.calls = []

    async def call(self, method, path, body=None, timeout=60):
        self.calls.append(body)
        if body["name"] == "keep":
            return 200, {"ok": True, "result": {"keep": 3, "said": "Okay, the tests,"}}
        return 200, {"ok": True, "result": {"text": "One sec."}}


class T:
    def __init__(self, i):
        self.id = i


def run_agent(heards, seed=3, **kw):
    sheet = load(SCN).facts
    slept = []

    async def sleep(s):
        slept.append(s)

    a = facts.FactAgent(sheet, FakeRest(), seed, sleep=sleep, **kw)

    async def go():
        out = []
        for k, h in enumerate(heards):
            out.append(await a.reply(None, h, [{"turn": T(f"t{k}"), "res": {}}]))
        return out
    return asyncio.run(go()), slept, a


def test_facts_agent_only_says_lines_of_its_sheet():
    sheet = load(SCN).facts
    lines = {f["say"] for f in sheet.values()}
    heards = ["did the tests pass", "who won the match last night", "tell me a joke", "is it raining", "", "the version please",
              "what's my password", "summarise my mail"] * 4
    said, _, _ = run_agent(heards, seed=11)
    assert all(s is None or s in lines for s in said)  # None: a turn it never answers
    assert any(s == sheet["unknown"]["say"] for s in said)


def test_think_times_seeded_and_spread():
    a = [facts.think_s(5, f"t{i}") for i in range(400)]
    assert a == [facts.think_s(5, f"t{i}") for i in range(400)]
    assert a != [facts.think_s(6, f"t{i}") for i in range(400)]
    got = [x for x in a if x is not None]
    assert 3.5 <= float(np.median(got)) <= 5.0
    assert 0.02 <= a.count(None) / len(a) <= 0.12  # some never
    assert 0.05 <= sum(1 for x in got if x >= 11) / len(a) <= 0.2  # some at 12 s
    assert facts.tap_s(5, "x", 1.5, 3.0) == facts.tap_s(5, "x", 1.5, 3.0)
    assert 1.5 <= facts.tap_s(5, "x", 1.5, 3.0) <= 3.0


def test_a_seed_makes_a_run_repeat_exactly():
    heards = [t.text for t in load(SCN).turns]
    one = run_agent(heards, seed=7)
    two = run_agent(heards, seed=7)
    assert one[0] == two[0] and one[1] == two[1]
    assert run_agent(heards, seed=8)[1] != one[1]


def test_quick_before_thinking_and_keep_first_words():
    _, _, a = run_agent(["did the tests pass"], seed=1, quick=True)
    assert a.rest.calls[0] == {"name": "quick", "args": {"id": facts.QUICK_ID}}
    sheet = load(SCN).facts

    async def go():
        a = facts.FactAgent(sheet, FakeRest(), 1, keep=True, sleep=lambda s: asyncio.sleep(0))
        return await a.reply(None, "did the firmware tests pass", [{"turn": T("tests"), "res": {"opener": "Okay, the tests, let me look."}}]), a
    text, a = asyncio.run(go())
    assert a.rest.calls[0] == {"name": "keep", "args": {"words": 1}}  # "Okay," ends the first clause
    assert facts.clause_words("no clause end here") == 4
    if text is not None:
        assert text == "Okay, the tests, " + sheet["tests"]["say"] and a.last["answer"] == sheet["tests"]["say"]


# ---------------------------------------------------------------- the scoreboard

def sf(**kw):
    base = dict(label="hand_off", truth="tests", pick="tests", think_s=4.0, eos_at=10.0, human=[(8.0, 10.0)],
                voices=[score.Voice("filler", "Hmm.", [(12.5, 13.0)]), score.Voice("answer", "Two failed in OTA, 40 passed.", [(14.6, 16.0)])],
                ack_at=10.7, answer="Two failed in OTA, 40 passed.")
    base.update(kw)
    return score.ScoreFacts(**base)


def test_first_sound_grounded_and_lead_in():
    m = score.score_turn(sf())
    assert m["first_sound_ms"] == 2500 and m["first_kind"] == "filler"
    assert m["lead_in_ms"] == 2500 and m["chirp_ms"] == 700
    assert m["grounded_ms"] == 4600 and m["grounded_less_think_ms"] == 600
    assert m["talk_over_ms"] == 0 and m["passed"] and m["fidelity"] is True


def test_wrong_or_missing_answer_has_no_grounded_time():
    m = score.score_turn(sf(pick="unknown"))
    assert m["correct"] is False and "grounded_ms" not in m and m["fails"] == ["wrong"]
    m = score.score_turn(sf(pick=None, voices=[score.Voice("filler", "Hmm.", [(12.5, 13.0)])]))
    assert not m["answered"] and m["fails"] == ["no_answer"]


def test_cut_in_a_pause_is_a_takeover_and_talk_over_counts():
    # cut at 9.0 while the human still speaks until 11.0; Molty's filler at 10.5..11.5 overlaps 0.5 s
    m = score.score_turn(sf(cut=True, pauses=True, human=[(7.0, 8.5), (9.5, 11.0)], eos_at=11.0,
                            voices=[score.Voice("filler", "Hmm.", [(10.5, 11.5)])]))
    assert m["pause_takeover"] is True and m["talk_over_ms"] == 500 and m["first_sound_ms"] == -500
    assert "cut" in m["fails"] and "talk_over" in m["fails"]


def test_tap_recovery():
    m = score.score_turn(sf(tap_at=20.0, tapped=[(18.0, 25.0)]))
    assert m["tap_stop_ms"] == 5000 and m["tap_recovered"] is False and "tap" in m["fails"]
    assert score.score_turn(sf(tap_at=20.0, tapped=[(18.0, 20.2)]))["tap_recovered"] is True


def test_voiced_spans_from_audio():
    x = np.zeros(16000 * 2, np.int16)
    x[8000:16000] = (np.sin(np.arange(8000) / 3) * 8000).astype(np.int16)
    sp = score.voiced_spans(x, 100.0)
    assert len(sp) == 1 and abs(sp[0][0] - 100.5) < 0.03 and abs(sp[0][1] - 101.0) < 0.03


def test_board_counts_and_text():
    rows = [{"id": "a", **score.score_turn(sf())}, {"id": "a", **score.score_turn(sf(pick="unknown"))},
            {"id": "b", **score.score_turn(sf(cut=True, pauses=True, label="alone"))}]
    rows[2]["false_cutoff"] = True
    b = score.board(rows)
    assert b["grounded_turns"] == {"n": 2, "correct": 1, "wrong": 1, "unanswered": 0}
    assert b["by_turn"]["a"]["passed"] == "1/2" and b["pause_takeover"]["hit"] == 1 and b["false_cutoffs"]["hit"] == 1
    assert "agent_free_turns" in b["not_applicable_today"]
    assert "first sound" in score.board_text(b)


# ---------------------------------------------------------------- the judge's seam

def test_judge_parses_votes_from_any_command(tmp_path):
    sheet = load(SCN).facts
    rows = [{"i": 0, "id": "tests", "said": "Hey", "label": "hand_off", "truth": "tests", "text": "did the tests pass", "heard": "did the tests pass",
             "reason": "turn_end", "voices": [{"kind": "answer", "text": sheet["tests"]["say"], "at_s": 4.2}]}]
    p = judge.prompt(rows, sheet)
    assert "Fact sheet" in p and "+4.2 s Molty (answer)" in p
    out = judge.judge("echo 'noise {\"invented\": 0, \"answered_right\": 1, \"smooth\": 4, \"one_voice\": 5, \"worst\": \"x\"}'", rows, sheet, votes=3)
    assert out["votes"] == 3 and out["invented"] == 0 and out["smooth"] == 4
    bad = judge.judge("echo nothing", rows, sheet, votes=2)
    assert bad["votes"] == 0 and bad["invented"] is None and len(bad["errors"]) == 2
    assert judge.merge([out, out])["answered_right"] == 2


def test_fidelity_catches_a_lost_number():
    m = score.score_turn(sf(voices=[score.Voice("answer", "Two failed in OTA, forty passed.", [(14.6, 16.0)])]))
    assert m["fidelity"] is False
