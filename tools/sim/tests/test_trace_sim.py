"""Turn trace in the sim: scenario keys, the shake settle port, the resend ring, trace header fields,
shake to talk on the sim device, and the offline measurement."""

import asyncio
import json
from pathlib import Path

import numpy as np
import pytest

from molty_sim import device as devmod
from molty_sim import measure, scenario
from molty_sim.device import SimDevice, decode_frame, new_device_id
from molty_sim.scenario import ScenarioError, parse
from molty_sim.uplink import (RATE, Settle, UpFrame, UplinkRing, apply_gain, clipped, deliver_offline, handling_noise, level_byte,
                              settle_of, settle_trim, shake_imu)
from molty_sim.voice import Clip, Mic, with_shake

TRACE = Path(__file__).resolve().parents[1] / "scenarios" / "trace.yaml"


# ---------------- scenario ----------------

def test_trace_suite_parses_shake_and_stall():
    sc = scenario.load(TRACE)
    by = {t.id: t for t in sc.turns}
    assert by["shake-noise"].shake == {"peak_g": 8.2, "after_s": 0.45, "noise_peak": 0.9}
    assert by["wifi-stall"].stalls == [(2.0, 5.0)] and by["wifi-stall"].shake is None
    t = parse({"turns": [{"user": "hi", "stall": [{"at_s": 1, "for_s": 0.5}, {"at_s": 3, "for_s": 1}]}]}).turns[0]
    assert t.stalls == [(1.0, 1.5), (3.0, 4.0)]


@pytest.mark.parametrize("turn", [
    {"user": "hi", "shake": {"peak_g": 30}},
    {"user": "hi", "shake": {"loud": 1}},
    {"user": "hi", "shake": 3},
    {"user": "hi", "stall": {"at_s": 1}},
    {"user": "hi", "stall": {"at_s": 1, "for_s": 0}},
])
def test_bad_shake_and_stall_are_refused(turn):
    with pytest.raises(ScenarioError):
        parse({"turns": [turn]})


# ---------------- settle (firmware rule) ----------------

def still(ms_from=0, ms_to=1000, g=1.0):
    return [(0.0, 0.0, g, ms) for ms in range(ms_from, ms_to, 20)]


def test_settle_is_confirmed_150ms_into_a_still_run():
    s = settle_of(still())
    assert (s.at_ms, s.still_from, s.capped) == (160, 0, False)  # 50 Hz samples: the first at >= 150 ms
    moving = [(0.0, 0.0, 1.3, ms) for ms in range(0, 300, 20)] + still(300)
    s = settle_of(moving)
    assert (s.at_ms, s.still_from) == (460, 300)
    # a jolt inside the run starts it over; just inside the tolerance is still
    jolt = still(0, 100) + [(0.0, 0.0, 1.2, 100)] + still(120, 1000, g=1.14)
    assert settle_of(jolt).still_from == 120


def test_settle_caps_at_600ms():
    s = settle_of(shake_imu(8.2, 0.45, seed=1))
    assert s.at_ms == 600 and s.capped
    s = Settle()
    assert not s.tick(599) and s.tick(600) and s.capped and s.at_ms == 600


def test_settle_trim_drops_the_lead_and_fades_in():
    x = np.full(RATE, 10000, np.int16)
    y = settle_trim(x, 250)
    assert len(y) == RATE - 4000
    assert y[0] < 100 and y[159] > 9900 and np.all(np.diff(y[:160].astype(int)) >= 0)  # 10 ms fade
    assert np.array_equal(settle_trim(x, 0), x)  # nothing cut: no fade either


def test_handling_noise_and_imu_are_deterministic():
    assert np.array_equal(handling_noise(0.45, seed=3), handling_noise(0.45, seed=3))
    assert not np.array_equal(handling_noise(0.45, seed=3), handling_noise(0.45, seed=4))
    h = handling_noise(0.45, seed=3)
    assert abs(float(np.max(np.abs(h))) - 0.9) < 1e-6 and len(h) == int(0.51 * RATE)
    imu = shake_imu(8.2, 0.45, seed=3)
    assert imu == shake_imu(8.2, 0.45, seed=3)
    assert max(abs(a) for a, _, _, ms in imu if ms < 450) > 6


# ---------------- trace fields ----------------

def test_level_byte_and_clipping():
    assert level_byte(np.zeros(1600, np.int16)) == 0
    sine = (np.sin(np.arange(1600) * 2 * np.pi * 300 / RATE) * 32767).astype(np.int16)
    assert level_byte(sine) == 194  # -3 dBFS
    assert level_byte((sine / 100).astype(np.int16)) == 114  # -43 dBFS: 2 * (-43 + 100)
    assert clipped(apply_gain((sine / 4).astype(np.int16), 8)) > 0 and clipped(apply_gain((sine / 8).astype(np.int16), 6)) == 0


# ---------------- the resend ring ----------------

def test_ring_keeps_failed_frames_and_drops_the_oldest_when_full():
    r = UplinkRing(max_s=1.0)
    for k in range(3):
        assert r.push(UpFrame(k, {}, b"", 0.0, 4800)) == []
    r.failed()
    assert r.head().seq == 0 and r.head().rt == 1 and r.fails == 1
    gone = r.push(UpFrame(3, {}, b"", 0.0, 4800))  # 1.2 s waiting > 1 s: the oldest goes
    assert [g.seq for g in gone] == [0] and r.dropped == 1 and r.dropped_samples == 4800
    assert r.sent_one().seq == 1
    r.push(UpFrame(4, {"end": True}, b"", 0.0, 0))
    assert [f.seq for f in r.q] == [2, 3, 4]


def test_stall_offline_old_loses_audio_and_resend_keeps_all():
    old = deliver_offline(9.0, [(2.0, 5.0)], resend=False)
    new = deliver_offline(9.0, [(2.0, 5.0)], resend=True)
    assert old["delivered_ms"] < old["wall_ms"] - 3000 and old["lost_frames"] > 0
    assert new["delivered_ms"] == new["captured_ms"] == 9000 and new["lost_frames"] == 0 and new["in_order"]
    assert new["failed_attempts"] == 30  # every 100 ms for 3 s
    # a stall longer than the ring: the oldest frames are dropped, the rest still arrives in order
    long = deliver_offline(20.0, [(1.0, 13.0)], resend=True)
    assert long["dropped_by_ring"] > 0 and long["in_order"] and long["delivered_ms"] < long["captured_ms"]


# ---------------- the sim device ----------------

class FakeWs:
    def __init__(self):
        self.sent = []

    async def send(self, d):
        self.sent.append(d)


def frames_of(dev):
    return [decode_frame(b) for b in dev._ws.sent if isinstance(b, bytes)]


def texts_of(dev):
    return [json.loads(x) for x in dev._ws.sent if isinstance(x, str)]


def test_listen_frames_carry_the_trace_fields_and_a_stall_is_resent_in_order():
    async def go():
        dev = SimDevice("ws://x", new_device_id())
        dev._ws = FakeWs()
        loud = np.zeros(int(2.0 * RATE), np.float32)
        loud[: RATE] = 1.2 * np.sin(np.arange(RATE) * 2 * np.pi * 200 / RATE)  # clips
        dev.mic_for = lambda job: Mic(Clip(loud))
        dev.stall_for = lambda job: [(0.35, 0.95)]
        await dev._on_cmd({"t": "cmd", "id": "c1", "name": "listen", "args": {"job": "L1", "rate": 16000, "vad": False, "ms": 1500}})
        rec = dev.listens["L1"]
        await asyncio.wait_for(rec.done.wait(), 5)
        fr = frames_of(dev)
        heads = [h for h, _ in fr]
        assert [h["seq"] for h in heads] == list(range(5)) and heads[-1]["end"] and heads[-1]["reason"] == "max"
        assert [h["ct"] for h in heads] == [0, 300, 600, 900, 1200]
        assert all(len(h["lv"]) == 2 * len(p) // 3200 for h, p in fr)  # 1 byte per 100 ms chunk
        assert heads[0]["cl"] > 0 and "cl" not in heads[-1]
        assert int(heads[0]["lv"][:2], 16) > 190 and int(heads[-1]["lv"][:2], 16) == 0
        retried = [h for h in heads if h.get("rt")]
        assert retried and all(h["wq"] >= 100 for h in retried)
        tr = heads[-1]["tr"]
        assert tr["sent"] == 5 and tr["fail"] == sum(h.get("rt", 0) for h in heads) > 0
        assert tr["cap"] == 1500 and tr["gain"] == 6 and tr["pk"] == 1000 and "set" not in tr
        assert sum(len(p) for _, p in fr) == 1500 * 32  # nothing lost

    asyncio.run(go())


def shaken_mic(after_s=0.45, peak_g=8.2):
    c = Clip(np.zeros(int(2.0 * RATE), np.float32), seed=9)
    c.pcm[int(0.8 * RATE):] = 0.2
    return with_shake(c, {"peak_g": peak_g, "after_s": after_s, "noise_peak": 0.9})


def test_shake_to_talk_trims_the_handling_noise_and_is_adopted():
    async def go():
        dev = SimDevice("ws://x", new_device_id())
        dev._ws = FakeWs()
        dev.features = ["listen_adopt"]
        c = shaken_mic()
        dev.mic_for = lambda job: Mic(c)
        dev.imu_for = lambda job: c.imu
        local = await dev.shake(peak_g=8.2)
        assert local == "shake-1"
        evt = texts_of(dev)[0]
        assert evt == {"t": "evt", "name": "shake", "data": {"peak_g": 8.2, "reversals": 4, "ms": 420, "listen": "shake-1"}}
        await asyncio.sleep(0.9)
        assert frames_of(dev) == []  # nothing goes up before the relay adopts it
        await dev._on_cmd({"t": "cmd", "id": "a1", "name": "listen", "args": {"job": "J1", "adopt": "shake-1", "rate": 16000, "vad": False, "ms": 1500}})
        assert texts_of(dev)[-1]["result"] == {"job": "J1", "ms": 1500, "ack": "chirp"}
        rec = dev.listens["J1"]
        await asyncio.wait_for(rec.done.wait(), 5)
        heads = [h for h, _ in frames_of(dev)]
        assert all(h["s"] == "J1" for h in heads) and [h["seq"] for h in heads] == list(range(len(heads)))
        assert heads[0]["ct"] == 600 and rec.trim_ms == 600 and rec.trim_capped  # a hard shake: the cap
        tr = heads[-1]["tr"]
        assert tr["set"] == 600 and tr["setc"] == 1 and tr["cap"] == 1500  # the window counts the audio kept
        assert all("cl" not in h for h in heads)
        # the handling noise (up to 0.51 s) never went up: the first chunk is room quiet, speech from 0.8 s
        assert int(heads[0]["lv"][:2], 16) == 0 and int(heads[0]["lv"][4:6], 16) > 150
        # a late adoption is refused
        await dev._on_cmd({"t": "cmd", "id": "a2", "name": "listen", "args": {"job": "J2", "adopt": "shake-1", "rate": 16000}})
        assert texts_of(dev)[-1]["error"] == "listen_gone"

    asyncio.run(go())


def test_shake_declined_or_unclaimed_ends_quietly(monkeypatch):
    monkeypatch.setattr(devmod, "ADOPT_WINDOW_MS", 500)

    async def go():
        for decline in (True, False):
            dev = SimDevice("ws://x", new_device_id())
            dev._ws = FakeWs()
            dev.features = ["listen_adopt"]
            c = shaken_mic(after_s=0.05, peak_g=2.0)
            dev.mic_for = lambda job, c=c: Mic(c)
            dev.imu_for = lambda job, c=c: c.imu
            local = await dev.shake(peak_g=2.0)
            if decline:
                await asyncio.sleep(0.3)
                await dev._on_cmd({"t": "cmd", "id": "d", "name": "listen_stop", "args": {"job": local, "reason": "no_speech"}})
            rec = dev.listens[local]
            await asyncio.wait_for(rec.done.wait(), 3)
            assert frames_of(dev) == [] and rec.ack == "off" and dev.mood == "idle"
            assert rec.reason == ("declined" if decline else "unclaimed") and dev._busy is None
        # without listen_adopt in the welcome: the plain shake, no listen
        dev = SimDevice("ws://x", new_device_id())
        dev._ws = FakeWs()
        assert await dev.shake() is None and texts_of(dev)[0]["data"] == {"peak_g": 8.2, "reversals": 4, "ms": 420}

    asyncio.run(go())


def test_hints_are_recorded_for_the_running_listen():
    async def go():
        dev = SimDevice("ws://x", new_device_id())
        dev._ws = FakeWs()
        dev.mic_for = lambda job: Mic(None)
        assert {"trace", "hints", "shake_talk"} <= set(dev.caps)
        await dev._on_cmd({"t": "cmd", "id": "c1", "name": "listen", "args": {"job": "L1", "rate": 16000, "vad": False, "ms": 1000}})
        await dev._on_text({"t": "hint", "job": "L1", "k": "pause"})
        await dev._on_text({"t": "hint", "job": "other", "k": "question"})
        await dev._on_text({"t": "hint", "job": "L1", "k": "dance"})
        rec = dev.listens["L1"]
        await asyncio.wait_for(rec.done.wait(), 3)
        assert rec.hints == ["pause"] and len(dev.hints) == 3

    asyncio.run(go())


# ---------------- offline measurement ----------------

def test_measure_numbers_make_sense():
    m = measure.measure()
    sn = m["shake_noise"]
    assert sn["settle_ms"] <= 600 and sn["speech_kept"]
    assert sn["handling_energy_sent_pct"]["trimmed"] < 1 and sn["vad_voiced_ms_before_speech"]["untrimmed"] > 160
    assert sn["vad_voiced_ms_before_speech"]["trimmed"] == 0
    q = m["shake_quick"]
    assert q["settle_ms"] < 400 and q["handling_energy_sent_pct"]["trimmed"] < q["if_cut_at_still_start"]["handling_energy_sent_pct"]
    ws = m["wifi_stall"]
    assert ws["resend"]["delivered_ms"] == ws["resend"]["captured_ms"] == 9000
    assert ws["old"]["delivered_ms"] < 6000  # the field: 8.97 s listen, 5.4 s arrived
    g = m["mic_gain"]
    assert g["gain_8"]["clipped_samples"] > 0 and g["gain_6"]["clipped_samples"] == 0
