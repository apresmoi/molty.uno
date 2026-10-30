import asyncio
import json

import numpy as np
import pytest

from molty_sim import vad as vadm
from molty_sim.device import (CAPS_146, SayRec, SimDevice, ack_arg, ack_decide, decode_frame, encode_frame, new_device_id)
from molty_sim.voice import Clip, Mic, assemble, speech_intervals
from molty_sim.scenario import parse_markup

RATE = 16000


def tone(sec, db, f=300.0):
    t = np.arange(int(sec * RATE)) / RATE
    return (10 ** (db / 20) * np.sqrt(2) * np.sin(2 * np.pi * f * t) * 32767).astype(np.int16)


def silence(sec, db=-70.0, seed=1):
    return (np.random.default_rng(seed).standard_normal(int(sec * RATE)) * 10 ** (db / 20) * 32767).astype(np.int16)


def run_vad(pcm, **cfg):
    v = vadm.Vad(vadm.VadCfg(**cfg))
    for i in range(0, len(pcm), 1600):
        if v.feed(pcm[i : i + 1600]) != vadm.CONTINUE:
            break
    return v


def test_vad_ends_on_silence_after_speech():
    v = run_vad(np.concatenate([silence(0.5), tone(1.0, -20), silence(2.0)]))
    assert v.verdict == vadm.END_SILENCE
    assert v.speech_ms == pytest.approx(1000, abs=40)
    assert 2.15 <= v.t_ms / 1000 <= 2.3  # 0.5 + 1.0 + 0.7 end_silence


def test_vad_no_speech_and_max_and_disabled():
    assert run_vad(silence(6.0)).verdict == vadm.END_NO_SPEECH
    v = run_vad(silence(6.0), enabled=False, max_ms=3000)
    assert v.verdict == vadm.END_MAX and v.t_ms == 3000 and not v.speech_known()
    # speech already running when the listen starts still counts (calibration replay, floor capped at -50)
    v = run_vad(np.concatenate([tone(0.25, -20), silence(2.0)]))
    assert v.verdict == vadm.END_SILENCE and v.speech_ms >= 160


def test_ack_rules_match_firmware():
    assert ack_arg({}, "chirp") == "chirp" and ack_arg({"ack": False}, "chirp") == "off"
    assert ack_arg({"ack": "silent"}, "chirp") == "thinking" and ack_arg({"ack": "silent"}, "off") == "off"
    assert ack_arg({"ack": "loud"}, "chirp") is None
    assert ack_decide("chirp", "turn_end", False, 0) == "chirp"
    assert ack_decide("chirp", "no_speech", True, 0) == "off"
    assert ack_decide("chirp", "max", True, 100) == "off" and ack_decide("chirp", "max", True, 160) == "chirp"
    assert ack_decide("chirp", "silence", True, 0) == "off" and ack_decide("chirp", "silence", False, 0) == "chirp"
    assert ack_decide("chirp", "error", True, 900) == "off"
    assert ack_decide("chirp", "turn_end", True, 900, has_speaker=False) == "thinking"


def test_frames_roundtrip_and_ids():
    h, p = decode_frame(encode_frame({"s": "j1", "seq": 3, "kind": "pcm16", "rate": 16000, "end": True}, b"\x01\x02"))
    assert h["seq"] == 3 and h["end"] and p == b"\x01\x02"
    assert decode_frame(b'{"s":"j","seq":0}') == ({"s": "j", "seq": 0}, b"")
    d = new_device_id()
    assert d.startswith("molty-sim") and len(d) == len("molty-") + 10
    with pytest.raises(ValueError):
        SimDevice("ws://x", "molty-k7m2q9xa4c")
    assert {"say_cues", "listen_stop", "listen_window", "ack"} <= set(CAPS_146)


def test_say_playback_model_underrun_and_cue_time():
    s = SayRec(job="j", text="a b", cmd_at=0.0)
    s.first_at, s.play_start, s.cursor = 10.0, 10.0, 10.0
    # emulate _on_audio bookkeeping: 0.32 s arrives at 10.0, the next 0.32 s only at 10.5 (dry 0.18 s)
    s.frames = [{"at": 10.0, "seq": 0, "n": 5120, "offset": 0, "play_at": 10.0, "cues": 0},
                {"at": 10.5, "seq": 1, "n": 5120, "offset": 5120, "play_at": 10.5, "cues": 0}]
    s.samples, s.cursor = 10240, 10.82
    assert s.play_time(1600) == pytest.approx(10.1)
    assert s.play_time(5120 + 800) == pytest.approx(10.55)
    assert s.play_time(99999) == pytest.approx(10.82)


class FakeWs:
    def __init__(self):
        self.sent = []

    async def send(self, d):
        self.sent.append(d)


def test_listen_streams_300ms_frames_and_honours_listen_stop():
    async def go():
        dev = SimDevice("ws://x", new_device_id())
        dev._ws = FakeWs()
        clip = Clip(np.full(RATE * 3, 0.1, np.float32))
        dev.mic_for = lambda job: Mic(clip)
        await dev._on_cmd({"t": "cmd", "id": "c1", "name": "listen", "args": {"job": "L1", "rate": 16000, "vad": False}})
        res = json.loads(dev._ws.sent[0])
        assert res["ok"] and res["result"] == {"job": "L1", "ms": 30000, "ack": "chirp"}
        await asyncio.sleep(0.75)  # two full 300 ms frames out, a third building
        await dev._on_cmd({"t": "cmd", "id": "c2", "name": "listen_stop", "args": {"job": "L1", "reason": "turn_end"}})
        rec = dev.listens["L1"]
        await asyncio.wait_for(rec.done.wait(), 1)
        frames = [decode_frame(b) for b in dev._ws.sent if isinstance(b, bytes)]
        sizes = [len(p) // 2 for _, p in frames]
        assert sizes[:2] == [4800, 4800] and 0 < sizes[2] <= 3200
        last = frames[-1][0]
        assert last["end"] and last["reason"] == "turn_end" and "speech_ms" not in last  # vad off: not measured
        assert [h["seq"] for h, _ in frames] == list(range(len(frames)))
        assert rec.ack == "chirp" and rec.stop_mic_s == pytest.approx(0.75, abs=0.05)
        assert rec.end_mic_s <= 0.9  # within one 100 ms chunk of the stop
        stop_res = [json.loads(x) for x in dev._ws.sent if isinstance(x, str) and '"c2"' in x][0]
        assert stop_res["result"] == {"job": "L1", "stopping": True}
        await dev._on_cmd({"t": "cmd", "id": "c3", "name": "listen_stop", "args": {"job": "L1", "reason": "nope"}})
        assert json.loads(dev._ws.sent[-1])["error"] == "invalid_args"

    asyncio.run(go())


def test_listen_busy_and_window_validation():
    async def go():
        dev = SimDevice("ws://x", new_device_id())
        dev._ws = FakeWs()
        await dev._on_cmd({"t": "cmd", "id": "a", "name": "listen", "args": {"job": "L", "ms": 30001}})
        assert json.loads(dev._ws.sent[-1])["error"] == "invalid_args"
        await dev._on_cmd({"t": "cmd", "id": "b", "name": "listen", "args": {"job": "L", "ms": 0, "vad": False}})
        assert json.loads(dev._ws.sent[-1])["result"]["ms"] == 0
        await dev._on_cmd({"t": "cmd", "id": "c", "name": "say", "args": {"job": "S", "text": "hi", "ms": 100}})
        assert json.loads(dev._ws.sent[-1])["error"] == "busy"
        await dev._on_cmd({"t": "cmd", "id": "d", "name": "listen_stop", "args": {"job": "L", "reason": "no_speech"}})
        await asyncio.wait_for(dev.listens["L"].done.wait(), 1)
        assert dev.listens["L"].reason == "no_speech" and dev.listens["L"].ack == "off"

    asyncio.run(go())


def test_assemble_puts_pauses_between_the_right_words():
    # 4 "words" of tone separated by short gaps; onsets at 0.1, 0.5, 0.65, 1.0 (word 2 is short)
    x = np.zeros(int(1.5 * RATE), np.float32)
    for a, b in [(0.1, 0.45), (0.5, 0.6), (0.65, 0.95), (1.0, 1.4)]:
        x[int(a * RATE):int(b * RATE)] = 0.3
    m = parse_markup("one two {pause 1.0} three four")
    y, words = assemble(x, [("one", 0.1), ("two", 0.5), ("three", 0.65), ("four", 1.0)], m)
    assert [w for w, _ in words] == ["one", "two", "three", "four"]
    on = [o for _, o in words]
    assert on[1] - on[0] == pytest.approx(0.4, abs=0.01) and on[2] - on[1] == pytest.approx(1.15, abs=0.01)
    iv = speech_intervals(y)
    assert len(iv) == 2  # one two  [1 s]  three four — the short "two" stays before the pause
    assert iv[0][1] == pytest.approx(on[1] + 0.1, abs=0.03) and iv[1][0] - iv[0][1] > 0.95
    c = Clip(y, speech=iv)
    assert c.speech_after(iv[1][0]) == pytest.approx(sum(b - a for a, b in iv[1:]))
