"""The simulated Molty: the firmware's side of docs/AGENT-LINK.md, timed like the firmware.

Mirrors firmware/main/link/ — hello (id, key, fw, the 1.46's caps, board, link_code), welcome,
ping every 20 s, pair.request auto-accepted ("the human taps") with link-code rotation, cmd/res,
and audio:

- listen (link_audio.c record()): the mic is read in 100 ms chunks in real time, gathered
  into 300 ms frames that leave as soon as they are full; the VAD (vad.py, a port of
  audio/vad.c) runs on every chunk unless the listen says vad:false; `listen_stop` is checked
  once per chunk and ends the listen with the relay's reason, sending what is buffered as the
  end frame. The end frame carries reason + speech_ms (only when known), like the firmware.
  The turn acknowledgement (link_ack_decide) is decided at the end, as on the device.
- say: frames are "played" on a model of the speaker: a frame starts when it arrives or when
  the previous one ends (a gap = an underrun), the chirp of the previous listen delays the
  start by up to ~110 ms (it is cut), say.done goes when the last sample has left the DMA.
  Every frame's arrival, word cue and play time is recorded for the metrics.
"""

from __future__ import annotations

import asyncio
import base64
import json
import logging
import secrets
import time
from collections.abc import Callable
from dataclasses import dataclass, field

import numpy as np
from websockets.asyncio.client import connect as ws_connect
from websockets.exceptions import ConnectionClosed

from . import vad as vadm
from .voice import Mic

log = logging.getLogger("molty_sim.device")

RATE = 16000
CHUNK = 1600  # 100 ms (REC_CHUNK_MS)
FRAME = 3 * CHUNK  # 300 ms per uplink frame (REC_FRAME_SAMPLES)
LISTEN_MAX_MS = 30000
LISTEN_CEILING_MS = 300000
CHIRP_S = 0.160
CHIRP_CUT_S = 0.110  # a say cuts the chirp: fade + DMA flush (AGENT-LINK "Turn acknowledgement")
DMA_TAIL_S = 0.040  # the I2S DMA ring after the last sample was written (say.done waits for it)
CUE_LEAD_S = 0.030  # the device reveals a word 30 ms before its cue (CUE_LEAD_SAMPLES)
ACK_MIN_SPEECH_MS = 160
LINK_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
ID_ALPHABET = "abcdefghijklmnopqrstuvwxyz23456789"
SIM_PREFIX = "molty-sim"

# The Waveshare 1.46's hello (firmware/main/link/link_identity.c: speaker + mic + touch).
CAPS_146 = ["fluid", "say", "say_cues", "listen", "listen_stop", "listen_window", "ack", "show", "mood", "creature", "ask", "update"]
BOARD_146 = "waveshare-esp32-s3-touch-lcd-1.46"
ACK_NAMES = ("off", "thinking", "chirp")


def new_device_id() -> str:
    """A sim id: `molty-sim` + 7 random chars — a valid device id that never collides with a real one's shape by intent."""
    return SIM_PREFIX + "".join(secrets.choice(ID_ALPHABET) for _ in range(7))


def new_link_code() -> str:
    return "".join(secrets.choice("ABCDEFGHIJKLMNOPQRSTUVWXYZ23456789") for _ in range(8))


def ack_arg(args: dict, setting: str) -> str | None:
    """link_ack_arg: absent/true = the setting, false = off, "silent" = thinking (off stays off)."""
    a = args.get("ack", True)
    if a is True:
        return setting
    if a is False:
        return "off"
    if a == "silent":
        return "off" if setting == "off" else "thinking"
    return None


def ack_decide(mode: str, reason: str | None, speech_known: bool, speech_ms: int, screen_taken: bool = False, has_speaker: bool = True) -> str:
    """link_ack_decide (firmware/main/link/link_protocol.c)."""
    if mode == "off" or screen_taken or not reason:
        return "off"
    heard = (reason == "turn_end" or (reason == "silence" and (not speech_known or speech_ms > 0))
             or (reason == "max" and speech_known and speech_ms >= ACK_MIN_SPEECH_MS))
    if not heard:
        return "off"
    return "thinking" if mode == "chirp" and not has_speaker else mode


def encode_frame(header: dict, payload: bytes = b"") -> bytes:
    return json.dumps(header, separators=(",", ":")).encode() + b"\n" + payload


def decode_frame(data: bytes) -> tuple[dict, bytes]:
    nl = data.find(b"\n", 0, 513)
    head, payload = (data, b"") if nl < 0 else (data[:nl], data[nl + 1 :])
    return json.loads(head), payload


@dataclass
class ListenRec:
    job: str
    args: dict
    cmd_at: float
    window_ms: int = 0
    vad: bool = True
    ack_mode: str = "chirp"
    mic_open_at: float = 0.0
    frames: list[tuple[float, int, int]] = field(default_factory=list)  # (sent_at, seq, samples)
    stop_at: float | None = None  # listen_stop received (wall)
    stop_reason: str | None = None
    stop_mic_s: float | None = None  # mic time when the stop arrived
    end_at: float | None = None  # end frame sent (wall)
    end_mic_s: float | None = None  # mic time captured when it ended (samples sent / 16k)
    reason: str | None = None
    speech_ms: int | None = None
    ack: str = "off"
    ack_end_at: float | None = None  # when the chirp would finish
    error: str | None = None
    done: asyncio.Event = field(default_factory=asyncio.Event)


@dataclass
class SayRec:
    job: str
    text: str
    cmd_at: float
    cues_on: bool = False
    ms: int = 0
    frames: list[dict] = field(default_factory=list)  # {at, seq, n, offset, play_at, cues}
    cues: list[tuple[int, int, float]] = field(default_factory=list)  # (word, sample, arrival)
    pcm: list[bytes] = field(default_factory=list)
    samples: int = 0
    first_at: float | None = None
    play_start: float | None = None
    cursor: float | None = None  # wall time the speaker will have played everything received
    underrun_s: float = 0.0
    underruns: int = 0
    end_at: float | None = None
    done_at: float | None = None
    chirp_delay_s: float = 0.0
    bad: list[str] = field(default_factory=list)
    done: asyncio.Event = field(default_factory=asyncio.Event)

    def play_time(self, sample: int) -> float | None:
        """Wall time the speaker plays `sample` (None before that audio arrived)."""
        for f in self.frames:
            if f["n"] and f["offset"] <= sample < f["offset"] + f["n"]:
                return f["play_at"] + (sample - f["offset"]) / RATE
        if self.frames and sample >= self.samples and self.cursor is not None:
            return self.cursor
        return None

    def audio(self) -> np.ndarray:
        return np.frombuffer(b"".join(self.pcm), dtype="<i2")


class SimDevice:
    def __init__(self, relay: str, device_id: str, key: bytes | None = None, *, fw: str = "0.1.0",
                 caps: list[str] | None = None, board: str = BOARD_146, listen_setting_ms: int = 30000,
                 ack_setting: str = "chirp", tap_delay_s: float = 0.3, accept: bool = True):
        if not device_id.startswith(SIM_PREFIX):
            raise ValueError(f"a sim device id starts with {SIM_PREFIX!r} (it must never impersonate a real Molty)")
        self.relay = relay.rstrip("/")
        self.id = device_id
        self.key = key or secrets.token_bytes(32)
        self.fw, self.caps, self.board = fw, list(caps or CAPS_146), board
        self.listen_setting_ms, self.ack_setting = listen_setting_ms, ack_setting
        self.tap_delay_s, self.accept = tap_delay_s, accept
        self.link_code = new_link_code()
        self.welcomed = asyncio.Event()
        self.closed: tuple[int, str] | None = None
        self.mic_for: Callable[[str], Mic] = lambda job: Mic(None)
        self.listens: dict[str, ListenRec] = {}
        self.says: dict[str, SayRec] = {}
        self.events: list[dict] = []  # notable things, for the run log
        self.pairs: list[dict] = []
        self.mood = "idle"
        self._ws = None
        self._busy: str | None = None  # the running audio job
        self._stop: dict[str, str] = {}
        self._chirp_until = 0.0
        self._send_lock = asyncio.Lock()
        self._tasks: set[asyncio.Task] = set()

    # ---------------- link ----------------

    def note(self, event: str, **data) -> None:
        self.events.append({"event": event, "at": time.monotonic(), **data})

    async def run(self) -> None:
        """Connects and serves until close (no reconnect: a sim run is one session)."""
        url = f"{self.relay}/v1/device?id={self.id}"
        async with ws_connect(url, max_size=1 << 20, ping_interval=None) as ws:
            self._ws = ws
            await self._send_json({"t": "hello", "id": self.id, "key": base64.b64encode(self.key).decode(), "fw": self.fw,
                                   "caps": self.caps, "board": self.board, "link_code": self.link_code})
            pinger = asyncio.create_task(self._pinger())
            try:
                async for msg in ws:
                    if isinstance(msg, bytes):
                        self._on_audio(msg)
                    else:
                        await self._on_text(json.loads(msg))
            except ConnectionClosed:
                pass
            finally:
                pinger.cancel()
                self.closed = (ws.close_code or 0, ws.close_reason or "")
                self.note("close", code=self.closed[0], reason=self.closed[1])

    async def _pinger(self) -> None:
        while True:
            await asyncio.sleep(20)
            await self._send_json({"t": "ping"})

    async def _send_json(self, obj) -> None:
        async with self._send_lock:
            await self._ws.send(json.dumps(obj, separators=(",", ":")))

    async def _send_bin(self, data: bytes) -> None:
        async with self._send_lock:
            await self._ws.send(data)

    async def close(self) -> None:
        if self._ws is not None:
            await self._ws.close()

    def _spawn(self, coro) -> None:
        t = asyncio.create_task(coro)
        self._tasks.add(t)
        t.add_done_callback(self._tasks.discard)

    async def _on_text(self, f: dict) -> None:
        t = f.get("t")
        if t == "welcome":
            self.welcomed.set()
            self.note("welcome")
        elif t == "pair.request":
            self._spawn(self._pair(f))
        elif t == "cmd":
            await self._on_cmd(f)
        elif t in ("pong", "forgotten"):
            pass

    async def _pair(self, f: dict) -> None:
        self.pairs.append({"agent_name": f.get("agent_name"), "code": f.get("code")})
        self.note("pair.request", agent_name=f.get("agent_name"))
        await asyncio.sleep(self.tap_delay_s)  # the human reads the code and taps
        if self.accept:
            self.link_code = new_link_code()
            await self._send_json({"t": "pair.result", "req": f.get("req"), "ok": True, "link_code": self.link_code})
        else:
            await self._send_json({"t": "pair.result", "req": f.get("req"), "ok": False, "reason": "rejected"})

    # ---------------- commands ----------------

    async def _reply(self, f: dict, ok: bool, result: dict | None = None, error: str | None = None) -> None:
        await self._send_json({"t": "res", "id": f.get("id"), "ok": True, "result": result or {}} if ok
                              else {"t": "res", "id": f.get("id"), "ok": False, "error": error})

    async def _on_cmd(self, f: dict) -> None:
        name, a = f.get("name"), f.get("args") or {}
        if name == "status":
            return await self._reply(f, True, {"fw": self.fw, "uptime": 1, "creature": "fluid", "state": "creature", "mood": self.mood, "fps": 30, "rssi": -50})
        if name == "mood":
            self.mood = str(a.get("mood", self.mood))
            return await self._reply(f, True, {"mood": self.mood})
        if name in ("show", "state", "volume", "creature"):
            return await self._reply(f, True, {})
        if name == "say":
            return await self._cmd_say(f, a)
        if name == "listen":
            return await self._cmd_listen(f, a)
        if name == "listen_stop":
            return await self._cmd_listen_stop(f, a)
        if name == "ask":  # the human never taps an ask in the sim: it times out on the device
            return await self._reply(f, True, {"job": a.get("job")})
        await self._reply(f, False, error="unknown_command")

    async def _cmd_say(self, f: dict, a: dict) -> None:
        job, text = a.get("job"), a.get("text")
        if not isinstance(job, str) or not isinstance(text, str) or not text or not isinstance(a.get("ms"), (int, float)):
            return await self._reply(f, False, error="invalid_args")
        if self._busy:
            return await self._reply(f, False, error="busy")
        self._busy = job
        self.says[job] = SayRec(job=job, text=text, cmd_at=time.monotonic(), cues_on=a.get("cues") is True, ms=int(a["ms"]))
        self.mood = "speaking"
        await self._reply(f, True, {})

    async def _cmd_listen(self, f: dict, a: dict) -> None:
        job = a.get("job")
        ms = a.get("ms", None)
        if not isinstance(job, str):
            return await self._reply(f, False, error="invalid_args")
        if ms is None:
            window = self.listen_setting_ms
        elif isinstance(ms, bool) or not isinstance(ms, (int, float)) or not 0 <= ms <= LISTEN_MAX_MS or ms != int(ms):
            return await self._reply(f, False, error="invalid_args")
        else:
            window = int(ms)
        vad_on = a.get("vad", True)
        es, ns = a.get("end_silence_ms", 700), a.get("no_speech_ms", 4000)
        if not isinstance(vad_on, bool) or not (isinstance(es, int) and 300 <= es <= 3000) or not (isinstance(ns, int) and 1000 <= ns <= 10000):
            return await self._reply(f, False, error="invalid_args")
        ack = ack_arg(a, self.ack_setting)
        if ack is None:
            return await self._reply(f, False, error="invalid_args")
        if self._busy:
            return await self._reply(f, False, error="busy")
        self._busy = job
        rec = ListenRec(job=job, args=dict(a), cmd_at=time.monotonic(), window_ms=window, vad=vad_on, ack_mode=ack)
        self.listens[job] = rec
        self.mood = "listening"
        await self._reply(f, True, {"job": job, "ms": window, "ack": ack})
        cfg = vadm.VadCfg(enabled=vad_on, max_ms=window or LISTEN_CEILING_MS, end_silence_ms=es, no_speech_ms=ns)
        self._spawn(self._record(rec, cfg))

    async def _cmd_listen_stop(self, f: dict, a: dict) -> None:
        job, reason = a.get("job"), a.get("reason", "turn_end")
        if not isinstance(job, str) or reason not in ("turn_end", "no_speech"):
            return await self._reply(f, False, error="invalid_args")
        rec = self.listens.get(job)
        mine = self._busy == job and rec is not None and rec.end_at is None
        if mine and job not in self._stop:
            self._stop[job] = reason
            rec.stop_at, rec.stop_reason = time.monotonic(), reason
            rec.stop_mic_s = rec.stop_at - rec.mic_open_at
        await self._reply(f, True, {"job": job, "stopping": mine})

    # ---------------- listen: the mic ----------------

    async def _record(self, rec: ListenRec, cfg: vadm.VadCfg) -> None:
        mic = self.mic_for(rec.job)
        v = vadm.Vad(cfg)
        total = 16 * (rec.window_ms or LISTEN_CEILING_MS)
        loop = asyncio.get_running_loop()
        rec.mic_open_at = time.monotonic()
        t0 = loop.time()
        acc: list[np.ndarray] = []
        have = done = seq = k = 0
        verdict = vadm.CONTINUE
        reason = None
        try:
            while verdict == vadm.CONTINUE:
                stop = self._stop.get(rec.job)
                if stop:
                    reason = stop
                    await self._send_up(rec, seq, acc, True, reason, v)
                    break
                k += 1
                wait = t0 + k * CHUNK / RATE - loop.time()  # the chunk is captured when its 100 ms are over
                if wait > 0:
                    await asyncio.sleep(wait)
                chunk = mic.read(CHUNK)
                got = min(CHUNK, total - done, FRAME - have)
                acc.append(chunk[:got])
                have += got
                done += got
                verdict = v.feed(chunk[:got])
                if verdict == vadm.CONTINUE and done >= total:
                    verdict = vadm.END_MAX
                if have >= FRAME or verdict != vadm.CONTINUE:
                    end = verdict != vadm.CONTINUE
                    reason = vadm.REASONS.get(verdict) if end else None
                    await self._send_up(rec, seq, acc, end, reason, v)
                    seq += 1
                    acc, have = [], 0
        except ConnectionClosed:
            rec.error = "link_closed"
            reason = "error"
        rec.end_at = time.monotonic()
        rec.end_mic_s = done / RATE
        rec.reason = reason
        rec.speech_ms = v.speech_ms if v.speech_known() else None
        rec.ack = ack_decide(rec.ack_mode, reason, v.speech_known(), v.speech_ms)
        if rec.ack == "chirp":
            self._chirp_until = rec.end_at + CHIRP_S
            rec.ack_end_at = self._chirp_until
        self.mood = "thinking" if rec.ack != "off" else "idle"
        self._stop.pop(rec.job, None)
        if self._busy == rec.job:
            self._busy = None
        rec.done.set()

    async def _send_up(self, rec: ListenRec, seq: int, acc: list[np.ndarray], end: bool, reason: str | None, v: vadm.Vad) -> None:
        pcm = np.concatenate(acc).astype("<i2").tobytes() if acc else b""
        h = {"s": rec.job, "seq": seq, "kind": "pcm16", "rate": RATE, "end": end}
        if end and reason:
            h["reason"] = reason
            if v.speech_known():
                h["speech_ms"] = v.speech_ms
        await self._send_bin(encode_frame(h, pcm))
        rec.frames.append((time.monotonic(), seq, len(pcm) // 2))

    # ---------------- say: the speaker ----------------

    def _on_audio(self, data: bytes) -> None:
        now = time.monotonic()
        try:
            h, payload = decode_frame(data)
        except ValueError:
            return self.note("audio.bad_frame")
        rec = self.says.get(h.get("s"))
        if rec is None:
            return self.note("audio.unexpected", s=h.get("s"))
        if h.get("kind") != "pcm16" or h.get("rate") != RATE or len(payload) > 16384:
            rec.bad.append("bad_frame")
        if h.get("seq") != len(rec.frames):
            rec.bad.append(f"seq {h.get('seq')} != {len(rec.frames)}")
        n = len(payload) // 2
        w = h.get("w") or []
        for c in w:
            if isinstance(c, list) and len(c) == 2:
                rec.cues.append((int(c[0]), int(c[1]), now))
        play_at = None
        if n:
            if rec.first_at is None:
                rec.first_at = now
                delay = min(max(0.0, self._chirp_until - now), CHIRP_CUT_S)  # the chirp is cut first
                rec.chirp_delay_s = delay
                rec.play_start = rec.cursor = now + delay
            if now > rec.cursor:  # the speaker ran dry before this frame came
                rec.underrun_s += now - rec.cursor
                rec.underruns += 1
                rec.cursor = now
            play_at = rec.cursor
            rec.cursor += n / RATE
        rec.frames.append({"at": now, "seq": h.get("seq"), "n": n, "offset": rec.samples, "play_at": play_at, "cues": len(w)})
        rec.pcm.append(payload[: n * 2])
        rec.samples += n
        if h.get("end"):
            rec.end_at = now
            self._spawn(self._say_done(rec))

    async def _say_done(self, rec: SayRec) -> None:
        left = (rec.cursor or time.monotonic()) + DMA_TAIL_S - time.monotonic()
        if left > 0:
            await asyncio.sleep(left)
        rec.done_at = time.monotonic()
        if self._busy == rec.job:
            self._busy = None
        self.mood = "idle"
        if rec.samples:
            await self._send_json({"t": "evt", "name": "say.done", "data": {"job": rec.job}})
        rec.done.set()
