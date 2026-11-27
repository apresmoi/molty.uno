"""The conversational front: the engine seam, the owner's daily minutes, and what a session costs.

A front engine is Molty's outer voice (docs/AGENT-LINK.md "Voice front"): it hears the person as
they speak, ends their turn, answers at once in its own voice, and speaks the agent's answers when
they come. The bridge (molty_voice/bridge.py) routes the relay's voice-box messages to it; the
engine answers through `Out`. Any engine that keeps this contract plugs in the same way: the first
is a realtime speech-to-speech model (molty_voice/realtime.py); a later one could be the local
Kyutai STT + a small fast model + Kyutai TTS, without touching the bridge or the relay.

  listen_start(id, front, no_speech_s)  relay stt.start: a listen opens (front: answer it yourself at its end)
  listen_audio(id, pcm16)               Molty's mic, 16 kHz PCM16, as it arrives
  listen_end(id) / listen_cancel(id)    relay stt.end / stt.cancel
  speak(id, text, mode, exact)          relay tts: mode "inner" (the agent's thought: voiced in the front's words,
                                        or absorbed: tts.end absorbed; exact keeps its wording), "wait" (still
                                        waiting on the agent), "plain" (verbatim TTS, the front off)
  speak_cancel(id)                      relay tts.cancel
  context(text, persona)                relay front.context: silent background from the agent, kept across
                                        sessions (persona: its standing persona line, in every session's brief)
  end(reason)                           relay front.end: the conversation is over, close the session
  close()                               the relay socket went away

The engine answers with the existing voice-box messages (stt.turn_end, stt.partial, stt.final, audio
frames, tts.word, tts.end) plus stt.hint {k:"speech"}, front.reply, tts.text and front.session.
"""

from __future__ import annotations

import json
import logging
import os
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Awaitable, Callable, Protocol

log = logging.getLogger("molty_voice")

SendJson = Callable[[dict], Awaitable[None]]
SendBin = Callable[[bytes], Awaitable[None]]


class FrontEngine(Protocol):
    async def listen_start(self, mid: str, front: bool, no_speech_s: float | None) -> None: ...
    async def listen_audio(self, mid: str, pcm16: bytes) -> None: ...
    async def listen_end(self, mid: str) -> None: ...
    async def listen_cancel(self, mid: str) -> None: ...
    async def speak(self, mid: str, text: str, mode: str) -> None: ...
    async def speak_cancel(self, mid: str) -> None: ...
    async def context(self, text: str) -> None: ...
    async def end(self, reason: str) -> None: ...
    async def close(self) -> None: ...
    def available(self) -> bool: ...


# US$ per 1M tokens, from developers.openai.com/api/docs/pricing (read 2026-11-27): audio in, cached
# audio in, audio out, text in, text out. Only for the usage line in the log: the provider's own
# usage page is the bill.
PRICES = {
    "gpt-realtime-2.1-mini": (10.0, 0.30, 20.0, 0.60, 2.40),
    "gpt-realtime-mini": (10.0, 0.30, 20.0, 0.60, 2.40),
    "gpt-realtime-2.1": (32.0, 0.40, 64.0, 4.0, 24.0),
    "gpt-realtime": (32.0, 0.40, 64.0, 4.0, 16.0),
}


@dataclass
class Usage:
    """Token counts summed over a session's responses (response.done usage)."""

    audio_in: int = 0
    audio_in_cached: int = 0
    audio_out: int = 0
    text_in: int = 0
    text_in_cached: int = 0
    text_out: int = 0
    responses: int = 0

    def add(self, u: dict) -> None:
        if not isinstance(u, dict):
            return
        i = u.get("input_token_details") if isinstance(u.get("input_token_details"), dict) else {}
        o = u.get("output_token_details") if isinstance(u.get("output_token_details"), dict) else {}
        c = i.get("cached_tokens_details") if isinstance(i.get("cached_tokens_details"), dict) else {}
        n = lambda d, k: int(d.get(k) or 0) if isinstance(d.get(k), (int, float)) else 0  # noqa: E731
        self.audio_in += n(i, "audio_tokens")
        self.audio_in_cached += n(c, "audio_tokens")
        self.text_in += n(i, "text_tokens")
        self.text_in_cached += n(c, "text_tokens")
        self.audio_out += n(o, "audio_tokens")
        self.text_out += n(o, "text_tokens")
        self.responses += 1

    def cost(self, model: str) -> float | None:
        p = PRICES.get(model)
        if p is None:
            return None
        a_in, a_cached, a_out, t_in, t_out = p
        return (
            (self.audio_in - self.audio_in_cached) * a_in + self.audio_in_cached * a_cached + self.audio_out * a_out
            + (self.text_in - self.text_in_cached) * t_in + self.text_in_cached * (t_in / 10) + self.text_out * t_out
        ) / 1e6

    def to_json(self) -> dict:
        return {k: v for k, v in self.__dict__.items()}


@dataclass
class UsageBook:
    """The owner's daily minutes (wall time of open sessions), kept next to the config (0600).
    The day is the local calendar day of this machine."""

    path: Path | None
    day: str = ""
    seconds: float = 0.0
    sessions: list = field(default_factory=list)

    @staticmethod
    def today() -> str:
        return time.strftime("%Y-%m-%d")

    @classmethod
    def load(cls, path: Path | None) -> "UsageBook":
        b = cls(path)
        try:
            if path and path.exists():
                d = json.loads(path.read_text())
                b.day, b.seconds, b.sessions = str(d.get("day", "")), float(d.get("seconds", 0)), list(d.get("sessions", []))[-20:]
        except (ValueError, OSError, TypeError) as e:
            log.warning("realtime usage file unreadable, starting a new day count: %s", e)
        b._roll()
        return b

    def _roll(self) -> None:
        if self.day != self.today():
            self.day, self.seconds = self.today(), 0.0

    def used_s(self) -> float:
        self._roll()
        return self.seconds

    def add(self, seconds: float, entry: dict) -> None:
        self._roll()
        self.seconds += max(0.0, seconds)
        self.sessions = (self.sessions + [entry])[-20:]
        if not self.path:
            return
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            tmp = self.path.with_suffix(".tmp")
            fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, "w") as f:
                json.dump({"day": self.day, "seconds": round(self.seconds, 1), "sessions": self.sessions}, f, indent=1)
            os.replace(tmp, self.path)
        except OSError as e:
            log.warning("realtime usage not saved: %s", e)
