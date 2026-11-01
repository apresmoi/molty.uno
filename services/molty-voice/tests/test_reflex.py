"""Molty's reflex on the box (molty_voice/reflex.py): the chunk rule (the relay's, word for word),
the GoEmotions fold, and stt.reflex through the bridge with a stub model (no onnxruntime needed)."""

import asyncio
import json
import urllib.error

import msgpack
import numpy as np
import pytest

from molty_voice import framing, reflex_hosted
from molty_voice.reflex import GO_MAP, PICKS, ReflexTracker, category, fold, reflex_due, reflex_input, rule_category
from test_bridge import DEVICE, FakeMoshi, with_servers


def test_every_word_count_the_relay_may_pick_is_classified():
    # the relay's reflexDue (services/relay/test/reflex.test.ts "reflex chunks") picks its chunks among
    # the word counts from FIRST_WORDS (3) on: the box classifies each new one, so any pick has a verdict
    assert reflex_due(0, "hi.") is None
    assert reflex_due(0, "hi there.") is None
    assert reflex_due(0, "hi there you") == 3
    assert reflex_due(3, "hi there you") is None  # nothing new
    assert reflex_due(3, "hi there you know") == 4
    assert reflex_due(3, "hi there. you know what?") == 5  # two words in one partial
    t = ReflexTracker(max_chunks=2)
    assert [t.due(x) for x in ["a b.", "a b. c", "a b. c d.", "a b. c d. e f."]] == [None, 3, 4, None]
    assert ReflexTracker().max == 120
    long = "word " * 200 + "the end"
    assert len(reflex_input(long)) <= 400 and reflex_input(long).endswith("the end")


def test_goemotions_fold_into_reactions():
    assert set(GO_MAP) == set(PICKS)
    pick, conf, p = fold({"surprise": 0.54, "excitement": 0.51, "joy": 0.14, "neutral": 0.02})
    assert (pick, conf) == ("surprised", 0.54) and p["happy"] == 0.51 and p["laugh"] == 0.0
    assert fold({"neutral": 0.92, "curiosity": 0.03})[0] == "none"
    assert fold({"curiosity": 0.53, "confusion": 0.43, "neutral": 0.34})[0] == "curious"


def test_category_rules_match_the_relay_and_the_model_adds_questions():
    # the same table as services/relay/test/reflex.test.ts "utterance category"
    assert rule_category("why do cats purr so much?") == "hard"
    assert rule_category("should I take the job or stay") == "hard"
    assert rule_category("remind me to call mom at five") == "request"
    assert rule_category("can you play some jazz?") == "request"
    assert rule_category("what time is it?") == "question"
    assert rule_category("is it raining") == "question"
    assert rule_category("I had a long day at work.") == "other"
    assert rule_category("what time is it? anyway I had a long day.") == "other"
    assert category("I wonder about the moon", {"curiosity": 0.7}) == ("question", 0.7)
    assert category("I had a long day", {"curiosity": 0.1}) == ("other", 0.9)


class LiveMoshi(FakeMoshi):
    """STT words while the audio still flows (the real server's way): three words on the 2nd chunk."""

    async def stt(self, ws):
        sess = {"samples": 0, "closed": False, "marker": None, "after": 0}
        self.stt_sessions.append(sess)
        n = 0
        try:
            async for m in ws:
                d = msgpack.unpackb(m)
                if d["type"] == "Audio" and sess["marker"] is None:
                    n += 1
                    if n == 2:
                        await ws.send(msgpack.packb({"type": "Word", "text": "hello", "start_time": 0.1}))
                        await ws.send(msgpack.packb({"type": "Word", "text": "there", "start_time": 0.3}))
                        await ws.send(msgpack.packb({"type": "Word", "text": "molty.", "start_time": 0.5}))
                elif d["type"] == "Audio":
                    sess["after"] += 1
                    if sess["after"] == 3:
                        await ws.send(msgpack.packb({"type": "Marker", "id": sess["marker"]}))
                elif d["type"] == "Marker":
                    sess["marker"] = d["id"]
        finally:
            sess["closed"] = True


class StubModel:
    def __init__(self):
        self.texts = []

    def classify(self, text):
        self.texts.append(text)
        return {"pick": "happy", "conf": 0.8, "p": {"happy": 0.8, "none": 0.1}, "ms": 12.5, "chars": len(text), "tokens": 5, "model": "stub"}


def _listen(reflex_model, start_extra):
    moshi = LiveMoshi()
    seen = {"frames": []}

    async def relay(ws):
        seen["hello"] = json.loads(await ws.recv())
        await ws.send(json.dumps({"t": "voice.welcome", "device_id": DEVICE, "mode": "auto", "voice": None}))
        await ws.send(json.dumps({"t": "stt.start", "id": "s1", "rate": 16000, **start_extra}))
        speech = np.full(3200, 3000, dtype="<i2").tobytes()
        for seq in range(3):
            await ws.send(framing.encode("s1", seq, speech))
            await asyncio.sleep(0.02)
        await asyncio.sleep(0.3)
        await ws.send(json.dumps({"t": "stt.end", "id": "s1"}))
        while True:
            d = json.loads(await ws.recv())
            seen["frames"].append(d)
            if d["t"] == "stt.final":
                break
        await asyncio.sleep(0.1)
        await ws.close()

    asyncio.run(with_servers(relay, moshi, bridge_kwargs={"reflex_model": reflex_model}))
    return seen


def test_stt_reflex_after_the_chunk_word_only_when_asked():
    model = StubModel()
    seen = _listen(model, {"reflex": True})
    assert seen["hello"]["reflex"] is True
    order = [(f["t"], f.get("text") or f.get("n")) for f in seen["frames"] if f["t"] in ("stt.partial", "stt.reflex")]
    # under three words nothing; the third word: n=3, its verdict right after that partial
    assert order[:4] == [("stt.partial", "hello"), ("stt.partial", "hello there"), ("stt.partial", "hello there molty."), ("stt.reflex", 3)]
    r = next(f for f in seen["frames"] if f["t"] == "stt.reflex")
    assert r == {"t": "stt.reflex", "id": "s1", "n": 3, "pick": "happy", "conf": 0.8, "p": {"happy": 0.8, "none": 0.1}, "ms": 12.5, "chars": 18, "tokens": 5, "model": "stub"}
    assert model.texts == ["hello there molty."]

    # a listen that did not ask: no classification at all
    model2 = StubModel()
    seen2 = _listen(model2, {})
    assert not [f for f in seen2["frames"] if f["t"] == "stt.reflex"] and model2.texts == []


def test_no_model_no_reflex_offered():
    seen = _listen(None, {"reflex": True})
    assert "reflex" not in seen["hello"]
    assert not [f for f in seen["frames"] if f["t"] == "stt.reflex"]


def test_a_failing_model_never_costs_the_turn():
    class Broken:
        def classify(self, text):
            raise RuntimeError("onnx exploded")

    seen = _listen(Broken(), {"reflex": True})
    assert seen["frames"][-1]["t"] == "stt.final" and seen["frames"][-1]["text"] == "hello there molty."


def test_hosted_verdict_uses_the_owners_key_and_marks_itself_hosted():
    seen = {}

    def post(url, body):
        seen.update(url=url, body=body)
        return {"model": "jev-1", "usage": {"input_tokens": 300}, "answers": {
            "reaction": {"choice": "happy", "confidence": 0.9, "probabilities": {"happy": 0.9, "none": 0.1, "odd": 1}},
            "category": {"choice": "other", "confidence": 0.8}}}

    v = reflex_hosted.HostedReflex("jev", "sk-secret", post=post).classify("I got the job")
    assert seen["url"] == reflex_hosted.JEV_URL and seen["body"]["state"]["human_said_so_far"] == "I got the job"
    assert "sk-secret" not in json.dumps(v)
    assert v["hosted"] is True and v["pick"] == "happy" and v["p"] == {"happy": 0.9, "none": 0.1}
    assert v["cat"] == "other" and v["tokens"] == 300 and v["chars"] == 13


def test_hosted_openai_answer_shape():
    answer = {"answers": [
        {"type": "choice", "name": "reaction", "choice": "sad", "confidence": 0.7, "probabilities": [{"value": "sad", "probability": 0.7}]},
        {"type": "choice", "name": "category", "choice": "hard", "confidence": 0.6}]}
    v = reflex_hosted.HostedReflex("openai", "k", post=lambda u, b: answer).classify("my dog died")
    assert (v["pick"], v["p"], v["cat"]) == ("sad", {"sad": 0.7}, "hard")


def test_hosted_junk_raises_so_the_relay_falls_back():
    with pytest.raises(ValueError):
        reflex_hosted.HostedReflex("openai", "k", post=lambda u, b: {"answers": [{"type": "refusal", "name": "reaction"}]}).classify("hello there you")


def test_hosted_refused_key_rests_instead_of_asking_at_every_word():
    calls = []

    def post(url, body):
        calls.append(url)
        raise urllib.error.HTTPError(url, 401, "no", {}, None)

    m = reflex_hosted.HostedReflex("jev", "bad", post=post)
    for _ in range(2):
        with pytest.raises(RuntimeError):
            m.classify("hello there you")
    assert len(calls) == 1


def test_hosted_load_picks_the_key_it_finds_and_none_without():
    assert reflex_hosted.load({}) is None
    assert reflex_hosted.load({"OPENAI_API_KEY": "a"}).engine == "openai"
    assert reflex_hosted.load({"OPENAI_API_KEY": "a", "TYPESAFE_API_KEY": "b"}).engine == "jev"
