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
                 {"t":"stt.trace","id","trace":{...}} right before each stt.turn_end and stt.final (molty_voice/trace.py)
                 {"t":"stt.hint","id","k":"pause"|"question"} while the human talks (Molty's listener reactions)
                 {"t":"stt.reflex","id","n","pick","conf","p","ms","chars","tokens","model"} per reflex chunk, only when
                 stt.start said "reflex": true and this box offers it (voice.hello "reflex": true; molty_voice/reflex.py)
                 {"t":"opener.result","id","text","ms","model"} answers {"t":"opener","id","text","history"}, only
                 on a box that offers it (voice.hello "opener": true; molty_voice/opener.py); no "text": no line
  both           {"t":"ping"} / {"t":"pong"} (the relay answers pings without waking up)
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import logging
import random
import re
import time
from urllib.parse import urlencode

import numpy as np
from websockets.asyncio.client import connect as ws_connect
from websockets.exceptions import ConnectionClosed, InvalidStatus

from . import __version__, framing, opener
from .audio import DEVICE_RATE, MODEL_RATE, Resampler, fade_in, float_to_pcm16, quiet_lead
from .config import Config
from .kyutai import SttSession, synthesize
from .reflex import ReflexTracker
from .trace import SttTrace

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
    def __init__(self, cfg: Config, *, connect=ws_connect, kyutai_connect=ws_connect, reflex_model=None):
        self.cfg = cfg
        # the reflex classifier (molty_voice/reflex.py), loaded on the first connect when cfg.reflex; tests inject one
        self.reflex = reflex_model
        self._reflex_tried = reflex_model is not None
        self.opener_ready = False
        self._opener_tasks: set[asyncio.Task] = set()
        self._connect = connect
        self._kconnect = kyutai_connect
        self._ws = None
        self._send_lock = asyncio.Lock()
        self._tts: dict[str, asyncio.Task] = {}
        self._stt: dict[str, asyncio.Queue] = {}
        self._stt_tasks: dict[str, asyncio.Task] = {}
        self.connected = asyncio.Event()
        # the STT language anchor (cfg.stt_prompt as float32 @24 kHz), synthesized by the box's TTS
        # in the background; until it is there (or when it cannot be made) listens go without it
        self.stt_prompt: np.ndarray | None = None
        self._prompt_task: asyncio.Task | None = None
        self._prompt_tried = -1e9

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
        if self.cfg.reflex and self.cfg.stt and not self._reflex_tried:
            self._reflex_tried = True
            from . import reflex_hosted
            from .reflex import load
            self.reflex = reflex_hosted.load() or await asyncio.to_thread(load, self.cfg.reflex_model, self.cfg.reflex_threads)
        if self.cfg.opener and not self.opener_ready:
            self.opener_ready = await asyncio.to_thread(opener.warm, self.cfg.opener_url, self.cfg.opener_model)
        try:
            hello = {"t": "voice.hello", "name": self.cfg.name, "version": __version__, "voices": self.cfg.voices, "stt": self.cfg.stt, "tts": self.cfg.tts}
            if self.reflex is not None and self.cfg.stt:
                hello["reflex"] = True
                hello["reflex_engine"] = getattr(self.reflex, "engine", "classifier")
            if self.opener_ready:
                hello["opener"] = True
            await self._send_json(hello)
            self._ensure_prompt()
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
        elif t == "opener" and isinstance(mid, str) and isinstance(m.get("text"), str):
            if self.opener_ready:
                task = asyncio.create_task(self._run_opener(mid, m["text"], m.get("history")))
                self._opener_tasks.add(task)
                task.add_done_callback(self._opener_tasks.discard)
        elif t == "tts" and isinstance(mid, str) and isinstance(m.get("text"), str):
            if mid not in self._tts:
                self._tts[mid] = asyncio.create_task(self._run_tts(mid, m["text"], m.get("voice")))
        elif t == "tts.cancel" and isinstance(mid, str):
            task = self._tts.pop(mid, None)
            if task:
                task.cancel()
        elif t == "stt.start" and isinstance(mid, str):
            self._ensure_prompt()
            if mid not in self._stt:
                q: asyncio.Queue = asyncio.Queue()
                self._stt[mid] = q
                ns = m.get("no_speech_ms")
                no_speech_s = ns / 1000 if isinstance(ns, int) and not isinstance(ns, bool) and 1000 <= ns <= 10000 else None
                reflex = m.get("reflex") is True and self.reflex is not None
                self._stt_tasks[mid] = asyncio.create_task(self._run_stt(mid, q, no_speech_s, reflex))
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

    PROMPT_RETRY_S = 30.0

    def _ensure_prompt(self) -> None:
        """Starts making the STT language anchor when it is wanted, missing and not being made
        (again at most every PROMPT_RETRY_S: Kyutai TTS may come up after the bridge)."""
        if not (self.cfg.stt and self.cfg.stt_prompt.strip()) or self.stt_prompt is not None:
            return
        if (self._prompt_task and not self._prompt_task.done()) or time.monotonic() - self._prompt_tried < self.PROMPT_RETRY_S:
            return
        self._prompt_tried = time.monotonic()
        self._prompt_task = asyncio.create_task(self._make_prompt())

    async def _make_prompt(self) -> None:
        chunks: list[np.ndarray] = []

        async def keep(pcm: np.ndarray) -> None:
            chunks.append(pcm)

        try:
            await asyncio.wait_for(synthesize(self.cfg.tts_url, self.cfg.api_key, self.cfg.stt_prompt, self.cfg.default_voice, keep,
                                              connect=self._kconnect), 20)
            x = np.concatenate(chunks).astype(np.float32)
            x = x[quiet_lead(x, MODEL_RATE):]  # the TTS's breath before and after the line: model time for nothing
            x = x[: len(x) - quiet_lead(x[::-1], MODEL_RATE)]
            if not 0.5 <= len(x) / MODEL_RATE <= 8.0:
                raise ValueError(f"prompt audio is {len(x) / MODEL_RATE:.1f} s")
            # Kyutai TTS samples: now and then a line comes out clipped or slurred. An anchor the STT
            # does not hear as exactly the line would steer nothing (or leak a word): check it once.
            heard = await asyncio.wait_for(self._transcribe(x), 20)
            if _words(heard) != _words(self.cfg.stt_prompt):
                raise ValueError(f"the stt heard the anchor as {heard[:80]!r}")
            self.stt_prompt = x
            log.info("stt language anchor ready: %r, %.1f s", self.cfg.stt_prompt, len(x) / MODEL_RATE)
        except asyncio.CancelledError:
            raise
        except Exception as e:  # noqa: BLE001 - listens go on without it; tried again later
            log.warning("stt language anchor not made (listens go without it for now): %s: %s", type(e).__name__, e)

    async def _transcribe(self, x: np.ndarray) -> str:
        s = SttSession(self.cfg.stt_url, self.cfg.api_key, flush_s=self.cfg.stt_flush_s, turn_end_threshold=0, connect=self._kconnect)
        try:
            await s.open()
            # 1 s of silence after the line, like Kyutai's reference client: the last word is only
            # sent once the model writes something after it (the marker alone can beat it)
            await s.feed_model(np.concatenate([x, np.zeros(MODEL_RATE, dtype=np.float32)]))
            return await s.finish()
        finally:
            await s.close()

    async def _cancel_all(self) -> None:
        tasks = list(self._tts.values()) + list(self._stt_tasks.values())
        if self._prompt_task is not None and not self._prompt_task.done():
            tasks.append(self._prompt_task)  # cut short by the disconnect: made again on the next connect
            self._prompt_tried = -1e9
        self._prompt_task = None
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

    async def _run_opener(self, mid: str, text: str, history) -> None:
        hist = [h for h in history if isinstance(h, dict)] if isinstance(history, list) else []
        try:
            out = await asyncio.to_thread(opener.generate, self.cfg.opener_url, self.cfg.opener_model, text, hist)
        except Exception as e:  # noqa: BLE001 - a line is never worth the turn: silence
            log.warning("opener %s failed: %s: %s", mid, type(e).__name__, e)
            out = {"ms": 0, "model": self.cfg.opener_model}
        with contextlib.suppress(Exception):
            await self._send_json({"t": "opener.result", "id": mid, **out})

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

    async def _run_stt(self, mid: str, q: asyncio.Queue, no_speech_s: float | None = None, reflex: bool = False) -> None:
        tracker = ReflexTracker() if reflex else None
        reflex_tasks: set[asyncio.Task] = set()

        async def classify(n: int, text: str) -> None:
            # off the event loop: the STT keeps flowing while the CPU classifies
            try:
                v = await asyncio.to_thread(self.reflex.classify, text)
                await self._send_json({"t": "stt.reflex", "id": mid, "n": n, **v})
            except asyncio.CancelledError:
                raise
            except Exception as e:  # noqa: BLE001 - a reflex must never cost the turn
                log.warning("stt %s: reflex failed: %s: %s", mid, type(e).__name__, e)

        async def partial(text: str) -> None:
            await self._send_json({"t": "stt.partial", "id": mid, "text": text})
            n = tracker.due(text) if tracker else None
            if n is not None:
                task = asyncio.create_task(classify(n, text))
                reflex_tasks.add(task)
                task.add_done_callback(reflex_tasks.discard)

        trace = SttTrace(self.cfg.turn_end, self.cfg.turn_end_skip_steps, self.cfg.turn_end_hold_s)

        trace_ok = [True]

        async def send_trace(final_ms: int | None = None) -> None:
            # best effort: the decision / final that follows always goes, whatever happens here
            if not trace_ok[0]:
                return
            try:
                await self._send_json({"t": "stt.trace", "id": mid, "trace": trace.to_json(final_ms)})
            except Exception as e:  # noqa: BLE001 - a trace must never cost the turn
                trace_ok[0] = False
                log.warning("stt %s: trace failed, no more traces for this listen: %s: %s", mid, type(e).__name__, e)

        async def turn_end() -> None:
            log.info("stt %s: end of turn (%s)", mid, (trace.end or {}).get("rule"))
            await send_trace()
            await self._send_json({"t": "stt.turn_end", "id": mid})

        async def no_speech() -> None:
            log.info("stt %s: nobody spoke within %.1f s", mid, no_speech_s)
            await send_trace()
            await self._send_json({"t": "stt.turn_end", "id": mid, "reason": "no_speech"})

        async def hint(k: str) -> None:
            try:
                await self._send_json({"t": "stt.hint", "id": mid, "k": k})
            except Exception as e:  # noqa: BLE001 - best effort, like the trace
                log.warning("stt %s: hint failed: %s: %s", mid, type(e).__name__, e)

        s = SttSession(self.cfg.stt_url, self.cfg.api_key, flush_s=self.cfg.stt_flush_s, on_word=partial,
                       on_turn_end=turn_end, turn_end_threshold=self.cfg.turn_end,
                       turn_end_skip_steps=self.cfg.turn_end_skip_steps, turn_end_hold_s=self.cfg.turn_end_hold_s,
                       on_no_speech=no_speech, no_speech_s=no_speech_s, observer=trace, on_hint=hint,
                       prompt=self.stt_prompt, connect=self._kconnect)
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
                await send_trace(ms["final"])
                await self._send_json({"t": "stt.final", "id": mid, "text": text, "ms": ms})
                log.info("stt %s: final %d ms after the end: %r", mid, ms["final"], text[:80])
                return
        except asyncio.CancelledError:
            raise
        except Exception as e:  # noqa: BLE001 - surfaced to the relay as an error, never as an empty transcript
            log.warning("stt %s failed: %s", mid, e)
            await send_trace()
            await self._send_json({"t": "stt.final", "id": mid, "error": str(e)[:200] or "stt failed"})
        finally:
            for task in list(reflex_tasks):
                task.cancel()
            await s.close()  # frees the server channel at once
            self._stt.pop(mid, None)
            self._stt_tasks.pop(mid, None)


def _words(text: str) -> list[str]:
    return re.sub(r"[^a-z0-9' ]+", " ", text.lower()).split()


def _why(e: BaseException) -> str:
    if isinstance(e, InvalidStatus):
        return f"HTTP {e.response.status_code}"
    return f"{type(e).__name__}: {e}"
