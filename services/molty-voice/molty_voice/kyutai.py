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
# moshi-server's TTS word timing is one model step late: it counts the step before it marks
# the word's start (py_module.rs: steps += 1, then prev_word_steps = steps), so the word's
# voice starts at start_s - 0.08 s. Measured against forced alignment (MMS_FA) on 240 words,
# 3 voices: raw start_s is late by median +91 ms (mean +85, sd 94); minus one step the mean
# error is +5 ms. The spread (sd ~90 ms) is the model's own and stays.
TTS_WORD_LATE_S = FRAME_S


class KyutaiError(RuntimeError):
    pass


# A last word ending in one of these closes a sentence (Kyutai's STT punctuates its Words:
# "meters.", "it?", "carve it," — a comma or no mark is mid-sentence). An ellipsis trails off.
SENTENCE_END = (".", "?", "!")
# Hesitations: never the end of a thought, even with a full stop after them ("um.").
# Conjunctions / prepositions / articles ("and", "from", "the", "like", "so", …) need no list:
# unpunctuated they are mid-sentence like any other unpunctuated word, and with a mark the
# mark wins ("What are you thinking of?", "I think so.").
HESITATIONS = frozenset({"um", "uh", "uhm", "umm", "erm", "er", "hmm", "mm", "ah", "eh"})


def sentence_complete(word: str) -> bool:
    """True when the transcript's last word ends a sentence (terminal . ? !, not an ellipsis nor a hesitation)."""
    w = word.strip().rstrip("\"')»”’)]")
    if not w.endswith(SENTENCE_END) or w.endswith(("..", "…")):
        return False
    return w.rstrip(".?!").strip(".,;:\"'«“‘(").lower() not in HESITATIONS


class PausePredictor:
    """Semantic end of turn from the STT's Step messages — Kyutai Unmute's rule.

    unmute/stt/speech_to_text.py + unmute_handler.py determine_pause: an exponential
    moving average of prs[2] (attack/release half-life 0.01 s, starting at 1.0), the
    first 12 steps ignored ("all over the place"), and a pause when it exceeds 0.6
    while the user is speaking — i.e. once they said something. Here: `update()`
    returns True exactly once, the first time that holds after >= 1 word. Pure
    silence never ends a turn that way.

    Mid-sentence hold (ours, not Unmute's): a thinking pause inside a sentence ("…at 2 meters
    from" + 0.6 s) raises prs[2] past the threshold just like the end of a turn. So the pause
    ends the turn at once only when the transcript so far reads as complete — its last Word
    ends a sentence (`sentence_complete`); otherwise the pause must persist for `hold_s`
    of continuous model steps above the threshold. The judgement uses the latest Word at each
    step (Words lag the voice by ~0.5-1.6 s, so the sentence's own last word may land during
    the hold and end it at once), and any Word arriving during the hold restarts it (the
    human is still talking, or the model is still catching up). hold_s 0: Unmute's rule.

    `no_speech(s)`: the other end — nobody spoke. True exactly once, the first step at which
    the model has heard >= s seconds of audio since the last voice activity (since the
    capture began, if none), no Word came, and nobody is speaking right then (the pause
    estimate above NO_SPEECH_QUIET). Voice activity is the model's own "someone is speaking"
    — prs[2] below ACTIVE_BELOW for ACTIVE_STEPS steps in a row — not a Word: Words lag the
    voice by 1.0-1.6 s (measured: speech from 3.8 s, first Word at 4.8-5.4 s), while prs[2]
    drops 0.3-0.45 s into it, over noise floors of -65..-40 dBFS and 15 dB quieter speech,
    and never on noise or knocks (prs[2] stayed >= 0.80). Frame energy was faster (0-0.12 s)
    but missed quiet speech in noise and took a knock for a voice, so it only VETOES: a loud
    frame within the last VETO_STEPS (`voice_recent`, see EnergyGate) postpones the verdict —
    a voice starting just before the deadline is not cut while prs[2] catches up. The clock
    is the audio fed (model steps), i.e. capture time, never wall time from stt.start. Never
    once a word came, and never after the turn ended.
    """

    NO_SPEECH_QUIET = 0.5
    ACTIVE_BELOW = 0.3
    ACTIVE_STEPS = 3

    SKIP_STEPS = 12

    def __init__(self, threshold: float = 0.6, attack_s: float = 0.01, release_s: float = 0.01, skip_steps: int = SKIP_STEPS,
                 hold_s: float = 1.2):
        self.threshold, self.attack_s, self.release_s = threshold, attack_s, release_s
        self.skip_steps = max(0, int(skip_steps))
        self.hold_s = max(0.0, float(hold_s))
        self.value = 1.0
        self.steps = 0
        self.words = 0
        self.last_word = ""  # the latest non-empty Word (Kyutai attaches punctuation to words)
        self.held = 0  # continuous paused steps since the pause began or the last Word came
        self.fired = False
        self.no_speech_fired = False
        self.active_run = 0
        self.quiet_since = 0  # step of the last voice activity (0: the capture's start)

    def word(self, text: str = "") -> None:
        self.words += 1
        self.held = 0  # a Word during the hold: still talking (or the model catching up) — start over
        if text.strip():
            self.last_word = text.strip()

    def update(self, pause_prob: float, dt: float = FRAME_S) -> bool:
        self.steps += 1
        if self.steps <= self.skip_steps:
            return False
        p = max(0.0, float(pause_prob))
        self.active_run = self.active_run + 1 if p < self.ACTIVE_BELOW else 0
        if self.active_run >= self.ACTIVE_STEPS:
            self.quiet_since = self.steps  # someone is speaking: the no-speech clock starts over
        half = self.attack_s if p > self.value else self.release_s
        alpha = 1 - math.exp(-dt / half * math.log(2))
        self.value = (1 - alpha) * self.value + alpha * p
        if self.fired or self.threshold <= 0 or self.words == 0:
            return False
        if self.value <= self.threshold:
            self.held = 0  # speaking again: the pause is over
            return False
        self.held += 1
        if self.hold_s > 0 and not sentence_complete(self.last_word) and self.held * dt < self.hold_s - 1e-9:
            return False  # mid-sentence: wait for the pause to last hold_s
        self.fired = True
        return True

    def no_speech(self, after_s: float, dt: float = FRAME_S, voice_recent: bool = False) -> bool:
        """Call after update() on each step: True once when nobody spoke for after_s of audio."""
        if self.no_speech_fired or self.fired or self.words or after_s <= 0 or voice_recent:
            return False
        if (self.steps - self.quiet_since) * dt < after_s - 1e-9 or self.value <= self.NO_SPEECH_QUIET:
            return False
        self.no_speech_fired = True
        return True


class EnergyGate:
    """Loud 80 ms frames over the noise floor, on the audio as it goes to the model.

    A frame is loud when its RMS is MARGIN_DB over the tracked noise floor (follows drops at
    once, rises 0.05 dB per frame) and over ABS_DB. `recent(step)`: a loud frame among the
    frames at or after `step - VETO_STEPS` — frames sent run ahead of the model's steps, so a
    loud frame not yet processed counts too.
    """

    MARGIN_DB = 12.0
    ABS_DB = -55.0
    VETO_STEPS = 6

    def __init__(self):
        self.floor: float | None = None
        self.frames = 0
        self.last_loud = -(10**9)

    def frame(self, x: np.ndarray) -> None:
        self.frames += 1
        r = 20 * math.log10(math.sqrt(float(np.mean(np.square(x, dtype=np.float64)))) + 1e-9)
        if self.floor is None or r < self.floor:
            self.floor = r
        else:
            self.floor = min(r, self.floor + 0.05)
        if r > self.floor + self.MARGIN_DB and r > self.ABS_DB:
            self.last_loud = self.frames

    def recent(self, step: int) -> bool:
        return self.last_loud >= step - self.VETO_STEPS


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
        turn_end_skip_steps: int = PausePredictor.SKIP_STEPS,
        turn_end_hold_s: float = 1.2,
        on_no_speech: Callable[[], Awaitable[None]] | None = None,
        no_speech_s: float | None = None,
        observer=None,
        on_hint: Callable[[str], Awaitable[None]] | None = None,
        connect=ws_connect,
    ):
        self.url, self.api_key, self.flush_s, self.on_word, self._connect = url, api_key, flush_s, on_word, connect
        self.on_turn_end = on_turn_end
        self.pause = PausePredictor(turn_end_threshold, skip_steps=turn_end_skip_steps, hold_s=turn_end_hold_s)
        # nobody spoke within no_speech_s (None: off): the relay turned the device's VAD off
        self.on_no_speech, self.no_speech_s = on_no_speech, no_speech_s
        self._finishing = False
        # turn trace (molty_voice/trace.py SttTrace): told every Word / EndWord / Step before the
        # flush; its hints (pause / question) go to on_hint
        self.observer, self.on_hint = observer, on_hint
        self.energy = EnergyGate()
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
                    self.pause.word(self.words[-1])
                    hint = self._observe("word", self.words[-1], d.get("start_time")) if not self._finishing else None
                    if self.on_word:
                        await self.on_word(" ".join(self.words))
                    await self._hint(hint)
                elif t == "EndWord":
                    self._observe("end_word", d.get("stop_time"))
                elif t == "Step":
                    prs = d.get("prs") or []
                    if len(prs) > 2:
                        # after finish() began, the flush silence is ours, not the human's pause
                        fired = self.pause.update(prs[2])
                        turn = bool(fired and not self._finishing and self.on_turn_end)
                        ns = bool(not turn and self.no_speech_s and self.on_no_speech and not self._finishing
                                  and self.pause.no_speech(self.no_speech_s, voice_recent=self.energy.recent(self.pause.steps)))
                        hint = self._observe("step", self.pause, prs[2], turn, ns) if not self._finishing else None
                        if turn:
                            await self.on_turn_end()
                        elif ns:
                            await self.on_no_speech()
                        else:
                            await self._hint(hint)
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

    def _observe(self, what: str, *args):
        """The turn trace's observer, strictly best effort: a failure there is logged once and the
        observer dropped — it never touches the transcript or a decision."""
        if self.observer is None:
            return None
        try:
            return getattr(self.observer, what)(*args)
        except Exception as e:  # noqa: BLE001 - observation must never break transcription
            log.warning("stt trace observer failed (%s), tracing off for this listen: %s: %s", what, type(e).__name__, e)
            self.observer = None
            return None

    async def _hint(self, kind) -> None:
        """A listen hint, best effort (a failing callback is logged once, then hints stop)."""
        if not kind or self.on_hint is None:
            return
        try:
            await self.on_hint(kind)
        except Exception as e:  # noqa: BLE001 - a hint must never break transcription
            log.warning("stt hint callback failed, hints off for this listen: %s: %s", type(e).__name__, e)
            self.on_hint = None

    async def _send_frames(self, x: np.ndarray, final: bool = False) -> None:
        buf = np.concatenate([self._pending, x]) if self._pending.size else x
        n = (len(buf) // FRAME) * FRAME
        for i in range(0, n, FRAME):
            if not self._finishing:
                self.energy.frame(buf[i : i + FRAME])
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


async def synthesize(url: str, api_key: str, text: str, voice: str, on_audio: Callable[[np.ndarray], Awaitable[None]], *,
                     on_word: Callable[[str, float], Awaitable[None]] | None = None, connect=ws_connect) -> int:
    """Streams `text` through TTS; on_audio gets float32 @24 kHz chunks as they come. Returns samples received.

    on_word(text, start_s): the server's word timing (moshi-server tts `Text {text, start_s,
    stop_s}`), one per word sent, in order: the word is spoken from start_s seconds into this
    stream's audio — the server's start_s less its one-step lag (TTS_WORD_LATE_S), never < 0.
    """
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
                elif t == "Text" and on_word is not None:
                    s = d.get("start_s")
                    if isinstance(s, (int, float)) and not isinstance(s, bool) and math.isfinite(s) and s >= 0:
                        await on_word(str(d.get("text", "")), max(0.0, float(s) - TTS_WORD_LATE_S))
                elif t == "Error":
                    raise KyutaiError(f"tts server error: {d.get('message', '')}"[:200])
        except ConnectionClosed as e:
            if e.rcvd is None or e.rcvd.code not in (1000, 1001):
                raise KyutaiError("tts server closed the stream abnormally") from e
    if total == 0:
        raise KyutaiError("tts returned no audio")
    return total
