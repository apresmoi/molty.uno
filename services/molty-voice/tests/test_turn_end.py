"""Semantic end of turn: Unmute's pause rule (PausePredictor) and stt.turn_end through the bridge."""

import asyncio
import json

import msgpack
import numpy as np
from websockets.asyncio.server import serve

from molty_voice import framing
from molty_voice.bridge import Bridge
from molty_voice.config import Config
from molty_voice.kyutai import FRAME_S, PausePredictor, sentence_complete

DEVICE = "molty-k7m2q9xa4c"
TOKEN = DEVICE + "." + "A" * 43


def feed(p: PausePredictor, values) -> list[int]:
    """Indices (0-based steps) at which update() fired."""
    return [i for i, v in enumerate(values) if p.update(v)]


def test_fires_once_after_words_when_the_pause_estimate_rises():
    p = PausePredictor()
    p.word("done.")
    fired = feed(p, [0.0] * 20 + [0.95] * 10 + [0.0] * 5 + [0.95] * 10)
    assert fired == [20]  # the first paused step after speech, and never again


def test_never_fires_on_silence_alone():
    p = PausePredictor()
    assert feed(p, [0.95] * 100) == []  # no word: Molty's own no_speech handles an empty listen


def test_ignores_the_first_12_steps():
    p = PausePredictor()
    p.word("done.")
    assert feed(p, [0.95] * 13) == [12]  # steps 0..11 are skipped, step 12 counts


def test_skip_steps_is_tunable():
    p = PausePredictor(skip_steps=4)
    p.word("done.")
    assert feed(p, [0.95] * 6) == [4]
    q = PausePredictor(skip_steps=0)
    q.word("done.")
    assert feed(q, [0.95]) == [0]


def test_threshold_and_off_switch():
    p = PausePredictor(threshold=0.97)
    p.word("done.")
    assert feed(p, [0.0] * 12 + [0.95] * 20) == []  # never above 0.97
    off = PausePredictor(threshold=0)
    off.word("done.")
    assert feed(off, [0.0] * 12 + [0.99] * 20) == []


def test_ema_matches_unmute_constants():
    p = PausePredictor()
    p.word("done.")
    feed(p, [0.0] * 12)  # skipped: the value is still the initial 1.0
    assert p.value == 1.0
    p.update(0.0)
    # half-life 0.01 s over an 80 ms step: alpha = 1 - 2**-8
    assert abs(p.value - 2 ** -8) < 1e-9


# ---- mid-sentence hold: recorded-style traces (80 ms steps; prs[2] < 0.3 while talking) ----

TALK, PAUSE = 0.1, 0.9  # prs[2] while speaking / in a pause (live: < 0.3 talking, > 0.6 paused)
HOLD_STEPS = 15  # 1.2 s default hold / 80 ms


def trace(talk_s: float, sentence: list[str], pause_s: float, then_talk_s: float = 0.0, *, lag_steps: int = 10, words_from: int = 0):
    """Steps of (prs[2], Word or None) like the live trace: the words of `sentence` spread over the
    first talk, each landing `lag_steps` (0.8 s) after it was spoken — so the sentence's last words
    land during the pause; then `pause_s` of pause, then `then_talk_s` more talking (no words)."""
    n_talk, n_pause, n_more = round(talk_s / FRAME_S), round(pause_s / FRAME_S), round(then_talk_s / FRAME_S)
    steps: list[list] = [[TALK, None] for _ in range(n_talk)] + [[PAUSE, None] for _ in range(n_pause)] + [[TALK, None] for _ in range(n_more)]
    for i, w in enumerate(sentence):
        spoken = words_from + (n_talk - 1 - words_from) * (i + 1) // len(sentence)
        at = min(spoken + lag_steps, len(steps) - 1)
        steps[at][1] = (steps[at][1] + " " if steps[at][1] else "") + w
    return steps, n_talk


def run_trace(steps, **kw) -> list[int]:
    p = PausePredictor(**kw)
    fired = []
    for i, (prs, words) in enumerate(steps):
        for w in (words or "").split():
            p.word(w)
        if p.update(prs):
            fired.append(i)
    return fired


SENTENCE = "so the thing is that it's pretty close to me".split()


def test_sentence_complete_reads_kyutai_punctuation():
    for w in ("close.", "it?", "Yes!", "me.\"", "of?", "so.", ".", "Done!)"):
        assert sentence_complete(w), w
    for w in ("from", "it,", "the", "and", "so...", "well…", "um.", "Uh?", "", "meters;"):
        assert not sentence_complete(w), w


def test_complete_sentence_then_a_short_pause_ends_as_before():
    # the full stop is on screen when the pause is judged: the first paused step, as Unmute
    steps, n_talk = trace(11.0, SENTENCE[:-1] + ["me."], 0.6, lag_steps=0)
    assert run_trace(steps) == run_trace(steps, hold_s=0) == [n_talk]
    # the usual case: the last word lags the voice, lands 0.4 s into the pause — and ends it then
    steps, n_talk = trace(11.0, SENTENCE[:-1] + ["me."], 0.8, lag_steps=5)
    fired = run_trace(steps)
    assert fired == [n_talk + 4]  # the step "me." arrives (spoken at n_talk-1, +5 steps)
    assert fired[0] - run_trace(steps, hold_s=0)[0] <= 5  # ≈ as before: at most the word's lag later


def test_mid_sentence_thinking_pause_then_more_speech_is_not_cut():
    # live 2026-10-30: "…it's pretty close to me, like at 2 meters from" + ~0.6 s pause, then more
    words = "it's pretty close to me, like at 2 meters from".split()
    steps, n_talk = trace(11.0, words, 0.6, 3.0)
    assert run_trace(steps, hold_s=0) == [n_talk]  # the old rule cut the human off here
    assert run_trace(steps) == []
    # same with "Yeah, I think that"
    steps, _ = trace(2.0, "Yeah, I think that".split(), 0.7, 2.0)
    assert run_trace(steps) == []


def test_mid_sentence_long_silence_ends_after_the_hold():
    words = "it's pretty close to me, like at 2 meters from".split()
    steps, n_talk = trace(11.0, words, 1.5, lag_steps=0)
    assert run_trace(steps) == [n_talk + HOLD_STEPS - 1]  # 15 paused steps = 1.2 s
    assert run_trace(steps, hold_s=0.4) == [n_talk + 4]  # configurable


def test_a_word_landing_during_the_hold_restarts_it():
    # "from" lags 0.8 s: it lands 0.8 s into the pause and the 1.2 s starts over from there
    steps, n_talk = trace(11.0, "close to me like at 2 meters from".split(), 2.5, lag_steps=10)
    fired = run_trace(steps)
    landed = n_talk - 1 + 10
    assert fired == [landed + HOLD_STEPS - 1]
    assert fired[0] > n_talk + HOLD_STEPS - 1  # not counted from the pause's start


def test_the_hold_restarts_when_speech_resumes_without_words():
    # 1.0 s pause, 0.3 s of speech (its words not out yet), 1.0 s pause: neither pause lasted 1.2 s
    steps = [[TALK, "so"], [TALK, "from"]] + [[TALK, None]] * 12 + [[PAUSE, None]] * 12 + [[TALK, None]] * 4 + [[PAUSE, None]] * 12
    assert run_trace(steps) == []
    assert run_trace(steps, hold_s=0) == [14]


def test_no_speech_fires_once_on_silence_after_the_time():
    p = PausePredictor()
    fired = [i for i in range(100) if (p.update(0.95), p.no_speech(1.0))[1]]
    # 1.0 s = 12.5 steps of 80 ms: the 13th step (index 12) is the first at >= 1 s
    assert fired == [12] and 13 * FRAME_S >= 1.0 > 12 * FRAME_S


def test_no_speech_never_after_a_word_nor_while_someone_speaks():
    p = PausePredictor()
    for _ in range(5):
        p.update(0.0)
    p.word()
    assert not any((p.update(0.95), p.no_speech(1.0))[1] for _ in range(100))
    # someone speaks (the estimate is low) before the deadline, with no word yet (words lag
    # the voice by ~1 s): the clock starts over from their last activity
    q = PausePredictor()
    seq = [0.95] * 12 + [0.0] * 20 + [0.95] * 20
    fired = [i for i, v in enumerate(seq) if (q.update(v), q.no_speech(1.0))[1]]
    assert fired == [31 + 13]  # 13 steps (>= 1 s) after the last active step (index 31)
    # a blip shorter than ACTIVE_STEPS is not a voice
    b = PausePredictor()
    seq = [0.95] * 12 + [0.0] * 2 + [0.95] * 20
    assert [i for i, v in enumerate(seq) if (b.update(v), b.no_speech(1.0))[1]] == [14]  # no restart: due at 12, once the blip is over
    # a recent loud frame (energy veto) postpones the verdict, no more
    e = PausePredictor()
    veto = lambda i: 12 <= i < 15
    assert [i for i in range(30) if (e.update(0.95), e.no_speech(1.0, voice_recent=veto(i)))[1]] == [15]
    off = PausePredictor()
    assert not any((off.update(0.95), off.no_speech(0))[1] for _ in range(100))


class VadMoshi:
    """STT that answers every 80 ms frame with a Step: prs[2] ~0 while there is sound, 0.95 in silence; a Word on the
    first sound; the Marker comes back 6 frames (~0.5 s) after it went in, with paused Steps in between."""

    def __init__(self, lag: int = 0, word_after: int = 1, word: str = "hi."):
        self.closed = []
        self.word = word
        # like the real model: prs[2] drops only after `lag` loud frames (measured 0.3-0.45 s),
        # and the first Word comes after `word_after` loud frames (measured 1.0-1.6 s)
        self.lag, self.word_after = lag, word_after

    async def handler(self, ws):
        said = False
        loud_n = 0
        marker, after = None, 0
        try:
            async for m in ws:
                d = msgpack.unpackb(m)
                if d["type"] == "Audio" and marker is not None:
                    after += 1  # like the real model, the marker lags ~0.5 s of audio behind
                    await ws.send(msgpack.packb({"type": "Step", "step_idx": 0, "prs": [0.0, 0.0, 0.95, 0.0]}, use_single_float=True))
                    if after == 6:
                        await ws.send(msgpack.packb({"type": "Marker", "id": marker}))
                elif d["type"] == "Audio":
                    loud = max((abs(x) for x in d["pcm"]), default=0) > 0.01
                    loud_n = loud_n + 1 if loud else 0
                    if loud_n >= self.word_after and not said:
                        said = True
                        await ws.send(msgpack.packb({"type": "Word", "text": self.word, "start_time": 0.1}))
                    speaking = loud_n > self.lag
                    await ws.send(msgpack.packb({"type": "Step", "step_idx": 0, "prs": [0.0, 0.0, 0.0 if speaking else 0.95, 0.0]}, use_single_float=True))
                elif d["type"] == "Marker":
                    marker = d["id"]
        finally:
            self.closed.append(True)


def run_listen(chunks, start_extra=None, moshi=None, **cfg_extra):
    """Streams `chunks` (True = speech, False = silence) as 200 ms device frames; returns the relay's messages for s1."""
    moshi = moshi or VadMoshi()
    msgs = []

    async def relay(ws):
        await ws.recv()
        await ws.send(json.dumps({"t": "voice.welcome", "device_id": DEVICE, "mode": "auto", "voice": None}))
        await ws.send(json.dumps({"t": "stt.start", "id": "s1", "rate": 16000, **(start_extra or {})}))
        for i, loud in enumerate(chunks):
            pcm = np.full(3200, 6000 if loud else 0, dtype="<i2").tobytes()
            await ws.send(framing.encode("s1", i, pcm))
            await asyncio.sleep(0.01)
        await asyncio.sleep(0.2)
        await ws.send(json.dumps({"t": "stt.end", "id": "s1"}))
        while True:
            d = json.loads(await ws.recv())
            if d["t"] != "stt.partial":
                msgs.append(d)
            if d["t"] == "stt.final":
                break
        await asyncio.sleep(0.1)
        await ws.close()

    async def main():
        async with serve(moshi.handler, "127.0.0.1", 0) as ms, serve(relay, "127.0.0.1", 0) as rs:
            mp, rp = ms.sockets[0].getsockname()[1], rs.sockets[0].getsockname()[1]
            cfg = Config(relay=f"http://127.0.0.1:{rp}", device_id=DEVICE, token=TOKEN, stt_url=f"ws://127.0.0.1:{mp}/api/asr-streaming", tts_url="ws://127.0.0.1:1/x", **cfg_extra)
            await asyncio.wait_for(Bridge(cfg).serve_once(), 10)

    asyncio.run(main())
    return msgs


def test_bridge_sends_turn_end_once_after_speech_then_pause():
    msgs = run_listen([True] * 8 + [False] * 6)
    assert [m["t"] for m in msgs] == ["stt.turn_end", "stt.final"]
    assert msgs[0] == {"t": "stt.turn_end", "id": "s1"}
    assert msgs[1]["text"] == "hi."


def test_bridge_never_sends_turn_end_on_silence_only():
    msgs = run_listen([False] * 14)
    assert [m["t"] for m in msgs] == ["stt.final"]


def test_the_flush_silence_after_stt_end_is_not_a_turn_end():
    # speech right up to stt.end: the marker's flush silence must not be taken for the human's pause
    msgs = run_listen([True] * 10)
    assert [m["t"] for m in msgs] == ["stt.final"]


def test_bridge_sends_no_speech_once_when_nobody_speaks_and_the_relay_asked():
    msgs = run_listen([False] * 14, {"no_speech_ms": 1000})
    assert [m["t"] for m in msgs] == ["stt.turn_end", "stt.final"]
    assert msgs[0] == {"t": "stt.turn_end", "id": "s1", "reason": "no_speech"}
    assert msgs[1]["text"] == ""


def test_bridge_no_speech_never_after_a_word():
    # speech first (a word), then a long silence: the word-based turn end, never no_speech
    msgs = run_listen([True] * 3 + [False] * 11, {"no_speech_ms": 1000})
    assert [m["t"] for m in msgs] == ["stt.turn_end", "stt.final"]
    assert msgs[0] == {"t": "stt.turn_end", "id": "s1"}


def test_bridge_ignores_a_bad_no_speech_ms():
    for bad in (0, 999, 10001, "4000", True, 1500.5):
        msgs = run_listen([False] * 10, {"no_speech_ms": bad})
        assert [m["t"] for m in msgs] == ["stt.final"], bad


def test_the_flush_silence_after_stt_end_is_not_no_speech():
    # a short silent listen ended by the relay before no_speech_ms: the marker's flush
    # silence (well past 1 s of model time) must not be taken for nobody speaking
    msgs = run_listen([False] * 3, {"no_speech_ms": 1000})
    assert [m["t"] for m in msgs] == ["stt.final"]


def test_speech_starting_just_before_the_no_speech_deadline_is_not_no_speech():
    # 4 s deadline, the human starts at 3.8 s; their first word comes ~1.2 s later and the
    # model's "speaking" ~0.4 s later (after the deadline): the energy veto holds the verdict,
    # the model's activity restarts the clock, the word ends the turn — never no_speech
    lagging = VadMoshi(lag=5, word_after=15)
    msgs = run_listen([False] * 19 + [True] * 12 + [False] * 10, {"no_speech_ms": 4000}, lagging)
    assert [m["t"] for m in msgs] == ["stt.turn_end", "stt.final"]
    assert msgs[0] == {"t": "stt.turn_end", "id": "s1"}


def test_speech_without_a_word_restarts_the_no_speech_clock():
    # a voice the model hears (prs) but no word yet (e.g. a hum): no_speech only once that
    # many seconds of quiet followed it
    hum = VadMoshi(lag=0, word_after=10**6)
    msgs = run_listen([False] * 4 + [True] * 4 + [False] * 4, {"no_speech_ms": 1000}, hum)
    assert [m["t"] for m in msgs] == ["stt.final"]  # 2.4 s of quiet after 1.6 s of hum: but only ~0.8 s fed after it ended
    msgs = run_listen([False] * 4 + [True] * 4 + [False] * 10, {"no_speech_ms": 1000}, VadMoshi(lag=0, word_after=10**6))
    assert [m["t"] for m in msgs] == ["stt.turn_end", "stt.final"]
    assert msgs[0]["reason"] == "no_speech"


def test_bridge_holds_a_mid_sentence_pause_and_hold_0_restores_the_old_rule():
    # the word has no full stop: 0.8 s of pause is not enough (the default hold is 1.2 s)...
    msgs = run_listen([True] * 8 + [False] * 4, moshi=VadMoshi(word="hi"))
    assert [m["t"] for m in msgs] == ["stt.final"]
    # ...2 s is
    msgs = run_listen([True] * 8 + [False] * 10, moshi=VadMoshi(word="hi"))
    assert [m["t"] for m in msgs] == ["stt.turn_end", "stt.final"]
    # a finished sentence ends at the pause, and turn_end_hold_s=0 (--turn-end-hold 0) is Unmute's rule
    assert [m["t"] for m in run_listen([True] * 8 + [False] * 4)] == ["stt.turn_end", "stt.final"]
    msgs = run_listen([True] * 8 + [False] * 4, moshi=VadMoshi(word="hi"), turn_end_hold_s=0)
    assert [m["t"] for m in msgs] == ["stt.turn_end", "stt.final"]
