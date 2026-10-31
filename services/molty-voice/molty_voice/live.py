"""A live conversation: Molty's mic and speaker on OpenAI's gpt-live-1, the box in the middle.

An experiment behind the owner's `conversation: "live"`. The relay plays the turns (one device listen,
then one device say: the device does one or the other, which is also the half duplex a board with no echo
cancellation needs); this module owns the session, which is billed per second: it closes on the relay's
live.stop, when the voice socket goes, after QUIET_S with nobody talking, and at CAP_S whatever happens.

gpt-live-1 streams continuously and marks no turns: its output is frames of speech and of silence, so
speech is told by loudness, and a reply ends after REPLY_END_S of silence on the model's own timeline.
"""

from __future__ import annotations

import asyncio
import base64
import json
import logging
import os
import time
from typing import Awaitable, Callable

import numpy as np

log = logging.getLogger("molty_voice")

URL = "wss://api.openai.com/v1/live/sessions"
MODEL = "gpt-live-1"
VOICE = "marin"
RATE = 16000
INSTRUCTIONS = (
    "You are Molty, a small round desk creature with a voice, in a spoken conversation with your human. "
    "Answer at once and briefly: one or two short spoken sentences, warm and natural, in their language."
)

SPEECH_RMS = 300.0     # output louder than this is speech (16-bit)
MIC_RMS = 800.0        # mic louder than this is someone talking, for the quiet timer and the timing
REPLY_END_S = 0.7      # this much output silence ends a reply
QUIET_S = 20.0         # nobody talked, nothing said: the session closes
CAP_S = 600.0          # whatever happens
STALL_S = 60.0         # the mic stayed off this long with no reply: the relay is not coming back
START_S = 2.5          # session.started within this
CLOSE_S = 1.0          # session.closed within this
SILENCE_EVERY_S = 0.1  # silence goes out in pieces this long, when no mic is coming
MIC_GAP_S = 0.45       # the device sends a frame each 300 ms: only a longer gap gets silence


def rms(pcm: bytes) -> float:
    a = np.frombuffer(pcm[: len(pcm) // 2 * 2], dtype="<i2")
    return float(np.sqrt(np.mean(a.astype(np.float64) ** 2))) if a.size else 0.0


def key_path(given: str = "") -> str:
    return os.path.expanduser(given or "~/.config/molty-voice/openai.key")


def available(given: str = "") -> bool:
    """The key's file is there and holds something (its contents are read only to open a session)."""
    try:
        return os.path.getsize(key_path(given)) > 0
    except OSError:
        return False


class LiveSession:
    def __init__(
        self,
        send_json: Callable[[dict], Awaitable[None]],
        send_audio: Callable[[bytes], Awaitable[None]],
        connect,
        key: str,
        url: str = URL,
        model: str = MODEL,
        voice: str = VOICE,
        quiet_s: float = QUIET_S,
        cap_s: float = CAP_S,
        stall_s: float = STALL_S,
    ):
        self.send_json, self.send_audio, self.connect, self.key = send_json, send_audio, connect, key
        self.url, self.model, self.voice = url, model, voice
        self.quiet_s, self.cap_s, self.stall_s = quiet_s, cap_s, stall_s
        self.stopping = asyncio.Event()
        self.muted = False
        self.in_reply = False
        self._ws = None
        self._held: list[bytes] = []   # silent frames inside a reply: sent only if speech follows
        self._held_s = 0.0
        self._last_loud_mic = 0.0
        self._last_mic = 0.0
        self._last_out = 0.0
        self._activity = 0.0
        self._muted_at = 0.0
        self.reason = ""

    def stop(self) -> None:
        self.reason = self.reason or "stopped"
        self.stopping.set()

    def listen(self) -> None:
        """Molty has played everything: the mic is on again, unless the next reply already began."""
        if not self.in_reply:
            self.muted = False
        self._activity = time.monotonic()

    def mic(self, pcm: bytes) -> None:
        """A frame from the device. Dropped while muted: silence goes instead (the pacer)."""
        ws = self._ws
        if ws is None or self.muted or not pcm:
            return
        now = time.monotonic()
        self._last_mic = now
        if rms(pcm) > MIC_RMS:
            self._last_loud_mic = now
            self._activity = now
        asyncio.ensure_future(self._append(pcm))

    async def _append(self, pcm: bytes) -> None:
        ws = self._ws
        if ws is None:
            return
        try:
            self._last_out = time.monotonic()
            await ws.send(json.dumps({"type": "session.input_audio.append", "audio": base64.b64encode(pcm).decode()}))
        except Exception:  # noqa: BLE001 - the reader sees the closed socket and ends the session
            pass

    async def _pacer(self) -> None:
        """The stream never stops: silence whenever Molty plays or the mic is between listens."""
        piece = bytes(int(RATE * SILENCE_EVERY_S) * 2)
        while True:
            await asyncio.sleep(SILENCE_EVERY_S)
            now = time.monotonic()
            if self.muted or now - self._last_mic >= MIC_GAP_S:
                await self._append(piece)

    def _timers(self) -> str:
        now = time.monotonic()
        if self.muted and not self.in_reply:
            return "stalled" if now - self._muted_at >= self.stall_s else ""
        if not self.muted and not self.in_reply and now - self._activity >= self.quiet_s:
            return "quiet"
        return ""

    async def _watch(self) -> None:
        while True:
            await asyncio.sleep(0.25)
            why = self._timers()
            if why:
                self.reason = why
                self.stopping.set()
                return

    async def _on_output(self, pcm: bytes) -> None:
        loud = rms(pcm) > SPEECH_RMS
        now = time.monotonic()
        if not self.in_reply:
            if not loud:
                return
            self.in_reply = True
            self.muted = True
            self._activity = now
            self._held, self._held_s = [], 0.0
            first = {"t": "live.reply"}
            if self._last_loud_mic:
                first["gap_ms"] = int((now - self._last_loud_mic) * 1000)
            await self.send_json(first)
            await self.send_audio(pcm)
            return
        self._activity = now
        if loud:
            for held in self._held:
                await self.send_audio(held)
            self._held, self._held_s = [], 0.0
            await self.send_audio(pcm)
            return
        self._held.append(pcm)
        self._held_s += len(pcm) / 2 / RATE
        if self._held_s >= REPLY_END_S:
            self._held, self._held_s = [], 0.0
            self.in_reply = False
            self._muted_at = now
            await self.send_json({"t": "live.reply_end"})

    async def _reader(self, ws) -> None:
        async for raw in ws:
            e = json.loads(raw)
            ty = e.get("type")
            if ty == "session.output_audio.delta":
                await self._on_output(base64.b64decode(e.get("delta", "")))
            elif ty == "session.input_transcript.delta":
                self._activity = time.monotonic()
                await self.send_json({"t": "live.heard", "text": str(e.get("delta", ""))})
            elif ty == "session.output_transcript.delta":
                await self.send_json({"t": "live.said", "text": str(e.get("delta", ""))})
            elif ty == "error":
                raise RuntimeError(str((e.get("error") or {}).get("code") or "model_error"))
        raise RuntimeError("model_closed")

    async def run(self) -> None:
        """Start to end; always sends live.end last, and never leaves the model's socket open."""
        t_started = 0.0
        error = None
        tasks: list[asyncio.Task] = []
        try:
            async with self.connect(self.url, additional_headers={"Authorization": f"Bearer {self.key}"}, max_size=None) as ws:
                await ws.send(json.dumps({"type": "session.start", "session": {
                    "model": self.model,
                    "instructions": INSTRUCTIONS,
                    "audio": {"format": {"type": "audio/pcm", "rate": RATE}, "output": {"voice": self.voice}},
                }}))
                async def started():
                    while True:
                        e = json.loads(await ws.recv())
                        if e.get("type") == "session.started":
                            return
                        if e.get("type") == "error":
                            raise RuntimeError(str((e.get("error") or {}).get("code") or "model_error"))
                await asyncio.wait_for(started(), START_S)
                t_started = time.monotonic()
                self._activity = t_started
                self._last_mic = t_started
                self._ws = ws
                await self.send_json({"t": "live.started"})
                reader = asyncio.create_task(self._reader(ws))
                stop = asyncio.create_task(self.stopping.wait())
                tasks = [reader, stop, asyncio.create_task(self._pacer()), asyncio.create_task(self._watch())]
                done, _ = await asyncio.wait({reader, stop}, timeout=self.cap_s, return_when=asyncio.FIRST_COMPLETED)
                if not done:
                    self.reason = "cap"
                elif reader in done:
                    exc = reader.exception() if not reader.cancelled() else None
                    error = _why(exc) if exc else "model_closed"
                self._ws = None
                for t in tasks:
                    t.cancel()
                await asyncio.gather(*tasks, return_exceptions=True)
                tasks = []
                try:
                    await ws.send(json.dumps({"type": "session.close"}))
                    await asyncio.wait_for(_closed(ws), CLOSE_S)
                except Exception:  # noqa: BLE001 - leaving the `async with` closes the socket either way
                    pass
        except asyncio.CancelledError:
            raise
        except Exception as e:  # noqa: BLE001 - never a stuck turn: the relay hears why
            error = _why(e)
        finally:
            self._ws = None
            for t in tasks:
                t.cancel()
        billed = round(time.monotonic() - t_started, 1) if t_started else 0
        if self.in_reply:
            await self.send_json({"t": "live.reply_end"})
        end = {"t": "live.end", "reason": "error" if error else (self.reason or "stopped"), "billed_s": billed}
        if error:
            end["error"] = error
        log.info("live session closed: %s, %s s", end["reason"], billed)
        await self.send_json(end)


async def _closed(ws) -> None:
    async for raw in ws:
        if json.loads(raw).get("type") == "session.closed":
            return


def _why(e: BaseException) -> str:
    """The kind of failure, never its text (a connect error could quote a header)."""
    return str(e)[:60] if isinstance(e, RuntimeError) else type(e).__name__
