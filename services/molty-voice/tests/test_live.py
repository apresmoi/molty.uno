"""A live conversation against a fake gpt-live-1 (a local websocket server)."""

import asyncio
import base64
import json

import numpy as np
from websockets.asyncio.client import connect
from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed

from molty_voice import live

LOUD = (np.sin(np.arange(1600) / 3) * 8000).astype("<i2").tobytes()  # 100 ms of speech
QUIET = bytes(3200)                                                   # 100 ms of silence


class FakeModel:
    def __init__(self, script=None, start=True):
        self.script = script   # async fn(send) run after session.started
        self.start = start
        self.appended = []     # rms of each appended piece
        self.closed = False    # session.close received
        self.gone = asyncio.Event()

    async def handler(self, ws):
        try:
            first = json.loads(await ws.recv())
            assert first["type"] == "session.start"
            assert first["session"]["audio"]["format"] == {"type": "audio/pcm", "rate": 16000}
            if not self.start:
                await ws.send(json.dumps({"type": "error", "error": {"code": "nope"}}))
                return
            await ws.send(json.dumps({"type": "session.started"}))
            if self.script:
                asyncio.create_task(self.script(ws))
            async for raw in ws:
                e = json.loads(raw)
                if e["type"] == "session.input_audio.append":
                    self.appended.append(live.rms(base64.b64decode(e["audio"])))
                elif e["type"] == "session.close":
                    self.closed = True
                    await ws.send(json.dumps({"type": "session.closed"}))
                    return
        except ConnectionClosed:
            pass
        finally:
            self.gone.set()


def out(pcm):
    return json.dumps({"type": "session.output_audio.delta", "delta": base64.b64encode(pcm).decode()})


async def run_session(model, body, **kw):
    sent, audio = [], []

    async def say(m):
        sent.append(m)

    async def aud(b):
        audio.append(b)

    async with serve(model.handler, "127.0.0.1", 0) as srv:
        port = srv.sockets[0].getsockname()[1]
        s = live.LiveSession(say, aud, connect, "k", url=f"ws://127.0.0.1:{port}", **kw)
        task = asyncio.create_task(s.run())
        await body(s, sent, audio, task)
        await asyncio.wait_for(task, 5)
        await asyncio.wait_for(model.gone.wait(), 2)
    return s, sent, audio


async def until(f, ms=3000):
    for _ in range(ms // 5):
        if f():
            return
        await asyncio.sleep(0.005)
    raise AssertionError("never happened")


def test_rms():
    assert live.rms(LOUD) > live.SPEECH_RMS and live.rms(QUIET) == 0 and live.rms(b"") == 0


def test_reply_is_told_by_loudness_and_ends_on_a_silence_run():
    async def script(ws):
        for _ in range(3):
            await ws.send(out(QUIET))      # silence before a reply is not a reply
        await asyncio.sleep(0.02)
        await ws.send(out(LOUD))
        await ws.send(out(QUIET))          # a short pause inside it is kept
        await ws.send(out(LOUD))
        for _ in range(8):
            await ws.send(out(QUIET))      # 0.8 s: over

    async def body(s, sent, audio, task):
        await until(lambda: {"t": "live.reply_end"} in sent)
        s.stop()

    async def go():
        m = FakeModel(script)
        _, sent, audio = await run_session(m, body)
        kinds = [x["t"] for x in sent]
        assert kinds[:3] == ["live.started", "live.reply", "live.reply_end"]
        assert kinds[-1] == "live.end"
        # speech, the short pause, speech: the trailing silence is not played
        assert audio == [LOUD, QUIET, LOUD]
        assert m.closed

    asyncio.run(go())


m = None


def test_half_duplex_mic_is_silence_during_a_reply_until_listen():
    async def script(ws):
        await asyncio.sleep(0.05)
        await ws.send(out(LOUD))

    async def body(s, sent, audio, task):
        s.mic(LOUD)
        await until(lambda: any(x["t"] == "live.reply" for x in sent))
        assert s.muted
        n = len(m.appended)
        for _ in range(4):
            s.mic(LOUD)                    # the device's tail, or Molty's own voice
            await asyncio.sleep(0.05)
        new = m.appended[n:]
        assert new and max(new) == 0       # silence kept flowing, no mic
        for _ in range(8):
            await s._on_output(QUIET)
        s.listen()
        assert not s.muted
        n = len(m.appended)
        s.mic(LOUD)
        await until(lambda: any(r > 0 for r in m.appended[n:]))
        s.stop()

    async def go():
        global m
        m = FakeModel(script)
        await run_session(m, body)

    asyncio.run(go())


def test_listen_does_not_unmute_a_reply_in_progress():
    async def go():
        async def say(_m):
            pass

        s = live.LiveSession(say, say, None, "k")
        await s._on_output(LOUD)
        s.listen()
        assert s.muted

    asyncio.run(go())


def test_quiet_closes_the_session():
    async def body(s, sent, audio, task):
        pass

    async def go():
        m = FakeModel()
        _, sent, _ = await run_session(m, body, quiet_s=0.3)
        end = sent[-1]
        assert end["t"] == "live.end" and end["reason"] == "quiet" and m.closed and end["billed_s"] > 0

    asyncio.run(go())


def test_the_cap_closes_the_session_even_while_someone_talks():
    async def body(s, sent, audio, task):
        while not task.done():
            s.mic(LOUD)
            await asyncio.sleep(0.05)

    async def go():
        m = FakeModel()
        _, sent, _ = await run_session(m, body, cap_s=0.4)
        assert sent[-1]["reason"] == "cap" and m.closed

    asyncio.run(go())


def test_a_relay_that_never_listens_again_closes_the_session():
    async def body(s, sent, audio, task):
        await s._on_output(LOUD)
        for _ in range(8):
            await s._on_output(QUIET)

    async def go():
        m = FakeModel()
        _, sent, _ = await run_session(m, body, stall_s=0.3)
        assert sent[-1]["reason"] == "stalled"

    asyncio.run(go())


def test_model_refusal_ends_it_with_a_reason_and_no_retry():
    async def body(s, sent, audio, task):
        pass

    async def go():
        m = FakeModel(start=False)
        _, sent, _ = await run_session(m, body)
        assert [x["t"] for x in sent] == ["live.end"]
        assert sent[0]["reason"] == "error" and sent[0]["error"] == "nope" and sent[0]["billed_s"] == 0

    asyncio.run(go())


def test_model_dropping_mid_session_ends_it():
    async def script(ws):
        await asyncio.sleep(0.05)
        await ws.close()

    async def body(s, sent, audio, task):
        pass

    async def go():
        m = FakeModel(script)
        _, sent, _ = await run_session(m, body)
        assert sent[-1]["reason"] == "error" and sent[-1]["error"]

    asyncio.run(go())


def test_a_connect_failure_never_shows_the_key():
    async def go():
        sent = []

        async def say(x):
            sent.append(x)

        s = live.LiveSession(say, say, connect, "SECRETKEY", url="ws://127.0.0.1:9")
        await s.run()
        assert sent[0]["t"] == "live.end" and "SECRETKEY" not in json.dumps(sent)

    asyncio.run(go())


def test_available_needs_a_key_file(tmp_path):
    p = tmp_path / "k"
    assert not live.available(str(p))
    p.write_text("")
    assert not live.available(str(p))
    p.write_text("x")
    assert live.available(str(p))


def test_bridge_offers_live_only_when_on_with_a_key_and_cancel_closes_the_session(tmp_path):
    from molty_voice import framing
    from molty_voice.bridge import Bridge
    from molty_voice.config import Config

    async def go():
        key = tmp_path / "key"
        key.write_text("k")
        m = FakeModel()
        relay_rx = []
        started = asyncio.Event()

        async def relay(ws):
            hello = json.loads(await ws.recv())
            relay_rx.append(hello)
            await ws.send(json.dumps({"t": "voice.welcome", "device_id": "d", "mode": "auto"}))
            await ws.send(json.dumps({"t": "live.start", "id": "s1"}))
            async for raw in ws:
                f = json.loads(raw) if isinstance(raw, str) else {}
                relay_rx.append(f)
                if f.get("t") == "live.end":
                    assert f["reason"] == "refused"
                    return
                if f.get("t") == "live.started":
                    await ws.send(framing.encode("s1", 0, LOUD))
                    await until(lambda: any(r > 0 for r in m.appended))
                    started.set()
                    return  # the socket goes: the bridge must close the session

        async with serve(m.handler, "127.0.0.1", 0) as ms, serve(relay, "127.0.0.1", 0) as rs:
            mport, rport = ms.sockets[0].getsockname()[1], rs.sockets[0].getsockname()[1]

            def live_connect(url, **kw):
                return connect(f"ws://127.0.0.1:{mport}", **kw)

            cfg = Config(relay=f"http://127.0.0.1:{rport}", device_id="molty-k7m2q9xa4c", token="t", stt_prompt="", live=True, live_key_file=str(key))
            b = Bridge(cfg, live_connect=live_connect)
            await asyncio.wait_for(b.serve_once(), 5)
            assert started.is_set() and relay_rx[0]["live"] is True
            await asyncio.wait_for(m.gone.wait(), 2)
            assert not b._live

            off = Bridge(Config(relay=cfg.relay, device_id=cfg.device_id, token="t", stt_prompt=""))
            relay_rx.clear()
            await asyncio.wait_for(off.serve_once(), 5)
            assert "live" not in relay_rx[0]

    asyncio.run(go())
