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

    relay, fake, _ = asyncio.run(run(scenario, fake_kw={"transcript": "what is on my calendar?"}))
    s = fake.sessions[0]
    assert s["auth"] is True
    up = s["updates"][0]
    assert up["instructions"] == brief.BRIEF and up["tools"] == brief.TOOLS
    assert up["audio"]["input"]["format"] == {"type": "audio/pcm", "rate": 24000}
    assert up["audio"]["output"]["format"] == {"type": "audio/pcm", "rate": 24000}
    # turn detection only while the listen runs: off at the open, on at its start, off at its end
    assert up["audio"]["input"]["turn_detection"] is None
    tds = [u["audio"]["input"]["turn_detection"] for u in s["updates"][1:]]
    assert tds[0]["type"] == "semantic_vad" and tds[0]["create_response"] is True and tds[0]["interrupt_response"] is False
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


def test_inner_voice_own_words_exact_and_the_queue():
    async def scenario(relay, fake, bridge):
        await relay.send({"t": "tts", "id": "t1", "text": "Your next meeting is at 3 pm.", "front": "own"})
        await relay.send({"t": "tts", "id": "t2", "text": "Build 42 passed.", "front": "exact"})  # while t1 speaks: queued, not refused
        await relay.send({"t": "front.context", "text": "The build log is 200 lines long."})
        await relay.send({"t": "tts", "id": "t3", "text": "hi"})  # plain tts = verbatim
        await relay.until(lambda: len(relay.of("tts.end")) == 3)

    relay, fake, _ = asyncio.run(run(scenario))
    s = fake.sessions[0]
    assert "Inner voice context: The build log is 200 lines long." in s["items"]
    assert [c["instructions"] for c in s["creates"]] == [brief.exactly(t) for t in ("Your next meeting is at 3 pm.", "Build 42 passed.", "hi")]
    assert relay.of("tts.text", id="t1")[-1]["text"] == "Your next meeting is at 3 pm."
    assert relay.of("tts.text", id="t2")[-1]["text"] == "Build 42 passed."
    assert [m["id"] for m in relay.of("tts.end")] == ["t1", "t2", "t3"]
    assert all("error" not in m for m in relay.of("tts.end"))
    assert not relay.of("front.reply")


def test_an_answer_sent_while_the_person_talks_waits_for_the_reply():
    async def scenario(relay, fake, bridge):
        await relay.send({"t": "stt.start", "id": "s1", "rate": 16000, "front": True})
        await relay.mic("s1", speech(400))
        await relay.send({"t": "tts", "id": "t1", "text": "Done.", "front": "own"})  # the agent, while the person still talks
        await asyncio.sleep(0.2)
        held = len(fake.sessions[0]["creates"])
        await relay.mic("s1", speech(200) + silence(700))
        await relay.until(lambda: relay.of("tts.end", id="t1") and relay.of("front.reply"))
        relay.held = held

    relay, fake, _ = asyncio.run(run(scenario))
    assert relay.held == 0  # nothing generated while the person was talking
    reply = relay.of("front.reply")[0]["id"]
    ends = [m["id"] for m in relay.of("tts.end")]
    assert ends.index(reply) < ends.index("t1")  # the front's reply first, then the agent's answer


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


def test_the_inner_voice_is_said_as_given_cues_never_spoken():
    # live: "Hey Juan! [happy] All good here..." came out as "Okay, that's a neat vibe, let me translate that briefly."
    async def scenario(relay, fake, bridge):
        await relay.send({"t": "tts", "id": "t1", "text": "Hey Juan! [happy] All good here, and yeah, pretty fast today.", "front": "own"})
        await relay.until(lambda: relay.of("tts.end", id="t1"))

    relay, fake, _ = asyncio.run(run(scenario))
    c = fake.sessions[0]["creates"][0]
    assert c["tool_choice"] == "none" and "[happy]" not in c["instructions"]
    assert not any(str(i).startswith("Inner voice: ") for i in fake.sessions[0]["items"])
    assert relay.of("tts.text", id="t1")[-1]["text"] == "Hey Juan! All good here, and yeah, pretty fast today."


def test_own_words_is_an_option():
    async def scenario(relay, fake, bridge):
        await relay.send({"t": "tts", "id": "t1", "text": "Build 42 passed. [happy]", "front": "own"})
        await relay.until(lambda: relay.of("tts.end", id="t1"))

    relay, fake, _ = asyncio.run(run(scenario, cfg_extra={"realtime_inner": "own"}))
    assert fake.sessions[0]["creates"][0]["instructions"] == brief.own_words("Build 42 passed.")
    assert relay.of("tts.text", id="t1")[-1]["text"] == "So, Build 42 passed."
