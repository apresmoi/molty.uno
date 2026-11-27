"""The realtime front engine: a realtime speech-to-speech model as Molty's outer voice.

The first provider is OpenAI's Realtime API over a server-to-server WebSocket. Everything below was
checked against developers.openai.com on 2026-11-27 (guides realtime-websocket, realtime-vad,
realtime-conversations, realtime-transcription, realtime-costs; the realtime API reference):

- connect  wss://api.openai.com/v1/realtime?model=<model>, header `Authorization: Bearer <key>`
           (the GA API needs no OpenAI-Beta header).
- audio    in and out `audio/pcm` at 24 kHz, 16-bit mono little endian (the only PCM rate it takes).
           Molty records and plays 16 kHz: in, 16k -> 24k (up 3 / down 2); out, 24k -> 16k (up 2 /
           down 3), both with the streaming polyphase resampler of molty_voice/audio.py, chunk by
           chunk as the audio arrives (no whole utterance is ever buffered).
- turns    server-side turn detection (semantic_vad by default, server_vad on request) ends the
           person's turn, with create_response OFF: the box itself asks for the reply on the commit
           (rule 1 below; live, an auto response was lost when turn detection went off right after
           the turn's end). `interrupt_response` is off: Molty cannot hear while it speaks (half
           duplex). Turn detection is on only during a listen (session.update at its start,
           turn_detection null at its end); a held listen (the button) has none at all: the release
           ends it, and the box commits and asks for the reply.
- state    one conversation, one place that decides (docs/AGENT-LINK.md "Voice front"):
           LISTENING (a listen is open), REPLYING (a response is made or played), WAITING (the
           front handed the last turn to the agent with a holding line: `_waiting_for`), IDLE (no
           session).
           1. every turn end of a front listen gets exactly ONE front response (`_reply`): its own
              answer, or a holding line and hand_to_agent (then WAITING for that turn);
           2. an agent line (tts front "inner", `turn`: the listen it answers) is voiced only when
              the front is WAITING for that turn ("ok, got it:" and the facts), and with no `turn`
              only when no conversation is going on (the agent speaking up: it opens the session).
              Anything else is context: kept in the instructions, never voiced, no response, no
              session opened; tts.end says absorbed at once;
           3. background (the agent's lines, context, its persona) lives in the session
              instructions as things already said or known, never as items a model may recite.
           Inline [cues] are never spoken.
- one response at a time in the conversation: requests wait in a queue for `response.done`.
- usage    `response.done.response.usage` (input / output token details): summed per session.
           Audio is 1 token per 100 ms in and 1 per 50 ms out.

The key is the owner's, from the bridge's own config file (0600) or the environment: it is only
ever put in the Authorization header of this one connection; never logged, printed or sent to the relay.
"""

from __future__ import annotations

import asyncio
import base64
import contextlib
import json
import logging
import random
import time
from dataclasses import dataclass, field

import numpy as np
from websockets.asyncio.client import connect as ws_connect
from websockets.exceptions import ConnectionClosed

from . import brief, framing
from .audio import DEVICE_RATE, Resampler, float_to_pcm16, pcm16_to_float
from .front import SendBin, SendJson, Usage, UsageBook

log = logging.getLogger("molty_voice")
# The websockets library logs the opening handshake's headers at DEBUG (the Authorization header with
# the key in it): this connection gets its own logger, held at WARNING whatever the app's logging says.
_ws_log = logging.getLogger("molty_voice.realtime.socket")
_ws_log.setLevel(logging.WARNING)

PROVIDER_RATE = 24000
# The model ended a turn: its own commit comes at once (2 ms live); without it this long, the box commits.
COMMIT_WAIT_S = 1.0
# What the front keeps of the agent across sessions: its last lines, each and in all at most this long.
# Lines older than the relay's longest conversation (15 min) are dropped.
MEMORY_LINES, MEMORY_LINE, MEMORY_CHARS, MEMORY_MAX_AGE_S = 8, 500, 2000, 900.0
# A held listen has no turn detection: a 100 ms chunk this loud (about -46 dBFS) is the person speaking.
HOLD_VOICE_RMS = 0.005


@dataclass
class _Listen:
    mid: str
    front: bool
    no_speech_s: float | None
    hold: bool = False  # held to talk: the release ends it, never the model
    rs: Resampler = field(default_factory=lambda: Resampler(DEVICE_RATE, PROVIDER_RATE))
    samples: int = 0  # 16 kHz samples in
    speech: bool = False
    turned: bool = False
    committed: bool = False
    ended: bool = False
    replied: bool = False  # its one front response is asked for
    finaled: bool = False
    item: str | None = None
    text: str = ""
    t_stop: float | None = None
    t_end: float | None = None


@dataclass
class _Speech:
    sid: str  # the stream id towards the relay (a tts id, or a front.reply id)
    kind: str  # reply | inner | wait | plain (a tts with the front off: word for word)
    text: str = ""  # the request's text (exact / own)
    listen: str | None = None  # reply: the listen it answers
    rs: Resampler = field(default_factory=lambda: Resampler(PROVIDER_RATE, DEVICE_RATE))
    seq: int = 0
    samples: int = 0  # 16 kHz samples sent
    said: str = ""  # the output transcript so far
    words: int = 0  # words cued so far (tts.word)
    last_start: float = 0.0
    text_sent: str = ""
    text_final: bool = False
    t0: float = 0.0
    first: float | None = None
    wait: bool = False
    rid: str | None = None  # the provider's response id
    cancelled: bool = False
    exact: bool = False  # inner: keep the agent's wording as close as it can
    awaited: bool = False  # inner: the answer the front waits for (WAITING)


def realtime_url(base: str, model: str) -> str:
    sep = "&" if "?" in base else "?"
    return f"{base}{sep}model={model}"


class RealtimeFront:
    """One realtime session at a time, opened on the first request and kept across turns (the model
    remembers the conversation), closed on idle, at the owner's limits, on `end` and on disconnect."""

    def __init__(self, cfg, send_json: SendJson, send_bin: SendBin, *, connect=ws_connect, book: UsageBook | None = None,
                 on_available=None):
        self.cfg = cfg
        self.key = cfg.front_key()
        self._send_json = send_json
        self._send_bin = send_bin
        self._connect = connect
        self.book = book or UsageBook(None)
        self._on_available = on_available  # called when the daily cap is reached or a new day frees it
        self._ws = None
        self._reader: asyncio.Task | None = None
        self._watch: asyncio.Task | None = None
        self._open_lock = asyncio.Lock()
        self._send_lock = asyncio.Lock()
        self._vad_on = False
        self.opened_at = 0.0
        self.last_activity = 0.0
        self.usage = Usage()
        self._listen: _Listen | None = None
        self._items: dict[str, _Listen] = {}
        self._queue: list[_Speech] = []
        self._creating: _Speech | None = None
        self._active: _Speech | None = None
        self._n = 0
        self._memory: list[tuple[float, str]] = []  # (when, line): the agent's lines and context, background (MEMORY_* caps)
        self._persona = ""  # the agent's standing persona line (front {context, persona}), in every session's brief
        self._waiting_for: str | None = None  # WAITING: the listen whose turn the front handed to the agent
        self._talking = False  # a conversation is going on: a front listen ran in this session
        self.capped = False
        self.close_reason = ""

    # ---------------- availability and limits ----------------

    def available(self) -> bool:
        return bool(self.key) and not self._over_cap()

    def _over_cap(self) -> bool:
        cap = self.cfg.realtime_daily_minutes
        if cap <= 0:
            return False
        live = time.monotonic() - self.opened_at if self._ws is not None else 0.0
        return self.book.used_s() + live >= cap * 60

    async def _watchdog(self) -> None:
        while self._ws is not None:
            await asyncio.sleep(1.0)
            now = time.monotonic()
            busy = self._listen is not None and not self._listen.ended or self._active is not None or self._creating is not None
            if not busy and now - self.last_activity > self.cfg.realtime_idle_s:
                await self._close("idle")
            elif now - self.opened_at > self.cfg.realtime_max_session_s:
                await self._close("max_session")
            elif self._over_cap():
                await self._close("daily_cap")

    async def tick_day(self) -> None:
        """A new day lifts the cap (the bridge calls it now and then)."""
        if self.capped and not self._over_cap():
            self.capped = False
            log.info("realtime: a new day, the daily minutes are back")
            if self._on_available:
                await self._on_available()

    # ---------------- the provider socket ----------------

    def _turn_detection(self) -> dict:
        td = {"create_response": False, "interrupt_response": False}  # the box asks for each reply itself (_reply)
        if self.cfg.realtime_turn == "server":
            return {"type": "server_vad", "threshold": 0.5, "prefix_padding_ms": 300, "silence_duration_ms": self.cfg.realtime_silence_ms, **td}
        return {"type": "semantic_vad", "eagerness": self.cfg.realtime_eagerness, **td}

    def _session(self) -> dict:
        audio_in = {"format": {"type": "audio/pcm", "rate": PROVIDER_RATE}, "turn_detection": None,
                    "noise_reduction": {"type": "near_field"}}
        if self.cfg.realtime_transcribe:
            audio_in["transcription"] = {"model": self.cfg.realtime_transcribe}
        return {
            "type": "realtime",
            "output_modalities": ["audio"],
            "instructions": self._instructions(),
            "audio": {"input": audio_in, "output": {"format": {"type": "audio/pcm", "rate": PROVIDER_RATE}, "voice": self.cfg.realtime_voice}},
            "tools": brief.TOOLS,
            "tool_choice": "auto",
            "max_output_tokens": self.cfg.realtime_max_output_tokens,
        }

    async def _ensure(self) -> bool:
        """The session, opened when needed (turn detection off until a listen). False: unavailable."""
        async with self._open_lock:
            if self._ws is None:
                if not self.key:
                    log.warning("realtime: no key configured (realtime_key in the config, or MOLTY_REALTIME_KEY / OPENAI_API_KEY)")
                    return False
                if self._over_cap():
                    await self._cap_reached()
                    return False
                url = realtime_url(self.cfg.realtime_url, self.cfg.realtime_model)
                t0 = time.monotonic()
                try:
                    self._ws = await self._connect(url, additional_headers={"Authorization": f"Bearer {self.key}"}, ping_interval=20,
                                                   max_size=1 << 24, open_timeout=10, logger=_ws_log)
                except Exception as e:  # noqa: BLE001 - never echo the request (it carries the key)
                    log.warning("realtime: could not open a session: %s", _why(e))
                    self._ws = None
                    return False
                self.opened_at = self.last_activity = time.monotonic()
                self.usage = Usage()
                self.close_reason = ""
                self._vad_on = False
                await self._send({"type": "session.update", "session": self._session()})
                self._reader = asyncio.create_task(self._read(self._ws))
                self._watch = asyncio.create_task(self._watchdog())
                log.info("realtime: session open (%s, voice %s, %s turns) in %.0f ms", self.cfg.realtime_model, self.cfg.realtime_voice,
                         self.cfg.realtime_turn, (time.monotonic() - t0) * 1000)
                await self._send_json({"t": "front.session", "state": "open", "model": self.cfg.realtime_model})
            self.last_activity = time.monotonic()
            return True

    async def _vad(self, td: dict | None) -> None:
        """Turn detection on (td) for a listen, or off (None) between listens: a new setting resets its state."""
        if td is None and not self._vad_on:
            return
        self._vad_on = td is not None
        await self._send({"type": "session.update", "session": {"type": "realtime", "audio": {"input": {"turn_detection": td}}}})

    async def _send(self, ev: dict) -> None:
        ws = self._ws
        if ws is None:
            return
        async with self._send_lock:
            try:
                await ws.send(json.dumps(ev, separators=(",", ":")))
            except ConnectionClosed:
                pass

    async def _cap_reached(self) -> None:
        if not self.capped:
            self.capped = True
            log.warning("realtime: the daily limit of %s minutes is reached: no more sessions today", self.cfg.realtime_daily_minutes)
            if self._on_available:
                await self._on_available()

    async def _close(self, reason: str) -> None:
        ws, self._ws = self._ws, None
        self._talking, self._waiting_for = False, None
        if ws is None:
            return
        self.close_reason = reason
        with contextlib.suppress(Exception):
            await ws.close()
        for t in (self._reader, self._watch):
            if t is not None and t is not asyncio.current_task():
                t.cancel()
        secs = time.monotonic() - self.opened_at
        cost = self.usage.cost(self.cfg.realtime_model)
        self.book.add(secs, {"at": int(time.time()), "s": round(secs, 1), "reason": reason, **self.usage.to_json()})
        u = self.usage
        log.info("realtime: session closed (%s) after %.1f min: %d responses, audio in %d tokens (%d cached), audio out %d, "
                 "text in %d, text out %d%s; today %.1f of %s min", reason, secs / 60, u.responses, u.audio_in, u.audio_in_cached,
                 u.audio_out, u.text_in, u.text_out, f", about ${cost:.4f}" if cost is not None else "", self.book.used_s() / 60,
                 self.cfg.realtime_daily_minutes or "unlimited")
        await self._fail_all(reason)
        with contextlib.suppress(Exception):
            await self._send_json({"t": "front.session", "state": "closed", "reason": reason, "s": round(secs, 1), "usage": u.to_json(),
                                   "today_s": round(self.book.used_s(), 1)})
        if reason == "daily_cap":
            await self._cap_reached()

    async def _fail_all(self, why: str) -> None:
        """The session is gone: whatever was in flight ends now (never left hanging)."""
        lst = self._listen
        self._items.clear()
        if lst and not lst.finaled:
            lst.finaled = True
            await self._send_json({"t": "stt.final", "id": lst.mid, "error": f"realtime session closed ({why})"})
        pending = [s for s in (self._active, self._creating) if s] + self._queue
        self._active = self._creating = None
        self._queue = []
        for s in pending:
            await self._end_speech(s, error=None if s.samples else f"realtime session closed ({why})")

    # ---------------- relay -> engine ----------------

    async def listen_start(self, mid: str, front: bool, no_speech_s: float | None, hold: bool = False) -> None:
        log.info("listen %s: start (%s%s)", mid, "front" if front else "transcript only", ", held" if hold else "")
        if not await self._ensure():
            await self._send_json({"t": "stt.final", "id": mid, "error": "realtime unavailable"})
            return
        self._listen = _Listen(mid, front, no_speech_s, hold)
        self._talking = self._talking or front
        # a clean start: nothing left in the buffer, the detector's state reset (or off while held)
        await self._send({"type": "input_audio_buffer.clear"})
        await self._vad(None if hold else self._turn_detection())

    async def listen_audio(self, mid: str, pcm16: bytes) -> None:
        lst = self._listen
        if lst is None or lst.mid != mid or lst.ended or not pcm16:
            return
        self.last_activity = time.monotonic()
        lst.samples += len(pcm16) // 2
        if lst.turned:
            return  # the turn is over: what Molty still records goes nowhere
        x = pcm16_to_float(pcm16)
        if lst.hold and not lst.speech and float(np.sqrt(np.mean(x * x))) > HOLD_VOICE_RMS:
            lst.speech = True  # no detector while held: a voice loud enough is the person speaking
            if lst.front:
                self._waiting_for = None  # the person speaks again: a new turn wins
        y = lst.rs.process(x)
        if y.size:
            await self._send({"type": "input_audio_buffer.append", "audio": base64.b64encode(float_to_pcm16(y)).decode()})
        if lst.no_speech_s and not lst.speech and lst.samples >= lst.no_speech_s * DEVICE_RATE:
            lst.turned = True
            log.info("stt %s: nobody spoke within %.1f s", mid, lst.no_speech_s)
            await self._send({"type": "input_audio_buffer.clear"})
            await self._send_json({"t": "stt.turn_end", "id": mid, "reason": "no_speech"})

    async def listen_end(self, mid: str) -> None:
        lst = self._listen
        if lst is None or lst.mid != mid or lst.ended:
            return
        lst.ended, lst.t_end = True, time.monotonic()
        await self._vad(None)
        if lst.committed or lst.finaled:
            return  # the turn detection committed it: its transcript is on its way
        if lst.turned and lst.speech and not lst.hold:
            # one turn, one end: the model ended it first (speech_stopped) and the provider commits it itself
            # (its `committed` brings the reply). A second commit here would be empty and fail (live, run 6).
            # Should that commit never come, the box commits after COMMIT_WAIT_S.
            asyncio.get_running_loop().create_task(self._commit_later(lst))
            return
        if not lst.speech or lst.samples < DEVICE_RATE // 10:
            # nothing worth a transcript (or a reply)
            log.info("listen %s: ended by Molty, nobody spoke", mid)
            lst.finaled = True
            await self._send({"type": "input_audio_buffer.clear"})
            await self._send_json({"t": "stt.final", "id": mid, "text": "", "ms": {"final": 0}})
            return
        await self._commit(lst)

    async def _commit_later(self, lst: _Listen) -> None:
        await asyncio.sleep(COMMIT_WAIT_S)
        if not lst.committed and self._listen is lst:
            log.info("listen %s: no commit from the provider: the box commits", lst.mid)
            await self._commit(lst)

    async def _commit(self, lst: _Listen) -> None:
        """Molty ended the turn (a tap, its window, the release), or the provider never committed it: commit, then the one reply."""
        mid = lst.mid
        y = lst.rs.flush()
        if y.size:
            await self._send({"type": "input_audio_buffer.append", "audio": base64.b64encode(float_to_pcm16(y)).decode()})
        # the turn is over now (whoever ended it): the reply below must not wait for the model's own end of turn
        lst.committed, lst.turned, lst.t_stop = True, True, lst.t_stop or lst.t_end
        log.info("listen %s: turn ended by Molty (a release, a tap, its window) after %.1f s: committed%s", mid, lst.samples / DEVICE_RATE,
                 ", the front answers" if lst.front else "")
        await self._send({"type": "input_audio_buffer.commit"})
        if lst.front:
            self._reply(lst)

    async def listen_cancel(self, mid: str) -> None:
        lst = self._listen
        if lst is None or lst.mid != mid:
            return
        lst.ended = lst.finaled = True
        await self._send({"type": "input_audio_buffer.clear"})
        await self._vad(None)

    async def speak(self, mid: str, text: str, mode: str, exact: bool = False, turn: str | None = None) -> None:
        """mode: inner (own / exact from an older relay) | wait | plain. `turn`: the listen an inner line answers."""
        kind = "inner" if mode in ("inner", "own", "exact") else mode if mode in ("wait", "plain") else "plain"
        waiting = kind == "inner" and turn is not None and turn == self._waiting_for
        if kind == "inner" and not waiting and (turn is not None or self._talking):
            # rule 2: not the answer the front waits for (nor the agent speaking up between conversations): context only
            log.info("front %s: inner voice, %d chars, kept as context (%s)", mid, len(text),
                     "the front answered that turn itself" if turn is not None else "a conversation is going on")
            await self._know(f"Your inner voice said: {brief.spoken(text)}")
            await self._send_json({"t": "tts.end", "id": mid, "absorbed": True})
            return
        log.info("front %s: %s, %d chars", mid, ("inner voice, the answer it waits for" if waiting else "inner voice") + (" (exact)" if exact or mode == "exact" else "")
                 if kind == "inner" else kind, len(text))
        if not await self._ensure():
            await self._send_json({"t": "tts.end", "id": mid, "error": "realtime unavailable"})
            return
        if kind == "inner":
            self._waiting_for = None  # answered: LISTENING again after it
            self._remember(f"Your inner voice said: {brief.spoken(text)}")
        self._enqueue(_Speech(sid=mid, kind=kind, text=text, exact=exact or mode == "exact", awaited=waiting))

    async def speak_cancel(self, mid: str) -> None:
        self._queue = [s for s in self._queue if s.sid != mid]
        for s in (self._active, self._creating):
            if s and s.sid == mid and not s.cancelled:
                s.cancelled = True
                await self._send({"type": "response.cancel"})

    def _remember(self, text: str) -> None:
        now = time.monotonic()
        self._memory = [m for m in self._memory if now - m[0] <= MEMORY_MAX_AGE_S][-(MEMORY_LINES - 1):] + [(now, text[:MEMORY_LINE])]
        while sum(len(t) for _, t in self._memory) > MEMORY_CHARS:
            self._memory.pop(0)

    def _instructions(self) -> str:
        now = time.monotonic()
        return brief.instructions(self._persona, [t for at, t in self._memory if now - at <= MEMORY_MAX_AGE_S])

    async def _know(self, text: str) -> None:
        """Background: into the instructions of this session (if one is open) and the next ones; never said."""
        self._remember(text)
        await self._send({"type": "session.update", "session": {"type": "realtime", "instructions": self._instructions()}})

    async def context(self, text: str, persona: bool = False) -> None:
        """Silent context, kept for later sessions; never a session (billed) opened just for it."""
        log.info("front: %s, %d chars", "persona" if persona else "context", len(text))
        if persona:
            self._persona = text[:MEMORY_LINE]
            return await self._send({"type": "session.update", "session": {"type": "realtime", "instructions": self._instructions()}})
        await self._know(text)

    async def end(self, reason: str) -> None:
        await self._close(reason or "ended")

    async def close(self) -> None:
        await self._close("relay_gone")

    # ---------------- responses: one at a time ----------------

    def _new_id(self) -> str:
        self._n += 1
        return f"f{self._n}{random.getrandbits(24):06x}"

    def _reply(self, lst: _Listen) -> None:
        """Rule 1: a front listen's turn end gets exactly one front response, first in the queue."""
        if lst.replied:
            return
        lst.replied, self._waiting_for = True, None  # a new turn wins: no earlier answer is awaited any more
        s = _Speech(sid=self._new_id(), kind="reply", listen=lst.mid, t0=time.monotonic())
        self._queue.insert(0, s)
        asyncio.get_running_loop().create_task(self._next())

    def _enqueue(self, s: _Speech) -> None:
        s.t0 = time.monotonic()
        self._queue.append(s)
        asyncio.get_running_loop().create_task(self._next())

    async def _next(self) -> None:
        if self._active is not None or self._creating is not None or not self._queue or self._ws is None:
            return
        lst = self._listen
        if lst is not None and lst.front and lst.speech and not lst.turned:
            return  # the person is talking: the agent's answer waits for their turn's end (and the front's reply)
        s = self._queue.pop(0)
        self._creating = s
        meta = {"molty": s.sid}
        eid = {"event_id": f"rc_{s.sid}"}  # an error for this request names it (error.event_id)
        if s.kind == "inner":
            # the agent's answer it waited for, or the agent speaking up (per-response instructions: a bare system message gets reacted to)
            say = self._instructions() + "\n\n" + brief.inner(s.text, s.exact, s.awaited)
            await self._send({"type": "response.create", **eid, "response": {"instructions": say, "tool_choice": "none", "metadata": meta}})
        elif s.kind == "plain":
            await self._send({"type": "response.create", **eid, "response": {"instructions": brief.exactly(s.text), "tool_choice": "none", "metadata": meta}})
        elif s.kind == "wait":
            await self._send({"type": "response.create", **eid, "response": {"instructions": self._instructions() + "\n" + brief.STILL_WAITING, "tool_choice": "none", "metadata": meta}})
        else:  # the one reply to the person's turn
            await self._send({"type": "response.create", **eid, "response": {"max_output_tokens": brief.REPLY_MAX_TOKENS, "metadata": meta}})

    async def _on_created(self, resp: dict) -> None:
        rid = resp.get("id") if isinstance(resp.get("id"), str) else None
        meta = resp.get("metadata") if isinstance(resp.get("metadata"), dict) else {}
        s = self._creating
        if s is not None and (meta.get("molty") == s.sid or not meta):
            self._creating = None
        else:  # the box asks for every response itself: one it did not ask for never plays
            log.info("front: a response nobody asked for, cancelled")
            await self._send({"type": "response.cancel"})
            return
        s.rid = rid
        if s.kind == "reply":
            s.t0 = time.monotonic()  # its first_audio counts from here: ms.created already covers the turn's end to here
        self._active = s
        log.info("front %s (%s): response created", s.sid, s.kind)
        if s.kind == "reply":
            lst = self._listen if self._listen and self._listen.mid == s.listen else None
            ms = {"created": round((time.monotonic() - lst.t_stop) * 1000)} if lst and lst.t_stop else {}
            await self._send_json({"t": "front.reply", "id": s.sid, "stt": s.listen, **({"ms": ms} if ms else {})})

    async def _on_audio(self, rid: str | None, b64: str) -> None:
        s = self._active
        if s is None or (rid and s.rid and rid != s.rid) or s.cancelled:
            return
        try:
            pcm = base64.b64decode(b64)
        except ValueError:
            return
        y = s.rs.process(pcm16_to_float(pcm))
        if s.first is None:
            s.first = time.monotonic()
            lst = self._listen if s.listen and self._listen and self._listen.mid == s.listen else None
            log.info("front %s: first audio %.0f ms after %s", s.sid, (s.first - (lst.t_stop if lst and lst.t_stop else s.t0)) * 1000,
                     "the end of the turn" if lst and lst.t_stop else "the request")
        await self._audio_out(s, y)

    async def _audio_out(self, s: _Speech, y: np.ndarray) -> None:
        if y.size == 0:
            return
        for fr in framing.chunks(s.sid, float_to_pcm16(y), s.seq):
            await self._send_bin(fr)
            s.seq += 1
        s.samples += y.size

    async def _on_transcript(self, rid: str | None, delta: str, done: bool = False) -> None:
        s = self._active
        if s is None or (rid and s.rid and rid != s.rid):
            return
        if not done:
            s.said += delta
        elif delta and delta.split()[:s.words] == s.said.split()[:s.words]:
            s.said = delta  # the final transcript, unless it changed words already cued and shown
        await self._said(s, done)

    async def _said(self, s: _Speech, done: bool) -> None:
        """Word cues and the text so far, from the output transcript: the same whitespace-split words, word i cue i,
        and each text the one before it plus whole words (the device's say_text takes it so)."""
        words = s.said.split()
        complete = len(words) if done or s.said[-1:].isspace() else max(0, len(words) - 1)
        while s.words < complete:
            # the word's text arrives about when its audio does: cue it at the audio sent so far
            s.last_start = max(s.last_start, s.samples / DEVICE_RATE)
            await self._send_json({"t": "tts.word", "id": s.sid, "i": s.words, "text": words[s.words][:64], "start_s": round(s.last_start, 3)})
            s.words += 1
        # the text so far, once per word (the relay shows it on the device when the first sound goes out)
        text = " ".join(words[:complete])
        if text and (done and not s.text_final or not done and text != s.text_sent):
            s.text_sent, s.text_final = text, done
            await self._send_json({"t": "tts.text", "id": s.sid, "text": text[:4000], **({"final": True} if done else {})})

    async def _on_done(self, resp: dict) -> None:
        s = self._active
        rid = resp.get("id")
        if s is None or (rid and s.rid and rid != s.rid):
            return
        self._active = None
        self.usage.add(resp.get("usage") or {})
        for item in resp.get("output") or []:
            name = item.get("name") if isinstance(item, dict) and item.get("type") == "function_call" else None
            if name == brief.HAND_TOOL and s.kind == "reply":
                s.wait = True
                await self._send({"type": "conversation.item.create", "item": {"type": "function_call_output", "call_id": item.get("call_id"),
                                                                             "output": json.dumps({"ok": True, "note": "Your inner voice is on it: you will be told its answer."})}})
        status = resp.get("status")
        if s.kind == "reply" and s.wait and not (self._listen and self._listen.mid != s.listen and self._listen.speech):
            self._waiting_for = s.listen  # WAITING for this turn (unless the person has already spoken again)
        log.info("front %s (%s): response %s, said %r", s.sid, s.kind, status, s.said.strip()[:300])
        await self._said(s, done=True)
        await self._end_speech(s, error=None if s.samples or status == "cancelled" or s.wait else f"no audio ({status})")
        await self._next()

    async def _end_speech(self, s: _Speech, error: str | None) -> None:
        y = s.rs.flush()
        if y.size and not s.cancelled:
            await self._audio_out(s, y)
        if s.said.strip() and not s.text_final:
            await self._said(s, done=True)
        end: dict = {"t": "tts.end", "id": s.sid}
        if error:
            end["error"] = error[:200]
        if s.wait:
            end["wait"] = True
        if s.first is not None:
            end["ms"] = {"first_audio": round((s.first - s.t0) * 1000), "total": round((time.monotonic() - s.t0) * 1000)}
        await self._send_json(end)
        log.info("front %s (%s): %.1f s of speech%s%s", s.sid, s.kind, s.samples / DEVICE_RATE,
                 ", handed to the agent" if s.wait else "",
                 f", failed: {error}" if error else "")

    # ---------------- provider -> engine ----------------

    async def _read(self, ws) -> None:
        try:
            async for raw in ws:
                if isinstance(raw, bytes):
                    continue
                try:
                    ev = json.loads(raw)
                except ValueError:
                    continue
                if isinstance(ev, dict):
                    self.last_activity = time.monotonic()
                    await self._on_event(ev)
        except ConnectionClosed:
            pass
        except asyncio.CancelledError:
            raise
        except Exception as e:  # noqa: BLE001 - a broken event must close the session, never hang it
            log.warning("realtime: reader failed: %s: %s", type(e).__name__, e)
        if self._ws is ws:
            code = getattr(ws, "close_code", None)
            await self._close(f"provider_closed_{code}" if code else "provider_closed")

    async def _on_event(self, ev: dict) -> None:
        t = ev.get("type")
        lst = self._listen
        if t == "input_audio_buffer.speech_started":
            if lst and not lst.turned and not lst.ended and not lst.hold:
                log.info("listen %s: the model heard speech start", lst.mid)
                lst.speech = True
                if lst.front:
                    self._waiting_for = None  # the person speaks again: a new turn wins
                await self._send_json({"t": "stt.hint", "id": lst.mid, "k": "speech"})
        elif t == "input_audio_buffer.speech_stopped":
            if lst and not lst.turned and lst.speech and not lst.hold:
                lst.turned, lst.t_stop = True, time.monotonic()
                log.info("stt %s: end of turn (the model's turn detection)", lst.mid)
                await self._send_json({"t": "stt.turn_end", "id": lst.mid})
                if not lst.front:
                    await self._next()
        elif t == "input_audio_buffer.committed":
            if lst and isinstance(ev.get("item_id"), str) and not (lst.committed or lst.speech):
                # not a turn of this listen (nobody spoke in it): it never ends the listen; its item goes
                log.info("listen %s: a commit of no speech of this listen, dropped", lst.mid)
                await self._send({"type": "conversation.item.delete", "item_id": ev["item_id"]})
            elif lst and isinstance(ev.get("item_id"), str):
                log.info("listen %s: the input audio is committed", lst.mid)
                lst.committed = True
                lst.item = ev["item_id"]
                self._items[lst.item] = lst
                lst.t_stop = lst.t_stop or time.monotonic()
                if not lst.turned:  # committed without a speech_stopped first: the turn is over all the same
                    lst.turned = True
                    await self._send_json({"t": "stt.turn_end", "id": lst.mid})
                if lst.front and not lst.hold:
                    self._reply(lst)  # the model's turn end: its audio is committed, now its one reply
        elif t == "conversation.item.input_audio_transcription.delta":
            it = self._items.get(ev.get("item_id"))
            if it and not it.finaled and isinstance(ev.get("delta"), str):
                it.text += ev["delta"]
                await self._send_json({"t": "stt.partial", "id": it.mid, "text": it.text.strip()[:4000]})
        elif t == "conversation.item.input_audio_transcription.completed":
            it = self._items.pop(ev.get("item_id"), None)
            if it and not it.finaled:
                it.finaled = True
                text = ev.get("transcript") if isinstance(ev.get("transcript"), str) else it.text
                since = it.t_end or it.t_stop or time.monotonic()
                log.info("listen %s: transcript, %d chars", it.mid, len(text.strip()))
                await self._send_json({"t": "stt.final", "id": it.mid, "text": text.strip()[:4000], "ms": {"final": round((time.monotonic() - since) * 1000)}})
        elif t == "conversation.item.input_audio_transcription.failed":
            it = self._items.pop(ev.get("item_id"), None)
            if it and not it.finaled:
                it.finaled = True
                await self._send_json({"t": "stt.final", "id": it.mid, "error": "transcription failed"})
        elif t == "response.created":
            await self._on_created(ev.get("response") if isinstance(ev.get("response"), dict) else {})
        elif t in ("response.output_audio.delta", "response.audio.delta"):
            if isinstance(ev.get("delta"), str):
                await self._on_audio(ev.get("response_id"), ev["delta"])
        elif t in ("response.output_audio_transcript.delta", "response.audio_transcript.delta"):
            if isinstance(ev.get("delta"), str):
                await self._on_transcript(ev.get("response_id"), ev["delta"])
        elif t in ("response.output_audio_transcript.done", "response.audio_transcript.done"):
            if isinstance(ev.get("transcript"), str):
                await self._on_transcript(ev.get("response_id"), ev["transcript"], done=True)
        elif t == "response.done":
            await self._on_done(ev.get("response") if isinstance(ev.get("response"), dict) else {})
        elif t == "error":
            await self._on_error(ev.get("error") if isinstance(ev.get("error"), dict) else {})

    async def _on_error(self, err: dict) -> None:
        code, msg = str(err.get("code") or err.get("type") or "error"), str(err.get("message") or "")[:200]
        s = self._creating
        if s is not None and "active_response" in code:
            # another response (the turn detection's) got there first: ours goes after it
            self._creating = None
            self._queue.insert(0, s)
            return
        log.warning("realtime error %s: %s", code, msg)  # the provider's message: never our key
        # only an error that names our response request fails it (a turn's empty second commit never does)
        eid = err.get("event_id")
        if s is not None and (eid == f"rc_{s.sid}" or (not eid and not code.startswith("input_audio_buffer"))):
            self._creating = None
            await self._end_speech(s, error=f"realtime error {code}")
            await self._next()


def _why(e: BaseException) -> str:
    status = getattr(getattr(e, "response", None), "status_code", None)
    return f"HTTP {status}" if status else type(e).__name__
