"""Clients for Kyutai's moshi-server (STT `/api/asr-streaming`, TTS `/api/tts_streaming`).

Protocol (msgpack over WebSocket, header `kyutai-api-key`), as in Kyutai's reference
clients `scripts/stt_from_file_rust_server.py` / `scripts/tts_rust_server.py`:

STT  client -> {"type":"Audio","pcm":[float @24 kHz]} ... {"type":"Marker","id":n}
     server -> {"type":"Word","text","start_time"} {"type":"EndWord"} {"type":"Step","prs":[...]}
               (one Step per 80 ms frame; prs[2] is the model's "the speaker paused" estimate)
               {"type":"Marker","id":n} once the audio before the marker is transcribed
               (the model lags, so silence must follow the marker to push it through).
TTS  client -> {"type":"Text","text":word} ... {"type":"Eos"}
     server -> {"type":"Audio","pcm":[float @24 kHz]} ... then closes the socket.

The STT server has a fixed number of channels: every session closes its socket as soon
as it is done (or cancelled), and a socket that closes before the marker came back
(e.g. "no free channels") is an error, never an empty transcript.
"""

from __future__ import annotations

import asyncio
import logging
import math
from collections.abc import Awaitable, Callable
from urllib.parse import urlencode

import msgpack
import numpy as np
from websockets.asyncio.client import connect as ws_connect
from websockets.exceptions import ConnectionClosed

from .audio import DEVICE_RATE, MODEL_RATE, Resampler, pcm16_to_float

log = logging.getLogger("molty_voice.kyutai")

FRAME = 1920  # 80 ms @24 kHz: the model's step
FRAME_S = FRAME / MODEL_RATE


class KyutaiError(RuntimeError):
    pass


class PausePredictor:
    """Semantic end of turn from the STT's Step messages — Kyutai Unmute's rule.

    unmute/stt/speech_to_text.py + unmute_handler.py determine_pause: an exponential
    moving average of prs[2] (attack/release half-life 0.01 s, starting at 1.0), the
    first 12 steps ignored ("all over the place"), and a pause when it exceeds 0.6
    while the user is speaking — i.e. once they said something. Here: `update()`
    returns True exactly once, the first time that holds after >= 1 word. Pure
    silence never ends a turn that way.

    `no_speech(s)`: the other end — nobody said a word. True exactly once, the first step
    at which the model has heard >= s seconds, no Word came, and nobody is speaking right
    then (the pause estimate above NO_SPEECH_QUIET): the device's own no_speech rule, for
    listens where the device's VAD is off and the box decides the end of turn. Never once
    a word came, and never after the turn ended.
    """

    NO_SPEECH_QUIET = 0.5

    SKIP_STEPS = 12

    def __init__(self, threshold: float = 0.6, attack_s: float = 0.01, release_s: float = 0.01):
        self.threshold, self.attack_s, self.release_s = threshold, attack_s, release_s
        self.value = 1.0
        self.steps = 0
        self.words = 0
        self.fired = False
        self.no_speech_fired = False

    def word(self) -> None:
        self.words += 1

    def update(self, pause_prob: float, dt: float = FRAME_S) -> bool:
        self.steps += 1
        if self.steps <= self.SKIP_STEPS:
            return False
        p = max(0.0, float(pause_prob))
        half = self.attack_s if p > self.value else self.release_s
        alpha = 1 - math.exp(-dt / half * math.log(2))
        self.value = (1 - alpha) * self.value + alpha * p
        if self.fired or self.threshold <= 0 or self.words == 0 or self.value <= self.threshold:
            return False
        self.fired = True
        return True

    def no_speech(self, after_s: float, dt: float = FRAME_S) -> bool:
        """Call after update() on each step: True once when nobody spoke within after_s."""
        if self.no_speech_fired or self.fired or self.words or after_s <= 0:
            return False
        if self.steps * dt < after_s - 1e-9 or self.value <= self.NO_SPEECH_QUIET:
            return False
        self.no_speech_fired = True
        return True


def _pack(obj) -> bytes:
    return msgpack.packb(obj, use_single_float=True)


def _headers(api_key: str) -> dict[str, str]:
    return {"kyutai-api-key": api_key}


class SttSession:
    """One streaming transcription: open(), feed() PCM16 @16 kHz as it arrives, finish() -> text."""

    def __init__(
        self,
        url: str,
        api_key: str,
        *,
        flush_s: float = 6.0,
        on_word: Callable[[str], Awaitable[None]] | None = None,
        on_turn_end: Callable[[], Awaitable[None]] | None = None,
        turn_end_threshold: float = 0.6,
        on_no_speech: Callable[[], Awaitable[None]] | None = None,
        no_speech_s: float | None = None,
        connect=ws_connect,
    ):
        self.url, self.api_key, self.flush_s, self.on_word, self._connect = url, api_key, flush_s, on_word, connect
        self.on_turn_end = on_turn_end
        self.pause = PausePredictor(turn_end_threshold)
        # nobody spoke within no_speech_s (None: off): the relay turned the device's VAD off
        self.on_no_speech, self.no_speech_s = on_no_speech, no_speech_s
        self._finishing = False
        self.words: list[str] = []
        self._rs = Resampler(DEVICE_RATE, MODEL_RATE)
        self._pending = np.zeros(0, dtype=np.float32)
        self._ws = None
        self._recv: asyncio.Task | None = None
        # Two different facts: the receiver ENDED (for any reason) vs the marker CAME BACK.
        # Only the second makes a transcript; every other end is a failure, never "".
        self._ended = asyncio.Event()
        self._marker_seen = False
        self._failure: str | None = None
        self._marker_id = 1

    async def open(self) -> None:
        self._ws = await self._connect(self.url, additional_headers=_headers(self.api_key), max_size=None, ping_interval=None)
        self._recv = asyncio.create_task(self._receive())

    async def _receive(self) -> None:
        try:
            async for m in self._ws:
                d = msgpack.unpackb(m, raw=False)
                t = d.get("type") if isinstance(d, dict) else None
                if t == "Word":
                    self.words.append(str(d.get("text", "")))
                    self.pause.word()
                    if self.on_word:
                        await self.on_word(" ".join(self.words))
                elif t == "Step":
                    prs = d.get("prs") or []
                    # after finish() began, the flush silence is ours, not the human's pause
                    if len(prs) > 2 and self.pause.update(prs[2]) and not self._finishing and self.on_turn_end:
                        await self.on_turn_end()
                    elif (len(prs) > 2 and self.no_speech_s and self.on_no_speech and not self._finishing
                          and self.pause.no_speech(self.no_speech_s)):
                        await self.on_no_speech()
                elif t == "Marker" and d.get("id") == self._marker_id:
                    self._marker_seen = True
                    return
                elif t == "Error":
                    self._failure = f"stt server error: {d.get('message', '')}"[:200]
                    return
            self._failure = "stt server closed the stream before the marker"
        except ConnectionClosed as e:
            self._failure = f"stt server closed the stream before the marker ({e.rcvd.reason if e.rcvd else 'no reason'})"[:200]
        except asyncio.CancelledError:
            self._failure = self._failure or "stt receiver cancelled"
            raise
        except Exception as e:  # noqa: BLE001 - malformed message, callback failure, …: all are failures
            self._failure = f"stt receiver failed: {type(e).__name__}: {e}"[:200]
        finally:
            if not self._marker_seen and not self._failure:
                self._failure = "stt receiver ended without the marker"
            self._ended.set()  # wake finish(): it decides by _marker_seen / _failure

    async def _send_frames(self, x: np.ndarray, final: bool = False) -> None:
        buf = np.concatenate([self._pending, x]) if self._pending.size else x
        n = (len(buf) // FRAME) * FRAME
        for i in range(0, n, FRAME):
            await self._ws.send(_pack({"type": "Audio", "pcm": buf[i : i + FRAME].tolist()}))
        self._pending = buf[n:]
        if final and self._pending.size:
            pad = np.zeros(FRAME, dtype=np.float32)
            pad[: self._pending.size] = self._pending
            await self._ws.send(_pack({"type": "Audio", "pcm": pad.tolist()}))
            self._pending = np.zeros(0, dtype=np.float32)

    def _check(self) -> None:
        if self._failure:
            raise KyutaiError(self._failure)

    async def feed(self, pcm16: bytes) -> None:
        self._check()
        await self._send_frames(self._rs.process(pcm16_to_float(pcm16)))

    async def finish(self, timeout: float = 5.0) -> str:
        """Marker + silence until the marker comes back; the words before it are the transcript."""
        self._check()
        self._finishing = True
        await self._send_frames(self._rs.flush(), final=True)
        await self._ws.send(_pack({"type": "Marker", "id": self._marker_id}))
        silence = np.zeros(FRAME, dtype=np.float32).tolist()
        sent = 0.0
        # Push the marker through the model's delay as fast as the server takes it.
        while not self._ended.is_set() and sent < self.flush_s:
            await self._ws.send(_pack({"type": "Audio", "pcm": silence}))
            sent += FRAME / MODEL_RATE
            await asyncio.sleep(0)
        try:
            await asyncio.wait_for(self._ended.wait(), timeout)
        except TimeoutError as e:
            raise KyutaiError("stt marker did not come back in time") from e
        self._check()
        if not self._marker_seen:  # unreachable by construction; never turn it into an empty success
            raise KyutaiError("stt receiver ended without the marker")
        return " ".join(w for w in self.words if w).strip()

    async def close(self) -> None:
        """Frees the server channel now (always call it)."""
        if self._ws is not None:
            try:
                await self._ws.close()
            except Exception:  # noqa: BLE001 - closing a dead socket
                pass
        if self._recv is not None and not self._recv.done():
            self._recv.cancel()
            try:
                await self._recv
            except (asyncio.CancelledError, Exception):  # noqa: BLE001
                pass


async def synthesize(url: str, api_key: str, text: str, voice: str, on_audio: Callable[[np.ndarray], Awaitable[None]], *, connect=ws_connect) -> int:
    """Streams `text` through TTS; on_audio gets float32 @24 kHz chunks as they come. Returns samples received."""
    q = urlencode({"voice": voice, "format": "PcmMessagePack"})
    sep = "&" if "?" in url else "?"
    total = 0
    async with connect(f"{url}{sep}{q}", additional_headers=_headers(api_key), max_size=None, ping_interval=None) as ws:
        for w in text.split():
            await ws.send(_pack({"type": "Text", "text": w}))
        await ws.send(_pack({"type": "Eos"}))
        try:
            async for m in ws:
                d = msgpack.unpackb(m, raw=False)
                t = d.get("type") if isinstance(d, dict) else None
                if t == "Audio":
                    pcm = np.asarray(d.get("pcm", []), dtype=np.float32)
                    total += pcm.size
                    if pcm.size:
                        await on_audio(pcm)
                elif t == "Error":
                    raise KyutaiError(f"tts server error: {d.get('message', '')}"[:200])
        except ConnectionClosed as e:
            if e.rcvd is None or e.rcvd.code not in (1000, 1001):
                raise KyutaiError("tts server closed the stream abnormally") from e
    if total == 0:
        raise KyutaiError("tts returned no audio")
    return total
