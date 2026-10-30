"""Semantic end of turn: Unmute's pause rule (PausePredictor) and stt.turn_end through the bridge."""

import asyncio
import json

import msgpack
import numpy as np
from websockets.asyncio.server import serve

from molty_voice import framing
from molty_voice.bridge import Bridge
from molty_voice.config import Config
from molty_voice.kyutai import FRAME_S, PausePredictor

DEVICE = "molty-k7m2q9xa4c"
TOKEN = DEVICE + "." + "A" * 43


def feed(p: PausePredictor, values) -> list[int]:
    """Indices (0-based steps) at which update() fired."""
    return [i for i, v in enumerate(values) if p.update(v)]


def test_fires_once_after_words_when_the_pause_estimate_rises():
    p = PausePredictor()
    p.word()
    fired = feed(p, [0.0] * 20 + [0.95] * 10 + [0.0] * 5 + [0.95] * 10)
    assert fired == [20]  # the first paused step after speech, and never again


def test_never_fires_on_silence_alone():
    p = PausePredictor()
    assert feed(p, [0.95] * 100) == []  # no word: Molty's own no_speech handles an empty listen


def test_ignores_the_first_12_steps():
    p = PausePredictor()
    p.word()
    assert feed(p, [0.95] * 13) == [12]  # steps 0..11 are skipped, step 12 counts


def test_threshold_and_off_switch():
    p = PausePredictor(threshold=0.97)
    p.word()
    assert feed(p, [0.0] * 12 + [0.95] * 20) == []  # never above 0.97
    off = PausePredictor(threshold=0)
    off.word()
    assert feed(off, [0.0] * 12 + [0.99] * 20) == []


def test_ema_matches_unmute_constants():
    p = PausePredictor()
    p.word()
    feed(p, [0.0] * 12)  # skipped: the value is still the initial 1.0
    assert p.value == 1.0
    p.update(0.0)
    # half-life 0.01 s over an 80 ms step: alpha = 1 - 2**-8
    assert abs(p.value - 2 ** -8) < 1e-9


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
    # speaking right at the deadline (the estimate is low): not yet; it fires once they stop
    q = PausePredictor()
    seq = [0.95] * 12 + [0.0] * 20 + [0.95] * 5
    fired = [i for i, v in enumerate(seq) if (q.update(v), q.no_speech(1.0))[1]]
    assert fired == [32]
    off = PausePredictor()
    assert not any((off.update(0.95), off.no_speech(0))[1] for _ in range(100))


class VadMoshi:
    """STT that answers every 80 ms frame with a Step: prs[2] ~0 while there is sound, 0.95 in silence; a Word on the
    first sound; the Marker comes back 6 frames (~0.5 s) after it went in, with paused Steps in between."""

    def __init__(self):
        self.closed = []

    async def handler(self, ws):
        said = False
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
                    if loud and not said:
                        said = True
                        await ws.send(msgpack.packb({"type": "Word", "text": "hi", "start_time": 0.1}))
                    await ws.send(msgpack.packb({"type": "Step", "step_idx": 0, "prs": [0.0, 0.0, 0.0 if loud else 0.95, 0.0]}, use_single_float=True))
                elif d["type"] == "Marker":
                    marker = d["id"]
        finally:
            self.closed.append(True)


def run_listen(chunks, start_extra=None):
    """Streams `chunks` (True = speech, False = silence) as 200 ms device frames; returns the relay's messages for s1."""
    moshi = VadMoshi()
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
            cfg = Config(relay=f"http://127.0.0.1:{rp}", device_id=DEVICE, token=TOKEN, stt_url=f"ws://127.0.0.1:{mp}/api/asr-streaming", tts_url="ws://127.0.0.1:1/x")
            await asyncio.wait_for(Bridge(cfg).serve_once(), 10)

    asyncio.run(main())
    return msgs


def test_bridge_sends_turn_end_once_after_speech_then_pause():
    msgs = run_listen([True] * 8 + [False] * 6)
    assert [m["t"] for m in msgs] == ["stt.turn_end", "stt.final"]
    assert msgs[0] == {"t": "stt.turn_end", "id": "s1"}
    assert msgs[1]["text"] == "hi"


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
