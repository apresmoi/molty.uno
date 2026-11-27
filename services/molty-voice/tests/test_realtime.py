"""The realtime engine (the conversational front) through the bridge, against a fake relay and a fake provider."""

import asyncio
import json
import logging

import numpy as np
from websockets.asyncio.server import serve

from molty_voice import brief, framing
from molty_voice.bridge import Bridge
from molty_voice.config import Config, load, save
from molty_voice.fake_realtime import FakeRealtime
from molty_voice.front import UsageBook

DEVICE = "molty-k7m2q9xa4c"
TOKEN = DEVICE + "." + "A" * 43
KEY = "test-provider-key-never-logged"


def speech(ms: int, level: int = 6000) -> bytes:
    n = 16 * ms
    return (np.sin(np.arange(n) * 2 * np.pi * 200 / 16000) * level).astype("<i2").tobytes()


def silence(ms: int) -> bytes:
    return bytes(32 * ms)


class Relay:
    """The relay's side of the voice socket: records everything the box sends."""

    def __init__(self):
        self.hellos: list[dict] = []
        self.msgs: list[dict] = []
        self.audio: dict[str, bytes] = {}
        self.ws = None
        self.got = asyncio.Event()

    async def handler(self, ws):
        self.ws = ws
        self.hellos.append(json.loads(await ws.recv()))
        await ws.send(json.dumps({"t": "voice.welcome", "device_id": DEVICE, "mode": "auto", "voice": None}))
        async for m in ws:
            if isinstance(m, bytes):
                f = framing.decode(m)
                self.audio[f.s] = self.audio.get(f.s, b"") + f.payload
            else:
                d = json.loads(m)
                if d["t"] == "voice.hello":
                    self.hellos.append(d)
                elif d["t"] != "ping":
                    self.msgs.append(d)
            self.got.set()

    async def send(self, obj):
        await self.ws.send(json.dumps(obj))

    async def mic(self, sid: str, pcm: bytes, chunk_ms: int = 100):
        step = 32 * chunk_ms
        for seq, i in enumerate(range(0, len(pcm), step)):
            await self.ws.send(framing.encode(sid, seq, pcm[i : i + step]))
            await asyncio.sleep(chunk_ms / 1000 / 4)  # 4x real time: the fake's detector counts audio, not wall time

    def of(self, t: str, **match):
        return [m for m in self.msgs if m["t"] == t and all(m.get(k) == v for k, v in match.items())]

    async def until(self, pred, timeout: float = 8.0):
        async def wait():
            while not pred():
                self.got.clear()
                await self.got.wait()
        await asyncio.wait_for(wait(), timeout)


async def run(scenario, cfg_extra=None, fake_kw=None, usage_path=None):
    async with FakeRealtime(**(fake_kw or {})) as fake:
        relay = Relay()
        async with serve(relay.handler, "127.0.0.1", 0) as rs:
            cfg = Config(relay=f"http://127.0.0.1:{rs.sockets[0].getsockname()[1]}", device_id=DEVICE, token=TOKEN, name="laptop",
                         engine="realtime", realtime_key=KEY, realtime_url=fake.url, **(cfg_extra or {}))
            bridge = Bridge(cfg, usage_path=usage_path)
            task = asyncio.create_task(bridge.serve_once())
            await asyncio.wait_for(bridge.connected.wait(), 5)
            try:
                await scenario(relay, fake, bridge)
            finally:
                await relay.ws.close()
                await asyncio.wait_for(task, 5)
        return relay, fake, bridge


def test_hello_offers_the_front_and_no_gpu_stack():
    async def scenario(relay, fake, bridge):
        pass

    relay, fake, _ = asyncio.run(run(scenario))
    h = relay.hellos[0]
    assert h["front"] == 1 and h["engine"] == "realtime" and h["stt"] is True and h["tts"] is True
    assert h["voices"][0] == "marin"
    assert fake.sessions == []  # nothing is opened (nor billed) until a conversation starts


def test_a_front_turn_ends_on_the_model_and_its_reply_streams_back():
    async def scenario(relay, fake, bridge):
        await relay.send({"t": "stt.start", "id": "s1", "rate": 16000, "front": True, "no_speech_ms": 5000})
        await relay.mic("s1", speech(600) + silence(900))
        await relay.until(lambda: relay.of("stt.final", id="s1") and relay.of("tts.end"))
        await relay.send({"t": "stt.end", "id": "s1"})  # the relay ends the listen after the turn end, as it does

    relay, fake, _ = asyncio.run(run(scenario, fake_kw={"transcript": "what is on my calendar?", "reply": lambda h: ("Let me check on that.", True)}))
    s = fake.sessions[0]
    assert s["auth"] is True
    up = s["updates"][0]
    assert up["instructions"] == brief.BRIEF and up["tools"] == brief.TOOLS
    assert up["audio"]["input"]["format"] == {"type": "audio/pcm", "rate": 24000}
    assert up["audio"]["output"]["format"] == {"type": "audio/pcm", "rate": 24000}
    # turn detection only while the listen runs: off at the open, on at its start, off at its end
    assert up["audio"]["input"]["turn_detection"] is None
    tds = [u["audio"]["input"]["turn_detection"] for u in s["updates"][1:]]
    # create_response off: the box asks for the one reply itself, on the commit (rule 1)
    assert tds[0]["type"] == "semantic_vad" and tds[0]["create_response"] is False and tds[0]["interrupt_response"] is False
    assert len(s["creates"]) == 1
    assert "max_output_tokens" not in s["creates"][0]  # no cap: live, it cut replies before their hand-off call
    assert tds[1:] == [None]
    # 16 kHz in -> 24 kHz to the provider (x1.5), streamed; nothing after the model ended the turn
    # (0.6 s of speech + its 0.5 s of silence, of the 1.5 s Molty recorded)
    assert 24 * 1050 <= s["appended"] <= 24 * 1300
    assert relay.of("stt.hint", id="s1", k="speech")
    assert relay.of("stt.turn_end", id="s1") and "reason" not in relay.of("stt.turn_end", id="s1")[0]
    reply = relay.of("front.reply")[0]
    assert reply["stt"] == "s1"
    rid = reply["id"]
    # the model said "Let me check on that." (5 words at 300 ms = 1.5 s @24 kHz) -> 1.5 s @16 kHz
    assert abs(len(relay.audio[rid]) // 2 - 16000 * 1.5) < 400
    assert [w["i"] for w in relay.of("tts.word", id=rid)] == [0, 1, 2, 3, 4]
    starts = [w["start_s"] for w in relay.of("tts.word", id=rid)]
    assert starts == sorted(starts)
    assert relay.of("tts.text", id=rid)[-1] == {"t": "tts.text", "id": rid, "text": "Let me check on that.", "final": True}
    end = relay.of("tts.end", id=rid)[0]
    assert end["wait"] is True and "error" not in end  # the front handed the turn to the agent
    assert relay.of("stt.final", id="s1")[0]["text"] == "what is on my calendar?"
    assert "function_call_output" in s["items"]  # the tool call is answered, so the conversation stays valid


def test_the_agent_speaking_up_between_conversations_is_voiced_exact_and_the_queue():
    async def scenario(relay, fake, bridge):
        await relay.send({"t": "tts", "id": "t1", "text": "Your next meeting is at 3 pm.", "front": "inner"})
        await relay.send({"t": "tts", "id": "t2", "text": "Build 42 passed.", "front": "inner", "exact": True})  # while t1 speaks: queued
        await relay.send({"t": "front.context", "text": "The build log is 200 lines long."})
        await relay.send({"t": "tts", "id": "t3", "text": "hi"})  # plain tts (the front off) = verbatim
        await relay.until(lambda: len(relay.of("tts.end")) == 3)

    relay, fake, _ = asyncio.run(run(scenario))
    s = fake.sessions[0]
    c0, c1, c2 = s["creates"]
    # the agent speaking up: read aloud out of band, exactly its words (no bridge: nobody was waiting)
    assert c0 == {"conversation": "none", "output_modalities": ["audio"], "instructions": brief.READ_ALOUD, "tool_choice": "none",
                  "input": [{"type": "message", "role": "user", "content": [{"type": "input_text", "text": "Your next meeting is at 3 pm."}]}],
                  "metadata": {"molty": "t1"}}
    assert c1["input"][0]["content"][0]["text"] == "Build 42 passed."
    assert c2["instructions"] == brief.exactly("hi") and "conversation" not in c2
    assert "- The build log is 200 lines long." in s["updates"][-1]["instructions"]  # background in the instructions...
    assert not any("build log" in str(i) for i in s["items"])  # ...never an item a model may recite
    assert relay.of("tts.text", id="t1")[-1]["text"] == "Your next meeting is at 3 pm."
    assert relay.of("tts.text", id="t2")[-1]["text"] == "Build 42 passed."  # exact: its wording kept
    assert [m["id"] for m in relay.of("tts.end")] == ["t1", "t2", "t3"]
    assert all("error" not in m and "absorbed" not in m for m in relay.of("tts.end"))


def test_an_agent_line_while_the_person_talks_is_context_and_the_turn_gets_its_one_reply():
    async def scenario(relay, fake, bridge):
        await relay.send({"t": "stt.start", "id": "s1", "rate": 16000, "front": True})
        await relay.mic("s1", speech(400))
        await relay.send({"t": "tts", "id": "t1", "text": "Done.", "front": "inner"})  # the agent, while the person still talks
        await relay.mic("s1", speech(200) + silence(700))
        await relay.until(lambda: relay.of("tts.end", id="t1") and relay.of("front.reply") and relay.of("tts.end", id=relay.of("front.reply")[0]["id"]))

    relay, fake, _ = asyncio.run(run(scenario))
    assert relay.of("tts.end", id="t1")[0] == {"t": "tts.end", "id": "t1", "absorbed": True}
    assert "t1" not in relay.audio and len(fake.sessions[0]["creates"]) == 1  # only the turn's own reply


def test_a_plain_listen_only_transcribes():
    async def scenario(relay, fake, bridge):
        await relay.send({"t": "stt.start", "id": "s2", "rate": 16000})
        await relay.mic("s2", speech(500) + silence(700))
        await relay.until(lambda: relay.of("stt.final", id="s2"))
        await asyncio.sleep(0.4)

    relay, fake, _ = asyncio.run(run(scenario))
    assert fake.sessions[0]["updates"][1]["audio"]["input"]["turn_detection"]["create_response"] is False
    assert relay.of("stt.final", id="s2")[0]["text"] == "hello molty"
    assert not relay.of("front.reply") and not fake.sessions[0]["creates"]


def test_nobody_spoke_and_a_tap_ended_turn():
    async def scenario(relay, fake, bridge):
        await relay.send({"t": "stt.start", "id": "s3", "rate": 16000, "front": True, "no_speech_ms": 1000})
        await relay.mic("s3", silence(1300))
        await relay.until(lambda: relay.of("stt.turn_end", id="s3"))
        await relay.send({"t": "stt.cancel", "id": "s3"})
        # the person talks and taps before the model's detector ended the turn: the box commits and answers
        await relay.send({"t": "stt.start", "id": "s4", "rate": 16000, "front": True})
        await relay.mic("s4", speech(500))
        await relay.send({"t": "stt.end", "id": "s4"})
        await relay.until(lambda: relay.of("stt.final", id="s4") and relay.of("tts.end"))

    relay, fake, _ = asyncio.run(run(scenario, fake_kw={"transcript": "hi"}))
    assert relay.of("stt.turn_end", id="s3")[0]["reason"] == "no_speech"
    assert fake.sessions[0]["commits"] == 1
    assert relay.of("front.reply")[0]["stt"] == "s4"
    assert relay.of("stt.final", id="s4")[0]["text"] == "hi"


def test_idle_and_end_close_the_session_with_its_usage(tmp_path):
    book = tmp_path / "realtime-usage.json"

    async def scenario(relay, fake, bridge):
        await relay.send({"t": "tts", "id": "t1", "text": "hello", "front": "exact"})
        await relay.until(lambda: relay.of("front.session", state="closed"), timeout=6)
        await relay.send({"t": "tts", "id": "t2", "text": "again", "front": "exact"})  # a new session opens
        await relay.until(lambda: relay.of("tts.end", id="t2"))
        await relay.send({"t": "front.end", "reason": "agent"})
        await relay.until(lambda: len(relay.of("front.session", state="closed")) == 2)

    relay, fake, _ = asyncio.run(run(scenario, cfg_extra={"realtime_idle_s": 0.5}, usage_path=book))
    closed = relay.of("front.session", state="closed")
    assert [c["reason"] for c in closed] == ["idle", "agent"]
    assert closed[0]["usage"]["responses"] == 1 and closed[0]["usage"]["audio_out"] > 0
    assert len(fake.sessions) == 2 and all(s["closed"] for s in fake.sessions)
    kept = json.loads(book.read_text())
    assert len(kept["sessions"]) == 2 and kept["seconds"] > 0
    assert (book.stat().st_mode & 0o777) == 0o600


def test_the_daily_cap_closes_the_session_and_stops_offering(tmp_path):
    async def scenario(relay, fake, bridge):
        await relay.send({"t": "stt.start", "id": "s1", "rate": 16000, "front": True})
        await relay.until(lambda: relay.of("front.session", state="closed"), timeout=6)
        await relay.until(lambda: len(relay.hellos) >= 2)
        await relay.send({"t": "stt.start", "id": "s2", "rate": 16000, "front": True})
        await relay.send({"t": "tts", "id": "t9", "text": "x"})
        await relay.until(lambda: relay.of("stt.final", id="s2") and relay.of("tts.end", id="t9"))

    # 0.02 min = 1.2 s a day
    relay, fake, _ = asyncio.run(run(scenario, cfg_extra={"realtime_daily_minutes": 0.02}, usage_path=tmp_path / "u.json"))
    assert relay.of("front.session", state="closed")[0]["reason"] == "daily_cap"
    assert relay.of("stt.final", id="s1")[0]["error"].startswith("realtime session closed")
    h = relay.hellos[-1]
    assert h["stt"] is False and h["tts"] is False and "front" not in h  # the relay falls back to today's path
    assert "error" in relay.of("stt.final", id="s2")[0] and "error" in relay.of("tts.end", id="t9")[0]
    assert len(fake.sessions) == 1  # nothing more was opened today


def test_the_key_stays_out_of_logs_reprs_and_saved_config(tmp_path, caplog, monkeypatch):
    caplog.set_level(logging.DEBUG)

    async def scenario(relay, fake, bridge):
        await relay.send({"t": "tts", "id": "t1", "text": "hello", "front": "exact"})
        await relay.until(lambda: relay.of("tts.end", id="t1"))

    relay, fake, bridge = asyncio.run(run(scenario))
    assert KEY not in caplog.text
    assert KEY not in repr(bridge.cfg) and "<set>" in repr(bridge.cfg)
    assert all(KEY not in json.dumps(m) for m in relay.msgs + relay.hellos)
    # a key from the environment is used but never written into the config file
    monkeypatch.setenv("MOLTY_REALTIME_KEY", "env-key-xyz")
    p = tmp_path / "config.json"
    cfg = Config(engine="realtime")
    assert cfg.front_key() == "env-key-xyz"
    save(cfg, p)
    assert "env-key-xyz" not in p.read_text() and load(p).realtime_key == ""


def test_usage_book_rolls_over_a_day(tmp_path):
    b = UsageBook.load(tmp_path / "u.json")
    b.add(90, {"s": 90})
    assert b.used_s() == 90
    b.day = "2000-01-01"
    assert b.used_s() == 0


def test_a_turn_molty_ends_itself_is_answered_at_once():
    # live 2026-11-27: the person held the button, spoke, let go; the box committed but no reply came
    # until the agent's answer, 16 s later. Whoever ends the turn, the front answers it now.
    async def scenario(relay, fake, bridge):
        await relay.send({"t": "stt.start", "id": "s1", "rate": 16000, "front": True, "no_speech_ms": 8000})
        await relay.mic("s1", speech(800))  # the model's detector has not ended it yet
        await relay.send({"t": "stt.end", "id": "s1"})  # Molty did (a tap, the button let go)
        await relay.until(lambda: relay.of("front.reply") and relay.of("tts.end"), timeout=3)

    relay, fake, _ = asyncio.run(run(scenario, fake_kw={"transcript": "hi there"}))
    assert relay.of("front.reply")[0]["stt"] == "s1"
    assert fake.sessions[0]["commits"] == 1 and len(fake.sessions[0]["creates"]) == 1


def test_a_held_listen_is_never_cut_by_the_model_and_is_answered_on_release():
    async def scenario(relay, fake, bridge):
        await relay.send({"t": "stt.start", "id": "h1", "rate": 16000, "front": True, "hold": True})
        await relay.mic("h1", speech(400) + silence(1000) + speech(400))  # a pause while the button is held
        await asyncio.sleep(0.3)
        relay.early = [m["t"] for m in relay.msgs if m["t"] in ("stt.turn_end", "front.reply")]
        await relay.send({"t": "stt.end", "id": "h1"})
        await relay.until(lambda: relay.of("tts.end") and relay.of("stt.final", id="h1"), timeout=3)

    relay, fake, _ = asyncio.run(run(scenario))
    assert relay.early == []
    s = fake.sessions[0]
    assert s["commits"] == 1 and 24 * 1700 <= s["appended"] <= 24 * 1900  # all of it reached the model, one turn
    assert relay.of("front.reply")[0]["stt"] == "h1"


def test_the_next_listen_is_not_ended_by_the_last_turn():
    # live: the follow up listen ended after 1.7 s of nobody speaking: the detector was still in the
    # first turn's speech (Molty ended that turn itself) and its late end of turn hit the new listen
    async def scenario(relay, fake, bridge):
        await relay.send({"t": "stt.start", "id": "s1", "rate": 16000, "front": True, "no_speech_ms": 8000})
        await relay.mic("s1", speech(800))
        await relay.send({"t": "stt.end", "id": "s1"})
        await relay.until(lambda: relay.of("tts.end"), timeout=3)
        await relay.send({"t": "stt.start", "id": "s2", "rate": 16000, "front": True, "no_speech_ms": 3000})
        await relay.mic("s2", silence(2500))
        await asyncio.sleep(0.3)
        relay.early = relay.of("stt.turn_end", id="s2") + relay.of("front.reply", stt="s2")
        await relay.mic("s2", silence(700))
        await relay.until(lambda: relay.of("stt.turn_end", id="s2"), timeout=3)

    relay, fake, _ = asyncio.run(run(scenario))
    assert relay.early == []
    assert relay.of("stt.turn_end", id="s2")[0]["reason"] == "no_speech"
    assert not relay.of("front.reply", stt="s2")


def test_the_agents_line_is_said_as_its_own_cues_never_spoken():
    # live: "Hey Juan! [happy] All good here..." came out as "Okay, that's a neat vibe, let me translate that briefly."
    async def scenario(relay, fake, bridge):
        await relay.send({"t": "tts", "id": "t1", "text": "Hey Juan! [happy] All good here, and yeah, pretty fast today.", "front": "inner"})
        await relay.until(lambda: relay.of("tts.end", id="t1"))

    relay, fake, _ = asyncio.run(run(scenario))
    c = fake.sessions[0]["creates"][0]
    assert c["input"][0]["content"][0]["text"] == "Hey Juan! All good here, and yeah, pretty fast today." and c["tool_choice"] == "none"
    for rule in ("exactly as written, word for word", "Say nothing before it and nothing after it", "never answer it"):
        assert rule in c["instructions"]
    assert not any(str(i).startswith("Inner voice") for i in fake.sessions[0]["items"])
    assert relay.of("tts.text", id="t1")[-1]["text"] == "Hey Juan! All good here, and yeah, pretty fast today."


async def _turn(relay, sid: str, ms: int = 500):
    """One person's turn the model ends; the relay ends the listen after it, as it does."""
    await relay.send({"t": "stt.start", "id": sid, "rate": 16000, "front": True})
    await relay.mic(sid, speech(ms) + silence(700))
    await relay.until(lambda: relay.of("front.reply", stt=sid) and relay.of("tts.end", id=relay.of("front.reply", stt=sid)[0]["id"]))
    await relay.send({"t": "stt.end", "id": sid})


def test_the_front_answered_a_turn_itself_the_agents_line_for_it_is_only_context():
    # live 2026-11-27: the front greeted, then the agent's own greeting for that turn was said as a second answer
    async def scenario(relay, fake, bridge):
        await _turn(relay, "s1")  # "hello molty": the front greets
        await relay.send({"t": "tts", "id": "t1", "text": "Hey Juan! Just lounging on your desk.", "front": "inner", "turn": "s1"})
        await relay.send({"t": "tts", "id": "t2", "text": "By the way, your build passed.", "front": "inner"})  # no turn, mid conversation
        await relay.until(lambda: relay.of("tts.end", id="t1") and relay.of("tts.end", id="t2"))

    relay, fake, _ = asyncio.run(run(scenario))
    for t in ("t1", "t2"):
        assert relay.of("tts.end", id=t)[0].get("absorbed") is True and t not in relay.audio
    s = fake.sessions[0]
    assert len(s["creates"]) == 1  # the turn's one reply, nothing else
    assert "- Your inner voice said: Hey Juan! Just lounging on your desk." in s["updates"][-1]["instructions"]


def test_a_question_it_cannot_answer_is_a_filler_naming_it_then_the_agents_answer_once():
    async def scenario(relay, fake, bridge):
        await _turn(relay, "s1")
        await relay.send({"t": "tts", "id": "w1", "text": "", "front": "wait"})
        await relay.send({"t": "tts", "id": "t1", "text": "You have two new emails, both from Ana.", "front": "inner", "turn": "s1"})
        await relay.until(lambda: relay.of("tts.end", id="t1"))
        await relay.send({"t": "tts", "id": "t2", "text": "One is about Friday.", "front": "inner", "turn": "s1"})
        await relay.until(lambda: relay.of("tts.end", id="t2"))

    relay, fake, _ = asyncio.run(run(scenario, fake_kw={"transcript": "any new emails?"}))
    reply = relay.of("front.reply")[0]["id"]
    assert relay.of("tts.end", id=reply)[0]["wait"] is True
    assert "new emails" in relay.of("tts.text", id=reply)[-1]["text"]  # the holding line names the task
    creates = fake.sessions[0]["creates"]
    # the answer came while "still on it" was being made: that filler never plays, the answer does at once
    assert relay.of("tts.end", id="w1")[0].get("absorbed") is True and "w1" not in relay.audio
    said = creates[-1]["input"][0]["content"][0]["text"]
    assert said.endswith(" You have two new emails, both from Ana.") and said.split(" You have")[0] in brief.BRIDGES  # the box's own bridge
    assert relay.of("tts.text", id="t1")[-1]["text"] == said
    assert relay.of("tts.end", id="t2")[0].get("absorbed") is True  # no longer waiting: context


def test_a_new_turn_wins_the_answer_to_the_turn_before_is_context():
    async def scenario(relay, fake, bridge):
        await _turn(relay, "s1")  # handed off
        await _turn(relay, "s2")  # the person speaks again before the agent answered
        await relay.send({"t": "tts", "id": "t1", "text": "You have two new emails.", "front": "inner", "turn": "s1"})
        await relay.until(lambda: relay.of("tts.end", id="t1"))

    relay, fake, _ = asyncio.run(run(scenario, fake_kw={"transcript": "any new emails?"}))
    assert relay.of("tts.end", id="t1")[0].get("absorbed") is True
    assert len(relay.of("front.reply")) == 2 and len(fake.sessions[0]["creates"]) == 2  # one reply per turn, no more


def test_the_brief_sets_filler_by_default_and_hand_off_for_real_questions():
    for rule in ("Answer yourself ONLY when you already have what it takes", "When unsure, hand off", "echoes a few of the person's own words",
                 "Never a bare \"one moment\"", "agent by name", "speak as it", "ONE short spoken sentence, at most 12 words",
                 "You are not an assistant", "never offer help", "Never explain a cause", "any complaint about how Molty behaves",
                 "Without one, be neutral and brief"):
        assert rule in brief.BRIEF
    assert brief.TOOLS[0]["parameters"]["required"] == ["request"]


def test_the_agents_lines_and_persona_outlive_the_session_as_background_never_said():
    # live 2026-11-27: memory sent as items to a new session was recited ("Just lounging on your desk...") a second time
    async def scenario(relay, fake, bridge):
        await relay.send({"t": "front.context", "text": "Dude is a cat who lounges on the desk; dry humour.", "persona": True})
        await relay.send({"t": "tts", "id": "t1", "text": "Just lounging on your desk.", "front": "inner"})
        await relay.until(lambda: relay.of("tts.end", id="t1"))
        await relay.send({"t": "front.end", "reason": "idle"})
        await relay.until(lambda: relay.of("front.session", state="closed"))
        await relay.send({"t": "tts", "id": "t2", "text": "Your build passed.", "front": "inner"})
        await relay.until(lambda: relay.of("tts.end", id="t2"))

    relay, fake, _ = asyncio.run(run(scenario))
    second = fake.sessions[1]
    ins = second["updates"][0]["instructions"]
    assert "\nPersona: Dude is a cat who lounges on the desk; dry humour." in ins
    assert "never say it again, never recite it):\n- Your inner voice said: Just lounging on your desk." in ins
    assert not any("lounging" in str(i) for i in second["items"])
    assert relay.of("tts.text", id="t2")[-1]["text"] == "Your build passed."  # said once, nothing replayed


def test_a_reply_that_waited_in_the_queue_counts_its_first_audio_from_its_creation():
    # live 2026-11-27: a turn Molty ended (the button let go) logged its first audio 566 ms after the turn's
    # end, but front.reply ms.created (211) + tts.end ms.first_audio (565) told the relay 776 ms
    async def scenario(relay, fake, bridge):
        await relay.send({"t": "tts", "id": "t1", "text": "Your next meeting is at three in the afternoon.", "front": "inner"})
        await relay.until(lambda: relay.of("tts.word", id="t1"))
        await relay.send({"t": "stt.start", "id": "h1", "rate": 16000, "front": True, "hold": True})
        await relay.mic("h1", speech(300))
        await relay.send({"t": "stt.end", "id": "h1"})  # the release; the reply waits for t1 to end
        relay.t_end = asyncio.get_running_loop().time()
        await relay.until(lambda: relay.of("front.reply") and relay.of("tts.end", id=relay.of("front.reply")[0]["id"]), timeout=5)

    relay, fake, _ = asyncio.run(run(scenario, fake_kw={"first_audio_ms": 300}))
    reply = relay.of("front.reply")[0]
    created, first = reply["ms"]["created"], relay.of("tts.end", id=reply["id"])[0]["ms"]["first_audio"]
    assert created > 300  # it waited behind t1
    assert 280 <= first < 450  # the model's own time to its first audio, once asked


def test_context_with_no_session_opens_none_and_reaches_the_next_one():
    # live 2026-11-27: a late answer must never open a (billed) session by itself
    async def scenario(relay, fake, bridge):
        await relay.send({"t": "front.context", "text": "The agent said: purring along nicely."})
        await relay.send({"t": "tts", "id": "t0", "text": "Mrrp, I am listening.", "front": "inner", "turn": "s9"})  # a turn the front never waited for
        await relay.until(lambda: relay.of("tts.end", id="t0"))
        await asyncio.sleep(0.3)
        relay.sessions_then = len(fake.sessions)
        await relay.send({"t": "tts", "id": "t1", "text": "hi", "front": "inner"})
        await relay.until(lambda: relay.of("tts.end", id="t1"))

    relay, fake, _ = asyncio.run(run(scenario))
    assert relay.sessions_then == 0 and relay.of("tts.end", id="t0")[0].get("absorbed") is True
    assert "- The agent said: purring along nicely." in fake.sessions[0]["updates"][0]["instructions"]
    assert relay.of("tts.text", id="t1")[-1]["text"] == "hi"


def test_word_cues_and_texts_line_up_with_the_whole_line():
    # the relay forwards each tts.text as the device's say_text (whole words, each continuing the last):
    # cue i must be word i of that text, for every word, whatever its punctuation
    line = "Hey Juan! It's 3:45, you've got 2 e-mails (both from Ana)... want them?"

    async def scenario(relay, fake, bridge):
        await relay.send({"t": "tts", "id": "t1", "text": line, "front": "inner", "exact": True})
        await relay.until(lambda: relay.of("tts.end", id="t1"))
        await relay.send({"t": "tts", "id": "t2", "text": "Hey Juan, all good.", "front": "inner", "exact": True})
        await relay.until(lambda: relay.of("tts.end", id="t2"))

    relay, fake, _ = asyncio.run(run(scenario, fake_kw={"final": lambda w: w.replace("Hey", "Hi", 1).replace("  ", " ")}))
    for sid, said in (("t1", line), ("t2", "Hey Juan, all good.")):
        assert [w["i"] for w in relay.of("tts.word", id=sid)] == list(range(len(said.split())))
        assert [w["text"] for w in relay.of("tts.word", id=sid)] == said.split()
        texts = [m["text"] for m in relay.of("tts.text", id=sid)]
        for a, b in zip(texts, texts[1:]):
            assert b == a or b.startswith(a + " ")
        assert texts[-1] == said  # a final transcript that restates earlier words never replaces them


def test_every_turn_end_gets_its_one_reply_even_when_the_listen_ends_at_once():
    # live 2026-11-27 (run 5, turn 2): the model ended the turn, the relay ended the listen at once (turn detection
    # off), and no reply ever came; then the agent's line for the turn before was said instead
    async def scenario(relay, fake, bridge):
        await relay.send({"t": "stt.start", "id": "s1", "rate": 16000, "front": True})
        await relay.mic("s1", speech(500) + silence(700))
        await relay.until(lambda: relay.of("stt.turn_end", id="s1"))
        await relay.send({"t": "stt.end", "id": "s1"})  # as the relay does, at once
        await relay.until(lambda: relay.of("front.reply", stt="s1") and relay.of("tts.end", id=relay.of("front.reply", stt="s1")[0]["id"]), timeout=3)

    relay, fake, _ = asyncio.run(run(scenario, fake_kw={"auto_create_ms": 300}))
    assert len(relay.of("front.reply", stt="s1")) == 1 and len(fake.sessions[0]["creates"]) == 1


def _ends(order: str):
    async def scenario(relay, fake, bridge):
        await relay.send({"t": "stt.start", "id": "s1", "rate": 16000, "front": True})
        if order == "model_first":
            await relay.mic("s1", speech(500) + silence(700))
            await relay.until(lambda: relay.of("stt.turn_end", id="s1"))
        else:
            await relay.mic("s1", speech(600))  # still speaking when Molty's end comes
        await relay.send({"t": "stt.end", "id": "s1"})  # the device's end of the same turn, at once
        await relay.until(lambda: relay.of("front.reply", stt="s1") and relay.of("tts.end", id=relay.of("front.reply", stt="s1")[0]["id"]), timeout=3)
        await asyncio.sleep(0.3)

    return scenario


def test_the_models_end_and_the_devices_end_of_one_turn_give_one_commit_and_one_reply_either_order():
    # live 2026-11-27 (rig run, turn 3 "Hmm"): the model ended the turn and 2 ms later Molty's end came; the box
    # committed again (empty: an error), that error failed the reply, and the reply was then cancelled: no answer
    for order, kw in (("model_first", {"commit_event_ms": 60}), ("device_first", {"ends_together": True})):
        relay, fake, _ = asyncio.run(run(_ends(order), fake_kw={"transcript": "hmm", **kw}))
        s = fake.sessions[0]
        replies = relay.of("front.reply", stt="s1")
        assert len(replies) == 1, order
        end = relay.of("tts.end", id=replies[0]["id"])[0]
        assert "error" not in end and replies[0]["id"] in relay.audio, order  # it was said
        assert s["commits"] == 1 and len(s["creates"]) == 1 and s["cancels"] == 0, order


def test_a_waiting_answer_is_read_aloud_and_checked_a_deviation_is_reported():
    # live 2026-11-27 (rig run 2, turn 4): the agent said "You have the team standup at ten and the dentist at three."
    # and the front said "I can't see your calendar. Sorry."
    async def scenario(relay, fake, bridge):
        await _turn(relay, "s1")
        await relay.send({"t": "tts", "id": "t1", "text": "You have the team standup at ten and the dentist at three.", "front": "inner", "turn": "s1"})
        await relay.until(lambda: relay.of("tts.end", id="t1"))

    for misread, faithful in ((None, True), (lambda t: "I can't see your calendar. Sorry.", False)):
        relay, fake, _ = asyncio.run(run(scenario, fake_kw={"transcript": "what is on my calendar?", **({"misread": misread} if misread else {})}))
        end = relay.of("tts.end", id="t1")[0]
        assert end.get("faithful", True) is faithful and "error" not in end
        s = fake.sessions[0]
        assert s["creates"][-1]["conversation"] == "none"
        # what was said joins the conversation as the front's own message, so later turns know it
        assert s["items"][-1] == relay.of("tts.text", id="t1")[-1]["text"]


def test_faithful_holds_every_number_name_and_content_word():
    ok = brief.faithful
    asked = "You have the team standup at ten and the dentist at three."
    assert ok(asked, "Ok, got it: You have the team standup at 10 and the dentist at 3.")[0]
    assert not ok(asked, "I can't see your calendar. Sorry.")[0]
    assert not ok(asked, "Ok, got it: You have the team standup at ten and the dentist at four.")[0]
    juan = "That was me answering a turn twice, Juan. Sorry, it should be fixed now."
    assert not ok(juan, "Thanks for pointing it out; it's being handled now.")[0]
    assert ok("You asked me to remember to buy oat milk on the way home.", "You asked me: remember to buy oat milk on the way home.")[0]


def test_filler_by_default_a_question_is_handed_off_by_rule_small_talk_is_answered():
    # live 2026-11-28 (rig run 4): the model answered "What is on my calendar today?" itself (a token cap cut it
    # before its hand-off call): the box hands off by rule whatever the model does; small talk stays the front's
    async def scenario(relay, fake, bridge):
        await _turn(relay, "s1")

    for heard, waiting in (("What is on my calendar today?", True), ("Thanks, and what was the last thing I asked you to remember?", True),
                           ("It seems that you are speaking twice.", True), ("Hey Molty, how is it going?", False), ("Okay, bye.", False)):
        relay, fake, _ = asyncio.run(run(scenario, fake_kw={"transcript": heard, "reply": lambda h: ("Hmm, okay.", False)}))
        reply = relay.of("front.reply", stt="s1")[0]["id"]
        assert relay.of("tts.end", id=reply)[0].get("wait", False) is waiting, heard


def test_small_talk_is_only_greetings_thanks_goodbyes_and_hums():
    for t in ("Hey Molty, how is it going?", "hum", "Okay, bye.", "Thanks.", "good morning molty", ""):
        assert brief.answers_alone(t), t
    for t in ("What is on my calendar today?", "It seems that you are speaking twice.", "hey dude, what time is it?",
              "Thanks, and what was the last thing I asked you to remember?", "remind me to call Ana"):
        assert not brief.answers_alone(t), t


def test_still_on_it_is_a_fixed_line_naming_the_task_never_free_words():
    # live 2026-11-28 (rig run 6): the free "still waiting" line said "Sorry, I'm still hearing double. Please continue."
    async def scenario(relay, fake, bridge):
        await _turn(relay, "s1")
        await relay.send({"t": "tts", "id": "w1", "text": "", "front": "wait"})
        await relay.until(lambda: relay.of("tts.end", id="w1"))

    relay, fake, _ = asyncio.run(run(scenario, fake_kw={"transcript": "what is on my calendar today?"}))
    c = fake.sessions[0]["creates"][-1]
    assert c["instructions"] == brief.READ_ALOUD and c["conversation"] == "none"
    assert relay.of("tts.text", id="w1")[-1]["text"] in ("Still checking your calendar.", "Still on your calendar, one moment.")
    assert brief.still_line("hmm, are you there?") in ("Still on it, one moment.", "Still checking, one moment.")


def test_a_handed_off_reply_that_said_nothing_is_followed_at_once_by_a_holding_line():
    # live 2026-11-28 (rig run 6, turn 2): the model called hand_to_agent and said nothing: 8 s of silence
    async def scenario(relay, fake, bridge):
        await _turn(relay, "s1")

    for hold, expect in ((None, "Okay, it seems that, let me check."), (lambda h: "", None)):
        relay, fake, _ = asyncio.run(run(scenario, fake_kw={"transcript": "it seems that you are speaking twice",
                                                            "reply": lambda h: ("", True), **({"hold": hold} if hold else {})}))
        rid = relay.of("front.reply", stt="s1")[0]["id"]
        assert len(relay.of("front.reply")) == 1  # one reply, one stream: the holding line rides in it
        end = relay.of("tts.end", id=rid)[0]
        assert end["wait"] is True and "error" not in end and rid in relay.audio
        text = relay.of("tts.text", id=rid)[-1]["text"]
        assert text == expect if expect else text in brief.HOLD_LINES  # a fixed line when that is empty too


def test_a_bare_hum_gets_a_minimal_acknowledgement():
    # live 2026-11-28: "hum" got "That sounds like a thought, go ahead when you're ready."
    async def scenario(relay, fake, bridge):
        await _turn(relay, "s1", ms=300)

    relay, fake, _ = asyncio.run(run(scenario, fake_kw={"transcript": "hum", "reply": lambda h: ("That sounds like a thought, go ahead when you're ready.", False)}))
    rid = relay.of("front.reply", stt="s1")[0]["id"]
    assert relay.of("tts.text", id=rid)[-1]["text"] == brief.ACK
    assert brief.is_hum("Hmm.") and brief.is_hum("okay") and not brief.is_hum("okay, bye") and not brief.is_hum("what?")
