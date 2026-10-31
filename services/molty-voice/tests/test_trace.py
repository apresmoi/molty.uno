"""The box's turn trace (stt.trace) and listen hints (stt.hint): molty_voice/trace.py and its bridge hooks."""

import json

import numpy as np

from molty_voice.kyutai import FRAME_S, PausePredictor
from molty_voice.trace import BUDGET_BYTES, HINT_GAP_S, MAX_PAUSE_BYTES, MAX_SPANS, MAX_WORDS, SttTrace

from test_turn_end import VadMoshi, run_listen_all

TALK, PAUSE = 0.1, 0.9


def drive(steps, hold_s=1.2, no_speech_s=None):
    """Steps of (prs2, [(word, start, stop)...]) through PausePredictor + SttTrace as SttSession does.
    Returns (trace, fired step indices, hints [(step, kind)])."""
    p = PausePredictor(hold_s=hold_s)
    tr = SttTrace(hold_s=hold_s)
    fired, hints = [], []
    for i, (prs, words) in enumerate(steps):
        for w, a, b in words or []:
            p.word(w)
            h = tr.word(w, a)
            tr.end_word(b)
            if h:
                hints.append((i, h))
        turn = p.update(prs)
        ns = bool(not turn and no_speech_s and p.no_speech(no_speech_s))
        h = tr.step(p, prs, turn, ns)
        if turn:
            fired.append(i)
        if h:
            hints.append((i, h))
    return tr, fired, hints


def talk_pause(n_talk, words, n_pause, n_more=0):
    steps = [[TALK, None] for _ in range(n_talk)] + [[PAUSE, None] for _ in range(n_pause)] + [[TALK, None] for _ in range(n_more)]
    for k, w in enumerate(words):
        at = 12 + (n_talk - 13) * (k + 1) // len(words)
        steps[at][1] = (steps[at][1] or []) + [(w, round(at * FRAME_S - 0.3, 2), round(at * FRAME_S - 0.05, 2))]
    return steps


def test_punct_rule_words_and_activity():
    tr, fired, hints = drive(talk_pause(40, ["so", "close", "to", "me."], 10))
    assert fired == [40]
    t = tr.to_json(final_ms=212)
    assert t["end"] == {"at": round(41 * FRAME_S, 2), "rule": "punct", "word": "me.", "held_s": 0.08, "ema": t["end"]["ema"]}
    assert t["end"]["ema"] > 0.6
    assert [w[0] for w in t["words"]] == ["so", "close", "to", "me."]
    assert all(w[1] is not None and w[2] is not None and w[2] > w[1] for w in t["words"])
    # activity: steps 12.. are below 0.3; the span starts at the first of the 3 steps, ends at the pause
    assert t["act"] == [[round(12 * FRAME_S, 2), round(40 * FRAME_S, 2)]]
    assert t["final_ms"] == 212 and t["step_ms"] == 80 and t["skip"] == 12 and t["thr"] == 0.6 and t["hold_s"] == 1.2
    assert len(t["pause"]) == 2 * 50 and t["pause_every"] == 1
    assert t["pause"][:2] == "ff"  # the EMA starts at 1.0
    assert "ns" not in t
    assert hints == []  # a finished sentence ends at once: no pause hint before it


def test_hold_rule_and_a_pause_hint_on_the_mid_sentence_pause():
    tr, fired, hints = drive(talk_pause(40, ["at", "two", "meters", "from"], 20))
    assert fired == [40 + 14]  # 15 paused steps = the 1.2 s hold
    end = tr.to_json()["end"]
    assert end["rule"] == "hold" and end["word"] == "from" and end["held_s"] == 1.2
    assert hints == [(40, "pause")]  # the first paused step, mid-turn


def test_unmute_rule_with_hold_0():
    tr, fired, hints = drive(talk_pause(40, ["at", "from"], 6), hold_s=0)
    assert fired == [40] and tr.to_json()["end"]["rule"] == "unmute" and hints == []


def test_rule_attribution_never_changes_the_decision():
    for hold in (0, 0.4, 1.2):
        steps = talk_pause(40, ["at", "from"], 30, 5)
        p = PausePredictor(hold_s=hold)
        plain = []
        for i, (prs, words) in enumerate(steps):
            for w, _, _ in words or []:
                p.word(w)
            if p.update(prs):
                plain.append(i)
        assert drive(steps, hold_s=hold)[1] == plain


def test_no_speech_decision():
    tr, fired, _ = drive([[0.95, None]] * 40, no_speech_s=1.0)
    t = tr.to_json()
    assert fired == [] and "end" not in t and t["words"] == [] and t["act"] == []
    assert t["ns"]["at"] == round(13 * FRAME_S, 2) and t["ns"]["quiet_s"] == round(13 * FRAME_S, 2)
    assert t["ns"]["ema"] > 0.9 and "no voice activity" in t["ns"]["why"]


def test_question_hint_and_the_rate_limit():
    steps = [[TALK, None] for _ in range(60)]
    steps[20][1] = [("right?", 1.0, 1.3)]
    steps[25][1] = [("you?", 1.4, 1.7)]  # 0.4 s later: rate-limited
    steps[45][1] = [("ok?", 3.0, 3.3)]   # 2 s after the first: allowed
    _, _, hints = drive(steps)
    assert hints == [(20, "question"), (45, "question")]
    gaps = [b[0] - a[0] for a, b in zip(hints, hints[1:])]
    assert all(g * FRAME_S >= HINT_GAP_S for g in gaps)
    # hesitations and ellipses are no question
    _, _, h2 = drive([[TALK, [("uh?", 0.5, 0.6)]]] + [[TALK, None]] * 20)
    assert h2 == []


def test_sizes_are_bounded():
    steps = []
    for k in range(3750):  # 300 s: an unlimited listen
        steps.append([TALK if (k // 10) % 2 else PAUSE, [(f"word{k}-longer-than-twenty-four-chars", k * FRAME_S, k * FRAME_S + 0.05)] if k % 7 == 0 else None])
    tr, _, _ = drive(steps, hold_s=100)
    t = tr.to_json(final_ms=1)
    assert len(t["pause"]) <= 2 * MAX_PAUSE_BYTES and t["pause_every"] == 13
    assert len(t["words"]) <= MAX_WORDS and t["words_n"] == 536 and all(len(w[0]) <= 24 for w in t["words"])
    assert len(t["act"]) == MAX_SPANS
    assert t["words"][-1][0].startswith("word3745")  # the newest kept: the end of the turn matters most
    assert len(json.dumps(t, separators=(",", ":"))) <= BUDGET_BYTES


# ---- through the bridge ----


def test_bridge_sends_the_trace_before_turn_end_and_before_final():
    msgs = run_listen_all([True] * 8 + [False] * 6)
    kinds = [m["t"] for m in msgs if m["t"] != "stt.partial"]
    assert kinds == ["stt.trace", "stt.turn_end", "stt.trace", "stt.final"]
    first, last = msgs[[m["t"] for m in msgs].index("stt.trace")], [m for m in msgs if m["t"] == "stt.trace"][-1]
    assert first["id"] == "s1" and first["trace"]["end"]["rule"] == "punct" and first["trace"]["end"]["word"] == "hi."
    assert "final_ms" not in first["trace"] and isinstance(last["trace"]["final_ms"], int)
    assert [w[0] for w in last["trace"]["words"]] == ["hi."] and last["trace"]["words"][0][1] == 0.1
    assert last["trace"]["act"] and last["trace"]["v"] == 1


def test_bridge_no_speech_trace():
    msgs = [m for m in run_listen_all([False] * 14, {"no_speech_ms": 1000}) if m["t"] != "stt.partial"]
    assert [m["t"] for m in msgs] == ["stt.trace", "stt.turn_end", "stt.trace", "stt.final"]
    assert msgs[1]["reason"] == "no_speech"
    assert msgs[0]["trace"]["ns"]["quiet_s"] >= 1.0 and "end" not in msgs[0]["trace"]


def test_bridge_hints_a_mid_sentence_pause_and_a_question():
    msgs = run_listen_all([True] * 8 + [False] * 4, moshi=VadMoshi(word="hi"))
    assert [m for m in msgs if m["t"] == "stt.hint"] == [{"t": "stt.hint", "id": "s1", "k": "pause"}]
    assert [m["t"] for m in msgs if m["t"] not in ("stt.partial", "stt.hint")] == ["stt.trace", "stt.final"]
    msgs = run_listen_all([True] * 8 + [False] * 6, moshi=VadMoshi(word="you?"))
    assert [m["k"] for m in msgs if m["t"] == "stt.hint"] == ["question"]
    i = [m["t"] for m in msgs]
    assert i.index("stt.hint") < i.index("stt.turn_end")


def test_one_pause_hint_per_pause_not_per_paused_step():
    # a 2.4 s pause (hold not reached: hold 5 s), speech again, a second pause: one hint each
    steps = talk_pause(40, ["at", "from"], 30, 25) + [[PAUSE, None] for _ in range(10)]
    _, fired, hints = drive(steps, hold_s=5)
    assert fired == [] and hints == [(40, "pause"), (95, "pause")]


# ---- best effort: tracing never costs a turn ----


def decisions(msgs):
    return [m for m in msgs if m["t"] not in ("stt.partial", "stt.trace", "stt.hint")]


def test_a_throwing_observer_never_changes_turn_end_or_final(monkeypatch):
    want = decisions(run_listen_all([True] * 8 + [False] * 6))

    def boom(*a, **k):
        raise RuntimeError("observer bug")

    for what in ("step", "word"):
        with monkeypatch.context() as m:
            m.setattr(SttTrace, what, boom)
            msgs = run_listen_all([True] * 8 + [False] * 6)
        assert decisions(msgs) == want, what
        assert [d["t"] for d in want] == ["stt.turn_end", "stt.final"] and want[1]["text"] == "hi."


def test_a_throwing_trace_serializer_sends_no_trace_but_every_decision(monkeypatch):
    def boom(*a, **k):
        raise ValueError("cannot serialize")

    monkeypatch.setattr(SttTrace, "to_json", boom)
    msgs = run_listen_all([True] * 8 + [False] * 6)
    assert [m["t"] for m in msgs if m["t"] != "stt.partial"] == ["stt.turn_end", "stt.final"]
    assert msgs[-1]["text"] == "hi."
    msgs = run_listen_all([False] * 14, {"no_speech_ms": 1000})
    assert [m["t"] for m in msgs if m["t"] != "stt.partial"] == ["stt.turn_end", "stt.final"]
    assert msgs[0]["reason"] == "no_speech"


def test_a_throwing_hint_callback_never_breaks_the_session():
    import asyncio

    from websockets.asyncio.server import serve

    from molty_voice.kyutai import SttSession

    async def main():
        moshi = VadMoshi(word="you?")
        ends, hints = [], []

        async def turn_end():
            ends.append(True)

        async def boom(k):
            hints.append(k)
            raise RuntimeError("relay socket gone")

        async with serve(moshi.handler, "127.0.0.1", 0) as ms:
            port = ms.sockets[0].getsockname()[1]
            s = SttSession(f"ws://127.0.0.1:{port}/api/asr-streaming", "k", on_turn_end=turn_end, observer=SttTrace(), on_hint=boom)
            await s.open()
            loud = (np.full(3200, 6000, dtype="<i2")).tobytes()
            quiet = np.zeros(3200, dtype="<i2").tobytes()
            for pcm in [loud] * 8 + [quiet] * 6:
                await s.feed(pcm)
                await asyncio.sleep(0.01)
            await asyncio.sleep(0.2)
            text = await s.finish()
            await s.close()
        return text, ends, hints

    text, ends, hints = asyncio.run(main())
    assert text == "you?" and ends == [True] and hints == ["question"]  # the hint failed once; the turn went on
