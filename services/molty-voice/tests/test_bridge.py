"""The bridge end to end against a fake relay and fake moshi-server STT/TTS (all local websockets)."""

import asyncio
import json
from http import HTTPStatus
from urllib.parse import parse_qs, urlparse

import msgpack
import numpy as np
import pytest
from websockets.asyncio.server import serve

from molty_voice import framing
from molty_voice.bridge import Bridge, Fatal
from molty_voice.config import DEFAULT_VOICE, Config

DEVICE = "molty-k7m2q9xa4c"
TOKEN = DEVICE + "." + "A" * 43


class FakeMoshi:
    """STT: words only after the marker went in and some silence pushed it through; TTS: 4800 samples per word."""

    def __init__(self):
        self.stt_sessions = []  # per session: {"samples", "closed", "marker"}
        self.tts_requests = []
        self.stt_mode = "ok"  # ok | no_channel | garbage
        self.tts_lead = 0  # samples of silence the first TTS audio frame opens with

    async def handler(self, ws):
        assert ws.request.headers.get("kyutai-api-key") == "public_token"
        if ws.request.path.startswith("/api/asr-streaming"):
            await self.stt(ws)
        else:
            await self.tts(ws)

    async def stt(self, ws):
        sess = {"samples": 0, "closed": False, "marker": None, "after": 0}
        self.stt_sessions.append(sess)
        if self.stt_mode == "no_channel":
            await ws.close(1011, "no free channels")
            sess["closed"] = True
            return
        try:
            async for m in ws:
                d = msgpack.unpackb(m)
                if d["type"] == "Audio":
                    if sess["marker"] is None:
                        sess["samples"] += len(d["pcm"])
                    else:
                        sess["after"] += 1
                        if sess["after"] == 6 and self.stt_mode == "garbage":
                            # a word, then a message that is not msgpack at all (0xc1 is never valid)
                            await ws.send(msgpack.packb({"type": "Word", "text": "hello", "start_time": 0.1}))
                            await ws.send(b"\xc1\xc1 not msgpack")
                        elif sess["after"] == 6:  # the model's delay: ~0.5 s of silence later
                            await ws.send(msgpack.packb({"type": "Word", "text": "hello", "start_time": 0.1}))
                            await ws.send(msgpack.packb({"type": "Word", "text": "molty.", "start_time": 0.4}))
                            await ws.send(msgpack.packb({"type": "Marker", "id": sess["marker"]}))
                            await ws.send(msgpack.packb({"type": "Word", "text": "late", "start_time": 2.0}))
                elif d["type"] == "Marker":
                    sess["marker"] = d["id"]
        finally:
            sess["closed"] = True

    async def tts(self, ws):
        q = parse_qs(urlparse(ws.request.path).query)
        req = {"voice": q["voice"][0], "format": q["format"][0], "words": []}
        self.tts_requests.append(req)
        async for m in ws:
            d = msgpack.unpackb(m)
            if d["type"] == "Text":
                req["words"].append(d["text"])
            elif d["type"] == "Eos":
                for i, w in enumerate(req["words"]):   # like moshi-server: the word's timing, then its audio
                    await ws.send(msgpack.packb({"type": "Text", "text": w, "start_s": 0.2 * i, "stop_s": 0.2 * i + 0.16}, use_single_float=True))
                    lead = self.tts_lead if i == 0 else 0
                    await ws.send(msgpack.packb({"type": "Audio", "pcm": [0.0] * lead + [0.25] * (2400 - lead)}, use_single_float=True))
                    await ws.send(msgpack.packb({"type": "Audio", "pcm": [0.25] * 2400}, use_single_float=True))
                await ws.close()
                return


async def with_servers(relay_handler, moshi: FakeMoshi, cfg_extra=None, process_request=None, bridge_kwargs=None):
    async with serve(moshi.handler, "127.0.0.1", 0) as ms, serve(relay_handler, "127.0.0.1", 0, process_request=process_request) as rs:
        mport = ms.sockets[0].getsockname()[1]
        rport = rs.sockets[0].getsockname()[1]
        cfg = Config(relay=f"http://127.0.0.1:{rport}", device_id=DEVICE, token=TOKEN, name="test box",
                     stt_url=f"ws://127.0.0.1:{mport}/api/asr-streaming", tts_url=f"ws://127.0.0.1:{mport}/api/tts_streaming",
                     **{"stt_prompt": "", **(cfg_extra or {})})
        bridge = Bridge(cfg, **(bridge_kwargs or {}))
        await asyncio.wait_for(bridge.serve_once(), 10)
        return bridge


def test_tts_and_streaming_stt_through_the_bridge():
    moshi = FakeMoshi()
    seen = {}

    async def relay(ws):
        seen["path"] = ws.request.path
        seen["auth"] = ws.request.headers.get("authorization")
        hello = json.loads(await ws.recv())
        seen["hello"] = hello
        await ws.send(json.dumps({"t": "voice.welcome", "device_id": DEVICE, "mode": "auto", "voice": None}))

        # TTS: an unsafe voice name falls back to the default; a normal one passes through
        audio = {"t1": b"", "t2": b""}
        ends = {}
        words = []
        await ws.send(json.dumps({"t": "tts", "id": "t1", "text": "hello molty there", "voice": "../../etc/passwd"}))
        await ws.send(json.dumps({"t": "tts", "id": "t2", "text": "hi", "voice": "expresso/ex04-ex02_happy_001_channel1_118s.wav"}))
        while len(ends) < 2:
            m = await ws.recv()
            if isinstance(m, bytes):
                f = framing.decode(m)
                assert len(f.payload) <= 16 * 1024
                audio[f.s] += f.payload
            else:
                d = json.loads(m)
                if d["t"] == "tts.end":
                    ends[d["id"]] = d
                if d["t"] == "tts.word":
                    words.append(d)
        seen["tts_words"] = words
        seen["tts_audio"] = audio
        seen["tts_end"] = ends

        # STT: start, chunks as Molty records, end -> final; partials may arrive
        await ws.send(json.dumps({"t": "stt.start", "id": "s1", "rate": 16000}))
        speech = np.full(3200, 3000, dtype="<i2").tobytes()
        for seq in range(3):
            await ws.send(framing.encode("s1", seq, speech))
            await asyncio.sleep(0.02)
        await asyncio.sleep(0.1)
        seen["stt_samples_before_end"] = moshi.stt_sessions[0]["samples"]
        await ws.send(json.dumps({"t": "stt.end", "id": "s1"}))
        partials = []
        while True:
            d = json.loads(await ws.recv())
            if d["t"] == "stt.partial":
                partials.append(d["text"])
            if d["t"] == "stt.final":
                seen["final"] = d
                break
        seen["partials"] = partials
        await asyncio.sleep(0.1)
        seen["stt_closed"] = moshi.stt_sessions[0]["closed"]

        # STT failure: the server has no free channel and closes without a marker -> an error, not ""
        moshi.stt_mode = "no_channel"
        await ws.send(json.dumps({"t": "stt.start", "id": "s2", "rate": 16000}))
        await ws.send(framing.encode("s2", 0, speech))
        await ws.send(json.dumps({"t": "stt.end", "id": "s2"}))
        while True:
            d = json.loads(await ws.recv())
            if d["t"] == "stt.final":
                seen["failed"] = d
                break
        await ws.close()

    asyncio.run(with_servers(relay, moshi))

    assert seen["path"] == f"/v1/voice?device_id={DEVICE}"
    assert seen["auth"] == f"Bearer {TOKEN}"
    assert seen["hello"] == {"t": "voice.hello", "name": "test box", "version": "0.1.0", "voices": Config().voices, "stt": True, "tts": True}
    # TTS
    assert moshi.tts_requests[0]["voice"] == DEFAULT_VOICE
    assert moshi.tts_requests[0]["format"] == "PcmMessagePack"
    assert moshi.tts_requests[0]["words"] == ["hello", "molty", "there"]
    assert moshi.tts_requests[1]["voice"] == "expresso/ex04-ex02_happy_001_channel1_118s.wav"
    assert seen["tts_end"]["t1"] == {"t": "tts.end", "id": "t1"}
    # the server's word timing less its one-step (80 ms) lag, forwarded per tts id with its index in the text
    w1 = [w for w in seen["tts_words"] if w["id"] == "t1"]
    assert [(w["i"], w["text"], w["start_s"]) for w in w1] == [(0, "hello", 0.0), (1, "molty", 0.12), (2, "there", 0.32)]
    assert [(w["i"], w["text"]) for w in seen["tts_words"] if w["id"] == "t2"] == [(0, "hi")]
    pcm = np.frombuffer(seen["tts_audio"]["t1"], dtype="<i2")
    assert len(pcm) == 3 * 4800 * 2 // 3  # 24 kHz -> 16 kHz, every sample accounted for
    assert abs(int(np.median(pcm[200:-200])) - 8192) < 50  # 0.25 * 32768
    # STT: streamed while "recording" (all 3 chunks at the server before stt.end), words before the marker only
    assert seen["stt_samples_before_end"] >= 3 * 3200 * 3 // 2 - 1920
    assert seen["final"]["text"] == "hello molty."
    assert seen["final"]["id"] == "s1" and "final" in seen["final"]["ms"]
    assert seen["partials"] and seen["partials"][-1] == "hello molty."
    assert seen["stt_closed"] is True  # the server channel was freed at once
    assert seen["failed"]["id"] == "s2" and "error" in seen["failed"] and "text" not in seen["failed"]


def test_cancel_frees_the_stt_channel():
    moshi = FakeMoshi()
    seen = {}

    async def relay(ws):
        await ws.recv()
        await ws.send(json.dumps({"t": "voice.welcome", "device_id": DEVICE, "mode": "auto", "voice": None}))
        await ws.send(json.dumps({"t": "stt.start", "id": "s9", "rate": 16000}))
        await ws.send(framing.encode("s9", 0, b"\x00\x10" * 1600))
        await asyncio.sleep(0.15)
        await ws.send(json.dumps({"t": "stt.cancel", "id": "s9"}))
        await asyncio.sleep(0.15)
        seen["closed"] = moshi.stt_sessions[0]["closed"]
        await ws.close()

    asyncio.run(with_servers(relay, moshi))
    assert seen["closed"] is True


def test_a_refused_token_is_fatal():
    moshi = FakeMoshi()

    def reject(connection, request):
        return connection.respond(HTTPStatus.UNAUTHORIZED, '{"ok":false,"error":"unauthorized"}\n')

    async def relay(ws):  # never reached
        await ws.close()

    with pytest.raises(Fatal):
        asyncio.run(with_servers(relay, moshi, process_request=reject))


def test_a_receiver_failure_is_an_stt_error_never_a_transcript():
    """Malformed msgpack from the STT server: stt.final {error}, so the relay can fall back to the cloud."""
    moshi = FakeMoshi()
    moshi.stt_mode = "garbage"
    seen = {}

    async def relay(ws):
        await ws.recv()
        await ws.send(json.dumps({"t": "voice.welcome", "device_id": DEVICE, "mode": "auto", "voice": None}))
        await ws.send(json.dumps({"t": "stt.start", "id": "s3", "rate": 16000}))
        await ws.send(framing.encode("s3", 0, np.full(3200, 3000, dtype="<i2").tobytes()))
        await ws.send(json.dumps({"t": "stt.end", "id": "s3"}))
        while True:
            d = json.loads(await ws.recv())
            if d["t"] == "stt.final":
                seen["final"] = d
                break
        await asyncio.sleep(0.1)
        seen["closed"] = moshi.stt_sessions[0]["closed"]
        await ws.close()

    asyncio.run(with_servers(relay, moshi))
    assert "error" in seen["final"] and "text" not in seen["final"], seen["final"]
    assert "receiver failed" in seen["final"]["error"]
    assert seen["closed"] is True


def _tts_run(cfg_extra=None, lead=0):
    moshi = FakeMoshi()
    moshi.tts_lead = lead
    seen = {"audio": b"", "words": []}

    async def relay(ws):
        await ws.recv()
        await ws.send(json.dumps({"t": "voice.welcome", "device_id": DEVICE, "mode": "auto", "voice": None}))
        await ws.send(json.dumps({"t": "tts", "id": "t1", "text": "one two"}))
        while True:
            m = await ws.recv()
            if isinstance(m, bytes):
                seen["audio"] += framing.decode(m).payload
            else:
                d = json.loads(m)
                if d["t"] == "tts.word":
                    seen["words"].append((d["i"], d["start_s"], len(seen["audio"]) // 2))
                if d["t"] == "tts.end":
                    break
        await ws.close()

    asyncio.run(with_servers(relay, moshi, cfg_extra))
    return seen


def test_tts_quiet_lead_is_trimmed_and_word_timings_follow():
    # 40 ms of silence opens the stream: 30 ms of it goes (10 ms kept), the timings move with it,
    # and timings that came before any audio wait until the trim is known
    seen = _tts_run(lead=960)
    assert len(seen["audio"]) // 2 == (4 * 2400 - 720) * 2 // 3
    assert [(i, s) for i, s, _ in seen["words"]] == [(0, 0.0), (1, 0.09)]
    pcm = np.frombuffer(seen["audio"], dtype="<i2")
    assert abs(int(pcm[0])) < 100  # 10 ms of the quiet kept, then the fade: no click


def test_tts_lead_trim_can_be_turned_off():
    seen = _tts_run({"trim_lead": False}, lead=960)
    assert len(seen["audio"]) // 2 == 4 * 2400 * 2 // 3
    assert [(i, s) for i, s, _ in seen["words"]] == [(0, 0.0), (1, 0.12)]


# ---------------- first words (molty_voice/opener.py) ----------------


def test_opener_prompt_has_history_and_keeps_it_short():
    from molty_voice import opener

    hist = [{"role": "human" if i % 2 == 0 else "molty", "text": f"m{i}"} for i in range(9)]
    body = opener.request_body("qwen3:1.7b", "and tomorrow?", hist)
    user = body["messages"][1]["content"]
    assert user.endswith("Human: and tomorrow?\nMolty:")
    assert "m4" in user and "m3" not in user  # the last five only
    assert "Molty: m7" in user and "Human: m8" in user
    assert body["think"] is False and body["keep_alive"] == -1 and body["stream"] is False


def test_opener_clean_keeps_one_plain_line():
    from molty_voice import opener

    assert opener.clean('"Hmm, the weather,"\nHuman: more') == "Hmm, the weather,"
    assert opener.clean("Molty: So,") == "So,"
    assert opener.clean("/here come the buttons,") == "here come the buttons,"
    assert opener.clean("¿Cómo, a ver") == "¿Cómo, a ver"


def test_opener_generate_and_warm_against_a_fake_ollama():
    import http.server
    import threading

    from molty_voice import opener

    seen = []

    class H(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            seen.append(json.loads(self.rfile.read(int(self.headers["content-length"]))))
            out = json.dumps({"message": {"content": "Hmm, the weather,\nnoise"}}).encode()
            self.send_response(200)
            self.end_headers()
            self.wfile.write(out)

        def log_message(self, *a):
            pass

    srv = http.server.HTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        url = f"http://127.0.0.1:{srv.server_port}"
        out = opener.generate(url, "m", "hi", [])
        assert out["text"] == "Hmm, the weather," and out["model"] == "m" and isinstance(out["ms"], int)
        assert opener.warm(url, "m") is True and seen[-1]["keep_alive"] == -1
    finally:
        srv.shutdown()
    assert opener.warm("http://127.0.0.1:9", "m", 0.2) is False  # no model: the box does not offer it


def test_bridge_answers_opener_only_when_ready(monkeypatch):
    from molty_voice import opener

    async def go():
        b = Bridge(Config(opener=True))
        sent = []

        async def send(obj):
            sent.append(obj)

        b._send_json = send
        monkeypatch.setattr(opener, "generate", lambda *a, **k: {"text": "So,", "ms": 3, "model": "m"})
        await b._on_text(json.dumps({"t": "opener", "id": "t1", "text": "hi", "history": []}))
        await asyncio.sleep(0.02)
        assert sent == []  # not offered: not answered
        b.opener_ready = True
        await b._on_text(json.dumps({"t": "opener", "id": "t2", "text": "hi", "history": [{"role": "human", "text": "x"}, 3]}))
        for _ in range(50):
            if sent:
                break
            await asyncio.sleep(0.01)
        assert sent == [{"t": "opener.result", "id": "t2", "text": "So,", "ms": 3, "model": "m"}]

        def boom(*a, **k):
            raise RuntimeError("model down")

        monkeypatch.setattr(opener, "generate", boom)
        sent.clear()
        await b._on_text(json.dumps({"t": "opener", "id": "t3", "text": "hi"}))
        for _ in range(50):
            if sent:
                break
            await asyncio.sleep(0.01)
        assert sent[0]["id"] == "t3" and "text" not in sent[0]  # silence, never a crash

    asyncio.run(go())
