"""The voice box: dials OUT to the relay's voice socket and bridges it to local moshi-server.

Wire (docs/AGENT-LINK.md "Voice box"): JSON text control messages + binary audio frames
`<JSON header>\\n<PCM16 @16 kHz>` with the request id in `s`.

  box -> relay   {"t":"voice.hello","name","version","voices":[...],"stt":bool,"tts":bool}
  relay -> box   {"t":"voice.welcome","device_id","mode","voice"}
  relay -> box   {"t":"tts","id","text","voice"?}      box -> relay  frames(s=id) ... {"t":"tts.end","id","error"?}
  relay -> box   {"t":"tts.cancel","id"}
  box -> relay   {"t":"tts.word","id","i","text","start_s"} per word, in order (Kyutai's word timing: word i of the
                 text is spoken from start_s seconds into this tts's audio)
  relay -> box   {"t":"stt.start","id","rate":16000,"no_speech_ms"?}, frames(s=id) as Molty records, {"t":"stt.end","id"} | {"t":"stt.cancel","id"}
  box -> relay   {"t":"stt.partial","id","text"}  {"t":"stt.turn_end","id"} (once: the human finished, Molty stops listening)
                 {"t":"stt.turn_end","id","reason":"no_speech"} (instead, once: nobody spoke within no_speech_ms —
                 sent only when stt.start carried it, i.e. Molty's own VAD is off and the box decides the end)
                 {"t":"stt.final","id","text","ms":{...}} | {"t":"stt.final","id","error"}
  both           {"t":"ping"} / {"t":"pong"} (the relay answers pings without waking up)
"""

from __future__ import annotations

import asyncio
import json
import logging
import random
import re
import time
from urllib.parse import urlencode

import numpy as np
from websockets.asyncio.client import connect as ws_connect
from websockets.exceptions import ConnectionClosed, InvalidStatus

from . import __version__, framing
from .audio import DEVICE_RATE, MODEL_RATE, Resampler, fade_in, float_to_pcm16, quiet_lead
from .config import Config
from .kyutai import SttSession, synthesize

log = logging.getLogger("molty_voice")

PING_S = 20.0
DEAD_S = 50.0
VOICE_RE = re.compile(r"^[A-Za-z0-9_][A-Za-z0-9_./-]{0,199}$")


class Fatal(RuntimeError):
    """The relay refused this box for good (unpaired / wrong token): reconnecting won't help."""


def voice_url(relay: str, device_id: str) -> str:
    base = relay.strip().rstrip("/")
    base = re.sub(r"^http(s?)://", r"ws\1://", base)
    return f"{base}/v1/voice?{urlencode({'device_id': device_id})}"


class Bridge:
    def __init__(self, cfg: Config, *, connect=ws_connect, kyutai_connect=ws_connect):
        self.cfg = cfg
        self._connect = connect
        self._kconnect = kyutai_connect
        self._ws = None
        self._send_lock = asyncio.Lock()
        self._tts: dict[str, asyncio.Task] = {}
        self._stt: dict[str, asyncio.Queue] = {}
        self._stt_tasks: dict[str, asyncio.Task] = {}
        self.connected = asyncio.Event()

    # ---------------- relay connection ----------------

    async def run_forever(self) -> None:
        """Connects, serves, reconnects with backoff (1 s .. 60 s, jittered). Raises Fatal on 401/403."""
        delay = 1.0
        while True:
            try:
                await self.serve_once()
                delay = 1.0  # a session that got going resets the backoff
            except Fatal:
                raise
            except (OSError, ConnectionClosed, InvalidStatus, TimeoutError) as e:
                log.warning("relay connection lost: %s", _why(e))
            wait = delay * (0.5 + random.random())
            log.info("reconnecting in %.1f s", wait)
            await asyncio.sleep(wait)
            delay = min(delay * 2, 60.0)

    async def serve_once(self) -> None:
        url = voice_url(self.cfg.relay, self.cfg.device_id)
        try:
            ws = await self._connect(url, additional_headers={"Authorization": f"Bearer {self.cfg.token}"}, ping_interval=None, max_size=1 << 20, open_timeout=15, user_agent_header=f"molty-voice/{__version__}")
        except InvalidStatus as e:
            if e.response.status_code in (401, 403):
                raise Fatal(f"the relay refused this voice box (HTTP {e.response.status_code}): it was unpaired or the token is wrong — run `molty-voice pair` again") from e
            raise
        self._ws = ws
        try:
            await self._send_json({"t": "voice.hello", "name": self.cfg.name, "version": __version__, "voices": self.cfg.voices, "stt": self.cfg.stt, "tts": self.cfg.tts})
            last_rx = time.monotonic()
            keepalive = asyncio.create_task(self._keepalive(lambda: last_rx))
            try:
                async for msg in ws:
                    last_rx = time.monotonic()
                    if isinstance(msg, bytes):
                        self._on_audio(msg)
                    else:
                        await self._on_text(msg)
            finally:
                keepalive.cancel()
            if ws.close_code in (4401,):
                raise Fatal(f"the relay closed the voice socket: {ws.close_reason or 'unauthorized'} — run `molty-voice pair` again")
            log.warning("relay closed the voice socket: %s %s", ws.close_code, ws.close_reason)
        finally:
            self.connected.clear()
            self._ws = None
            await self._cancel_all()

    async def _keepalive(self, last_rx) -> None:
        while True:
            await asyncio.sleep(PING_S)
            if time.monotonic() - last_rx() > DEAD_S:
                log.warning("no answer from the relay for %.0f s: reconnecting", DEAD_S)
                await self._ws.close(1001, "keepalive timeout")
                return
            await self._send_json({"t": "ping"})

    async def _send_json(self, obj) -> None:
        await self._send(json.dumps(obj, separators=(",", ":")))

    async def _send(self, data) -> None:
        ws = self._ws
        if ws is None:
            return
        async with self._send_lock:
            try:
                await ws.send(data)
            except ConnectionClosed:
                pass

    async def _on_text(self, raw: str) -> None:
        try:
            m = json.loads(raw)
        except ValueError:
            return
        if not isinstance(m, dict):
            return
        t, mid = m.get("t"), m.get("id")
        if t == "voice.welcome":
            log.info("connected to the relay for %s (mode %s)", m.get("device_id"), m.get("mode"))
            self.connected.set()
        elif t == "tts" and isinstance(mid, str) and isinstance(m.get("text"), str):
            if mid not in self._tts:
                self._tts[mid] = asyncio.create_task(self._run_tts(mid, m["text"], m.get("voice")))
        elif t == "tts.cancel" and isinstance(mid, str):
            task = self._tts.pop(mid, None)
            if task:
                task.cancel()
        elif t == "stt.start" and isinstance(mid, str):
            if mid not in self._stt:
                q: asyncio.Queue = asyncio.Queue()
                self._stt[mid] = q
                ns = m.get("no_speech_ms")
                no_speech_s = ns / 1000 if isinstance(ns, int) and not isinstance(ns, bool) and 1000 <= ns <= 10000 else None
                self._stt_tasks[mid] = asyncio.create_task(self._run_stt(mid, q, no_speech_s))
        elif t == "stt.end" and isinstance(mid, str):
            if mid in self._stt:
                self._stt[mid].put_nowait(("end", None))
        elif t == "stt.cancel" and isinstance(mid, str):
            task = self._stt_tasks.pop(mid, None)
            self._stt.pop(mid, None)
            if task:
                task.cancel()

    def _on_audio(self, data: bytes) -> None:
        try:
            f = framing.decode(data)
        except framing.FrameError:
            return
        q = self._stt.get(f.s)
        if q is not None and f.payload:
            q.put_nowait(("audio", f.payload))

    async def _cancel_all(self) -> None:
        tasks = list(self._tts.values()) + list(self._stt_tasks.values())
        self._tts.clear()
        self._stt.clear()
        self._stt_tasks.clear()
        for t in tasks:
            t.cancel()
        for t in tasks:
            try:
                await t
            except (asyncio.CancelledError, Exception):  # noqa: BLE001
                pass

    # ---------------- TTS ----------------

    def pick_voice(self, asked) -> str:
        if isinstance(asked, str) and VOICE_RE.match(asked) and ".." not in asked and asked.endswith((".wav", ".safetensors")):
            return asked
        return self.cfg.default_voice

    async def _run_tts(self, mid: str, text: str, voice) -> None:
        rs = Resampler(MODEL_RATE, DEVICE_RATE)
        seq = 0
        t0 = time.monotonic()
        first = None
        trim_s: float | None = None  # seconds cut from the stream's start (known with the first audio)
        held: list[tuple[str, float]] = []  # word timings that came before the first audio

        async def out(pcm24: np.ndarray, flush: bool = False) -> None:
            nonlocal seq, first, trim_s
            if not flush and trim_s is None:
                # the voice comes forward: the quiet lead of the first frame goes (never more than it)
                cut = quiet_lead(pcm24, MODEL_RATE) if self.cfg.trim_lead else 0
                trim_s = cut / MODEL_RATE
                pcm24 = fade_in(pcm24[cut:], int(MODEL_RATE * 0.005)) if cut else pcm24
                for w, s0 in held:
                    await word(w, s0)
                held.clear()
            y = rs.process(pcm24) if not flush else rs.flush()
            if y.size == 0:
                return
            first = first if first is not None else time.monotonic() - t0
            for fr in framing.chunks(mid, float_to_pcm16(y), seq):
                await self._send(fr)
                seq += 1

        nword = 0

        async def word(w: str, start_s: float) -> None:
            # word timing for the relay: Molty shows each word when it is spoken (tts.word {i, start_s}),
            # on the stream as sent (after the trimmed lead)
            nonlocal nword
            if trim_s is None:
                held.append((w, start_s))
                return
            await self._send_json({"t": "tts.word", "id": mid, "i": nword, "text": w[:64], "start_s": round(max(0.0, start_s - trim_s), 3)})
            nword += 1

        try:
            await synthesize(self.cfg.tts_url, self.cfg.api_key, text, self.pick_voice(voice), out, on_word=word, connect=self._kconnect)
            await out(np.zeros(0, np.float32), flush=True)
            await self._send_json({"t": "tts.end", "id": mid})
            log.info("tts %s: %d chars, first audio %.0f ms (lead trimmed %.0f ms), total %.0f ms", mid, len(text), (first or 0) * 1000, (trim_s or 0) * 1000, (time.monotonic() - t0) * 1000)
        except asyncio.CancelledError:
            raise
        except Exception as e:  # noqa: BLE001 - every failure ends the request cleanly
            log.warning("tts %s failed: %s", mid, e)
            await self._send_json({"t": "tts.end", "id": mid, "error": str(e)[:200] or "tts failed"})
        finally:
            self._tts.pop(mid, None)

    # ---------------- STT ----------------

    async def _run_stt(self, mid: str, q: asyncio.Queue, no_speech_s: float | None = None) -> None:
        async def partial(text: str) -> None:
            await self._send_json({"t": "stt.partial", "id": mid, "text": text})

        async def turn_end() -> None:
            log.info("stt %s: end of turn", mid)
            await self._send_json({"t": "stt.turn_end", "id": mid})

        async def no_speech() -> None:
            log.info("stt %s: nobody spoke within %.1f s", mid, no_speech_s)
            await self._send_json({"t": "stt.turn_end", "id": mid, "reason": "no_speech"})

        s = SttSession(self.cfg.stt_url, self.cfg.api_key, flush_s=self.cfg.stt_flush_s, on_word=partial,
                       on_turn_end=turn_end, turn_end_threshold=self.cfg.turn_end,
                       turn_end_skip_steps=self.cfg.turn_end_skip_steps,
                       on_no_speech=no_speech, no_speech_s=no_speech_s, connect=self._kconnect)
        t_end = None
        try:
            await s.open()
            while True:
                kind, payload = await q.get()
                if kind == "audio":
                    await s.feed(payload)
                    continue
                t_end = time.monotonic()
                text = await s.finish()
                ms = {"final": round((time.monotonic() - t_end) * 1000)}
                await self._send_json({"t": "stt.final", "id": mid, "text": text, "ms": ms})
                log.info("stt %s: final %d ms after the end: %r", mid, ms["final"], text[:80])
                return
        except asyncio.CancelledError:
            raise
        except Exception as e:  # noqa: BLE001 - surfaced to the relay as an error, never as an empty transcript
            log.warning("stt %s failed: %s", mid, e)
            await self._send_json({"t": "stt.final", "id": mid, "error": str(e)[:200] or "stt failed"})
        finally:
            await s.close()  # frees the server channel at once
            self._stt.pop(mid, None)
            self._stt_tasks.pop(mid, None)


def _why(e: BaseException) -> str:
    if isinstance(e, InvalidStatus):
        return f"HTTP {e.response.status_code}"
    return f"{type(e).__name__}: {e}"
