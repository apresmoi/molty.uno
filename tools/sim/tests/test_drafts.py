"""Shake turns in the sim: the device's shake listen + adoption, react moods, the scenario keys and the drafts metrics."""

import asyncio
import json
from pathlib import Path

import numpy as np
import pytest

from molty_sim import scenario
from molty_sim.device import CAPS_146, SimDevice, new_device_id
from molty_sim.metrics import ShakeFacts, drafts_summary, shake_metrics
from molty_sim.voice import Clip, Mic

RATE = 16000
SIM = Path(__file__).resolve().parents[1]


class FakeWs:
    def __init__(self):
        self.sent = []

    async def send(self, d):
        self.sent.append(d)


def texts(ws):
    return [json.loads(x) for x in ws.sent if isinstance(x, str)]


def test_react_moods_and_gestures_keep_the_turn_ack_explicit_moods_do_not():
    async def go():
        dev = SimDevice("ws://x", new_device_id())
        dev._ws = FakeWs()
        assert {"shake_talk", "react", "act"} <= set(CAPS_146)
        dev.mic_for = lambda job: Mic(Clip(np.full(RATE * 3, 0.1, np.float32)))

        async def turn(job, moods):
            await dev._on_cmd({"t": "cmd", "id": f"l-{job}", "name": "listen", "args": {"job": job, "rate": 16000, "vad": False}})
            await asyncio.sleep(0.25)
            for i, (name, args) in enumerate(moods):
                await dev._on_cmd({"t": "cmd", "id": f"m-{job}-{i}", "name": name, "args": args})
            await dev._on_cmd({"t": "cmd", "id": f"s-{job}", "name": "listen_stop", "args": {"job": job, "reason": "turn_end"}})
            rec = dev.listens[job]
            await asyncio.wait_for(rec.done.wait(), 2)
            return rec.ack

        # a react mood and a gesture during the listen: the device still acknowledges the turn
        assert await turn("J1", [("mood", {"mood": "happy", "hold_s": 0, "react": True}), ("act", {"action": "nod"})]) == "chirp"
        assert [(m, r) for _, m, r in dev.moods] == [("happy", True), ("act:nod", True)]
        # an explicit mood during the listen: no acknowledgement (link_ack_decide screen_taken)
        assert await turn("J2", [("mood", {"mood": "happy"})]) == "off"
        await dev._on_cmd({"t": "cmd", "id": "bad", "name": "mood", "args": {"mood": "happy", "react": 1}})
        assert [json.loads(x) for x in dev._ws.sent if isinstance(x, str) and '"bad"' in x][0]["error"] == "invalid_args"

    asyncio.run(go())


def test_drafts_scenario_parses_and_rejects_bad_shake_keys():
    sc = scenario.load(SIM / "scenarios" / "drafts.yaml")
    assert sc.shake and sc.arms == ["baseline", "drafts"]
    t = {x.id: x for x in sc.turns}
    assert t["alarm-flip"].draft == {"when": "alarm", "opener": "Sure, alarm at seven.", "family": "confirm"} and t["alarm-flip"].answer == "hedge"
    assert t["bug-fixed"].react == {"when": "fixed", "mood": "happy"}
    assert scenario.parse({**{"name": "x", "shake": True}, "turns": [{"id": "a", "user": "hi there", "reply": "x", "react": {"when": "hi", "action": "nod"}}]}).turns[0].react["action"] == "nod"
    base = {"name": "x", "shake": True, "turns": [{"id": "a", "user": "hi there", "reply": "hello"}]}
    assert scenario.parse(base).turns[0].reply == "hello"
    for bad in [
        {**base, "turns": [{"id": "a", "user": "hi", "reply": "x", "say": "no say in a shake turn"}]},
        {**base, "turns": [{"id": "a", "user": "hi"}]},  # no reply
        {**base, "turns": [{"id": "a", "user": "hi", "reply": "x", "draft": {"when": "hi", "opener": "Yo", "family": "f"}}]},  # no answer
        {**base, "turns": [{"id": "a", "user": "hi", "reply": "x", "react": {"when": "hi", "mood": "speaking"}}]},
        {**base, "arms": ["drafts", "nope"]},
        {"name": "x", "turns": [{"id": "a", "user": "hi", "reply": "x"}]},  # shake keys without shake: true
    ]:
        with pytest.raises(scenario.ScenarioError):
            scenario.parse(bad)


def test_shake_metrics_gap_wrong_opener_seam_and_react():
    used_wrong = shake_metrics(ShakeFacts(arm="drafts", speech_end_at=10.0, first_voice_at=10.35, partials=2, drafted=True, used=True,
                                          family="commit", answer="modify", opener_end_at=11.2, follow_start_at=11.26,
                                          react_sent_at=8.0, react_shown_at=8.05, react_word_at=7.6, ack="chirp"))
    assert used_wrong["gap_ms"] == 350 and used_wrong["wrong_opener"] and used_wrong["seam_ms"] == 60
    assert used_wrong["react_rtt_ms"] == 50 and used_wrong["react_lag_ms"] == 450
    saved = shake_metrics(ShakeFacts(arm="drafts", speech_end_at=10.0, first_voice_at=11.5, drafted=True, dropped="meaning", family="confirm", answer="hedge"))
    assert saved["saved"] and not saved["lost"] and not saved["wrong_opener"]
    lost = shake_metrics(ShakeFacts(arm="drafts", speech_end_at=10.0, first_voice_at=11.5, drafted=True, dropped="new_words", family="empathy", answer="empathy"))
    assert lost["lost"] and not lost["saved"]
    base = shake_metrics(ShakeFacts(arm="baseline", speech_end_at=10.0, first_voice_at=11.4))
    assert base == {"arm": "baseline", "partials": 0, "ack": "off", "gap_ms": 1400}


def test_drafts_summary_pairs_the_arms():
    rows = [
        {"id": "a", "arm": "baseline", "gap_ms": 1400}, {"id": "a", "arm": "drafts", "gap_ms": 350, "drafted": True, "used": True, "partials": 2, "seam_ms": 60},
        {"id": "b", "arm": "baseline", "gap_ms": 1500}, {"id": "b", "arm": "drafts", "gap_ms": 1450, "drafted": True, "dropped": "meaning", "saved": True, "partials": 1},
        {"id": "c", "arm": "baseline", "gap_ms": 1300}, {"id": "c", "arm": "drafts", "gap_ms": 380, "drafted": True, "used": True, "wrong_opener": True, "partials": 1,
                                                          "react_error": None, "react_rtt_ms": 40, "react_lag_ms": 500},
        {"id": "d", "arm": "baseline", "gap_ms": 1200}, {"id": "d", "arm": "drafts", "gap_ms": 1250, "partials": 0},
    ]
    s = drafts_summary(rows)
    assert s["drafted"] == 3 and s["used"] == 2 and s["use_rate"] == 0.67
    assert s["dropped"] == {"meaning": 1} and s["wrong_openers"] == 1 and s["wrong_opener_rate"] == 0.5 and s["saved_by_relay"] == 1
    assert s["gap_saved_ms"]["median"] == pytest.approx(485.0)  # paired: 1050, 50, 920, -50
    assert s["gap_drafts_used_ms"]["median"] == 365.0 and s["turns_with_partials"] == 3
    assert s["reacts"] == 1 and s["react_lag_ms"]["median"] == 500


def test_careful_drafter_hears_turn_around_words():
    from molty_sim.run import heard_flip
    assert heard_flip("set an alarm for seven, but")
    assert heard_flip("Actually, no.") and heard_flip("I don’t know")
    assert not heard_flip("what's on my calendar today?")
