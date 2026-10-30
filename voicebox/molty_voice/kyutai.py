"""Clients for Kyutai's moshi-server (STT `/api/asr-streaming`, TTS `/api/tts_streaming`).

Protocol (msgpack over WebSocket, header `kyutai-api-key`), as in Kyutai's reference
clients `scripts/stt_from_file_rust_server.py` / `scripts/tts_rust_server.py`:

STT  client -> {"type":"Audio","pcm":[float @24 kHz]} ... {"type":"Marker","id":n}
     server -> {"type":"Word","text","start_time"} {"type":"EndWord"} {"type":"Step",...}
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
from collections.abc import Awaitable, Callable
from urllib.parse import urlencode

import msgpack
import numpy as np
from websockets.asyncio.client import connect as ws_connect
from websockets.exceptions import ConnectionClosed

from .audio import DEVICE_RATE, MODEL_RATE, Resampler, pcm16_to_float

log = logging.getLogger("molty_voice.kyutai")

FRAME = 1920  # 80 ms @24 kHz: the model's step


class KyutaiError(RuntimeError):
    pass


def _pack(obj) -> bytes:
    return msgpack.packb(obj, use_single_float=True)


def _headers(api_key: str) -> dict[str, str]:
    return {"kyutai-api-key": api_key}


class SttSession:
    """One streaming transcription: open(), feed() PCM16 @16 kHz as it arrives, finish() -> text."""

    def __init__(self, url: str, api_key: str, *, flush_s: float = 6.0, on_word: Callable[[str], Awaitable[None]] | None = None, connect=ws_connect):
        self.url, self.api_key, self.flush_s, self.on_word, self._connect = url, api_key, flush_s, on_word, connect
        self.words: list[str] = []
        self._rs = Resampler(DEVICE_RATE, MODEL_RATE)
        self._pending = np.zeros(0, dtype=np.float32)
        self._ws = None
        self._recv: asyncio.Task | None = None
        self._marker = asyncio.Event()
        self._closed_early: str | None = None
        self._marker_id = 1

    async def open(self) -> None:
        self._ws = await self._connect(self.url, additional_headers=_headers(self.api_key), max_size=None, ping_interval=None)
        self._recv = asyncio.create_task(self._receive())

    async def _receive(self) -> None:
        try:
            async for m in self._ws:
                d = msgpack.unpackb(m, raw=False)
                t = d.get("type") if isinstance(d, dict) else None
                if t == "Word" and not self._marker.is_set():
                    w = str(d.get("text", ""))
                    self.words.append(w)
                    if self.on_word:
                        await self.on_word(" ".join(self.words))
                elif t == "Marker":
                    self._marker.set()
                    return
                elif t == "Error":
                    self._closed_early = f"stt server error: {d.get('message', '')}"[:200]
                    return
            self._closed_early = "stt server closed the stream before the marker"
        except ConnectionClosed as e:
            self._closed_early = f"stt server closed the stream before the marker ({e.rcvd.reason if e.rcvd else 'no reason'})"[:200]
        finally:
            if not self._marker.is_set():
                self._marker.set()  # wake finish(): it checks _closed_early

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
        if self._closed_early:
            raise KyutaiError(self._closed_early)

    async def feed(self, pcm16: bytes) -> None:
        self._check()
        await self._send_frames(self._rs.process(pcm16_to_float(pcm16)))

    async def finish(self, timeout: float = 5.0) -> str:
        """Marker + silence until the marker comes back; the words before it are the transcript."""
        self._check()
        await self._send_frames(self._rs.flush(), final=True)
        await self._ws.send(_pack({"type": "Marker", "id": self._marker_id}))
        silence = np.zeros(FRAME, dtype=np.float32).tolist()
        sent = 0.0
        # Push the marker through the model's delay as fast as the server takes it.
        while not self._marker.is_set() and sent < self.flush_s:
            await self._ws.send(_pack({"type": "Audio", "pcm": silence}))
            sent += FRAME / MODEL_RATE
            await asyncio.sleep(0)
        try:
            await asyncio.wait_for(self._marker.wait(), timeout)
        except TimeoutError as e:
            raise KyutaiError("stt marker did not come back in time") from e
        self._check()
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
