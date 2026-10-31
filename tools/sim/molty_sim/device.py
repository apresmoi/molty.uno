"""The simulated Molty: the firmware's side of docs/AGENT-LINK.md, timed like the firmware.

Mirrors device/firmware/main/link/ — hello (id, key, fw, the 1.46's caps, board, link_code), welcome,
ping every 20 s, pair.request auto-accepted ("the human taps") with link-code rotation, cmd/res,
and audio:

- listen (link_audio.c record()): the mic is read in 100 ms chunks in real time, gathered
  into 300 ms frames that leave as soon as they are full; the VAD (vad.py, a port of
  audio/vad.c) runs on every chunk unless the listen says vad:false; `listen_stop` is checked
  once per chunk and ends the listen with the relay's reason, sending what is buffered as the
  end frame. The end frame carries reason + speech_ms (only when known), like the firmware.
  The turn acknowledgement (link_ack_decide) is decided at the end, as on the device.
- the listen uplink (docs/AGENT-LINK.md "Turn trace", molty_sim/uplink.py): every frame carries
  the trace fields (ct, lv, cl, rt, wq; the end frame `tr`); frames wait in a resend ring and a
  send that fails (a scenario `stall:` window) is retried in order — the mic keeps being read.
- shake to talk: `shake()` starts the device's own listen (`shake-<n>`), sends `evt shake`
  naming it, keeps the audio until the relay adopts it (`cmd listen {adopt}`) or declines it
  (`listen_stop`), 3 s at most; the audio before the IMU shows the shake settled is trimmed.
- hints: `{"t":"hint"}` frames are recorded (the firmware's listener reactions are visual only).
- mood: an explicit mood counts (the listen's ack is skipped: link_ack_decide screen_taken);
  `react:true` shows it without counting (cap `react`). Every mood is recorded with its time.
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
from .uplink import Settle, UpFrame, UplinkRing, clipped, in_windows, level_byte, settle_trim
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

# The Waveshare 1.46's hello (device/firmware/main/link/link_identity.c: speaker + mic + touch).
CAPS_146 = ["fluid", "say", "say_cues", "listen", "listen_stop", "listen_window", "ack", "shake_talk", "trace", "hints",
            "show", "mood", "act", "creature", "react", "ask", "update"]
ADOPT_WINDOW_MS = 3000  # LINK_SHAKE_ADOPT_MS: a shake's listen nobody adopted by then ends quietly
RETRY_S = 0.1  # a failed uplink send is retried this often
FLUSH_S = 15.0  # the end frame waits at most this long for the ring to drain
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
    """link_ack_decide (device/firmware/main/link/link_protocol.c)."""
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
    # shake to talk: the device's own id, and the relay's adoption (its listen args) / decline
    local: str | None = None
    adopt: dict | None = None
    declined: bool = False
    trim_ms: int | None = None  # the shake's handling noise cut from the start
    trim_capped: bool = False
    up: list[dict] = field(default_factory=list)  # every header sent (trace fields included)
    hints: list[str] = field(default_factory=list)  # relay hints that named this listen
    mood_gen0: int = 0  # explicit moods counted when it began (an explicit one meanwhile: no ack)


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


@dataclass
class _Rec:
    """What record() carries from one chunk to the next (link_audio.c rec_t)."""

    v: vadm.Vad
    total: int
    pos_ms: float = 0.0  # mic ms of the next sample taken
    pos_ms_start: float = 0.0
    acc: list = field(default_factory=list)
    have: int = 0
    done: int = 0
    seq: int = 0
    frame_ct: float = 0.0
    lv: list = field(default_factory=list)
    cl: int = 0
    peak: int = 0


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
        self.features: list[str] = []
        self.stall_for: Callable[[str], list[tuple[float, float]]] = lambda job: []  # uplink stalls (mic s)
        self.imu_for: Callable[[str], list | None] = lambda job: None  # a shake's IMU trace
        self.mic_gain, self.rssi = 6, -55
        self.hints: list[dict] = []
        self._shake_n = 0
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
        self.moods: list[tuple[float, str, bool]] = []  # (wall, mood, react); acts as (wall, "act:<name>", True)
        self._mood_gen = 0

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
            self.features = list(f.get("features") or [])
            self.welcomed.set()
            self.note("welcome")
        elif t == "pair.request":
            self._spawn(self._pair(f))
        elif t == "cmd":
            await self._on_cmd(f)
        elif t == "hint":
            self.hints.append({"job": f.get("job"), "k": f.get("k"), "at": time.monotonic()})
            rec = self.listens.get(f.get("job")) if isinstance(f.get("job"), str) else None
            if rec is not None and self._busy == rec.job and rec.end_at is None and f.get("k") in ("pause", "question"):
                rec.hints.append(f["k"])
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
        if name == "act":  # a gesture over whatever shows (never an explicit mood, never a sound)
            if not isinstance(a.get("action"), str):
                return await self._reply(f, False, error="invalid_args")
            self.moods.append((time.monotonic(), f"act:{a['action']}", True))
            return await self._reply(f, True, {"action": a["action"]})
        if name == "mood":
            react = a.get("react", False)
            if not isinstance(react, bool):
                return await self._reply(f, False, error="invalid_args")
            self.mood = str(a.get("mood", self.mood))
            self.moods.append((time.monotonic(), self.mood, react))
            if not react:
                self._mood_gen += 1  # explicit: the running listen's ack is skipped
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

    def _listen_args(self, a: dict) -> tuple[int, bool, int, int, str] | None:
        """The listen's window / VAD / ack args, checked like the firmware (None: invalid_args)."""
        ms = a.get("ms", None)
        if ms is None:
            window = self.listen_setting_ms
        elif isinstance(ms, bool) or not isinstance(ms, (int, float)) or not 0 <= ms <= LISTEN_MAX_MS or ms != int(ms):
            return None
        else:
            window = int(ms)
        vad_on = a.get("vad", True)
        es, ns = a.get("end_silence_ms", 700), a.get("no_speech_ms", 4000)
        if not isinstance(vad_on, bool) or not (isinstance(es, int) and 300 <= es <= 3000) or not (isinstance(ns, int) and 1000 <= ns <= 10000):
            return None
        ack = ack_arg(a, self.ack_setting)
        if ack is None:
            return None
        return window, vad_on, es, ns, ack

    async def _cmd_listen(self, f: dict, a: dict) -> None:
        job = a.get("job")
        if not isinstance(job, str):
            return await self._reply(f, False, error="invalid_args")
        if "adopt" in a:
            return await self._cmd_adopt(f, a, job)
        args = self._listen_args(a)
        if args is None:
            return await self._reply(f, False, error="invalid_args")
        window, vad_on, es, ns, ack = args
        if self._busy:
            return await self._reply(f, False, error="busy")
        self._busy = job
        rec = ListenRec(job=job, args=dict(a), cmd_at=time.monotonic(), window_ms=window, vad=vad_on, ack_mode=ack, mood_gen0=self._mood_gen)
        self.listens[job] = rec
        self.mood = "listening"
        await self._reply(f, True, {"job": job, "ms": window, "ack": ack})
        cfg = vadm.VadCfg(enabled=vad_on, max_ms=window or LISTEN_CEILING_MS, end_silence_ms=es, no_speech_ms=ns)
        self._spawn(self._record(rec, cfg))

    async def _cmd_adopt(self, f: dict, a: dict, job: str) -> None:
        """cmd listen {job, adopt: <local id>, …}: the relay takes the shake's listen as `job`."""
        local = a.get("adopt")
        args = self._listen_args(a)
        if not isinstance(local, str) or not local or args is None:
            return await self._reply(f, False, error="invalid_args")
        rec = self.listens.get(local)
        if rec is None or rec.local != local or self._busy != local or rec.adopt is not None or rec.declined or rec.end_at is not None:
            return await self._reply(f, False, error="listen_gone")
        window, vad_on, es, ns, ack = args
        rec.adopt = {"job": job, "cfg": vadm.VadCfg(enabled=vad_on, max_ms=window or LISTEN_CEILING_MS, end_silence_ms=es, no_speech_ms=ns)}
        rec.job, rec.args, rec.window_ms, rec.vad, rec.ack_mode = job, dict(a), window, vad_on, ack
        self.listens[job] = rec
        self._busy = job
        await self._reply(f, True, {"job": job, "ms": window, "ack": ack})

    async def shake(self, peak_g: float = 8.2, reversals: int = 4, ms: int = 420) -> str | None:
        """The human shakes Molty: with shake to talk (the relay adopts), a listen of its own at once
        and `evt shake` naming it; otherwise the plain shake. Returns the local listen id (or None)."""
        data = {"peak_g": round(peak_g, 2), "reversals": reversals, "ms": ms}
        if self._busy or "listen_adopt" not in self.features:
            await self._send_json({"t": "evt", "name": "shake", "data": data})
            return None
        self._shake_n += 1
        local = f"shake-{self._shake_n}"
        window = self.listen_setting_ms
        rec = ListenRec(job=local, args={}, cmd_at=time.monotonic(), window_ms=window, vad=True, ack_mode=self.ack_setting, local=local, mood_gen0=self._mood_gen)
        self.listens[local] = rec
        self._busy = local
        self.mood = "listening"
        self._spawn(self._record(rec, vadm.VadCfg(max_ms=window or LISTEN_CEILING_MS)))
        await self._send_json({"t": "evt", "name": "shake", "data": {**data, "listen": local}})
        return local

    async def _cmd_listen_stop(self, f: dict, a: dict) -> None:
        job, reason = a.get("job"), a.get("reason", "turn_end")
        if not isinstance(job, str) or reason not in ("turn_end", "no_speech"):
            return await self._reply(f, False, error="invalid_args")
        rec = self.listens.get(job)
        mine = self._busy == job and rec is not None and rec.end_at is None
        if mine and rec.local == job and rec.adopt is None:
            rec.declined = True  # a shake's listen the relay will not adopt: it ends quietly
        elif mine and job not in self._stop:
            self._stop[job] = reason
            rec.stop_at, rec.stop_reason = time.monotonic(), reason
            rec.stop_mic_s = rec.stop_at - rec.mic_open_at
        await self._reply(f, True, {"job": job, "stopping": mine})

    # ---------------- listen: the mic ----------------

    async def _record(self, rec: ListenRec, cfg: vadm.VadCfg) -> None:
        """link_audio.c record(): 100 ms chunks in real time; a shake's listen trims its handling
        noise, keeps its audio until adopted; frames go to the uplink ring, the sender drains it."""
        mic = self.mic_for(rec.job)
        imu = list(self.imu_for(rec.job) or []) if rec.local else []
        settle = Settle() if rec.local and imu else None
        stalls = self.stall_for(rec.job)
        loop = asyncio.get_running_loop()
        rec.mic_open_at = time.monotonic()
        t0 = loop.time()
        ring = UplinkRing()
        wake = asyncio.Event()
        state = {"capturing": True}
        sender = asyncio.create_task(self._sender(rec, ring, stalls, t0, wake, state))
        st = _Rec(vadm.Vad(cfg), 16 * (rec.window_ms or LISTEN_CEILING_MS))
        pre: list[np.ndarray] = []  # a shake's audio until it is adopted
        held: list[np.ndarray] = []  # ...and before it settled
        k = 0
        reason = None
        unclaimed = False
        pending = rec.local is not None

        def push(end: bool, why: str | None) -> None:
            pcm = np.concatenate(st.acc).astype("<i2") if st.acc else np.zeros(0, "<i2")
            h = {"s": rec.job, "seq": st.seq, "kind": "pcm16", "rate": RATE, "end": end, "ct": round(st.frame_ct),
                 "lv": "".join(f"{b:02x}" for b in st.lv)}
            if st.cl:
                h["cl"] = st.cl
            if end and why:
                h["reason"] = why
                if st.v.speech_known():
                    h["speech_ms"] = st.v.speech_ms
            if end:
                tr = {"wall": round((loop.time() - t0) * 1000), "cap": round(st.done / 16), "r0": self.rssi, "r1": self.rssi,
                      "gain": self.mic_gain, "pk": round(st.peak * 1000 / 32767)}
                if rec.local:
                    tr["set"] = rec.trim_ms or 0
                    if rec.trim_capped:
                        tr["setc"] = 1
                h["tr"] = tr
            for gone in ring.push(UpFrame(st.seq, h, pcm.tobytes(), loop.time(), len(pcm))):
                self.note("uplink.drop", job=rec.job, seq=gone.seq)
            wake.set()
            st.seq += 1
            st.acc, st.have, st.lv, st.cl = [], 0, [], 0
            st.frame_ct = st.pos_ms

        def take(piece: np.ndarray) -> int:
            """One chunk into the frame being built and the VAD; the frame goes when full or ended."""
            got = min(len(piece), st.total - st.done, FRAME - st.have)
            piece = piece[:got]
            if st.have == 0:
                st.frame_ct = st.pos_ms
            st.acc.append(piece)
            st.have += got
            st.done += got
            st.pos_ms += got / 16
            st.lv.append(level_byte(piece))
            st.cl += clipped(piece)
            st.peak = max(st.peak, int(np.max(np.abs(piece.astype(np.int32)))) if got else 0)
            verdict = st.v.feed(piece)
            if verdict == vadm.CONTINUE and st.done >= st.total:
                verdict = vadm.END_MAX
            if st.have >= FRAME or verdict != vadm.CONTINUE:
                push(verdict != vadm.CONTINUE, vadm.REASONS.get(verdict))
            return verdict

        verdict = vadm.CONTINUE
        try:
            while verdict == vadm.CONTINUE:
                if pending:
                    if rec.declined or sum(len(p) for p in pre) >= 16 * ADOPT_WINDOW_MS:
                        unclaimed, reason = True, "declined" if rec.declined else "unclaimed"
                        break
                    if rec.adopt is not None:  # the relay's listen from here: the audio so far replays through it
                        pending = False
                        st = _Rec(vadm.Vad(rec.adopt["cfg"]), 16 * (rec.window_ms or LISTEN_CEILING_MS), pos_ms=st.pos_ms_start)
                        for p in pre:
                            verdict = take(p)
                            if verdict != vadm.CONTINUE:
                                break
                        pre = []
                        if verdict != vadm.CONTINUE:
                            reason = vadm.REASONS.get(verdict)
                            break
                        continue
                else:
                    stop = self._stop.get(rec.job)
                    if stop:
                        reason = stop
                        push(True, reason)
                        break
                k += 1
                wait = t0 + k * CHUNK / RATE - loop.time()  # the chunk is captured when its 100 ms are over
                if wait > 0:
                    await asyncio.sleep(wait)
                chunk = mic.read(CHUNK)
                pieces = [chunk]
                if settle is not None and not settle.done:
                    held.append(chunk)
                    now_ms = k * CHUNK * 1000 // RATE
                    while imu and imu[0][3] <= now_ms and not settle.done:
                        ax, ay, az, ms = imu.pop(0)
                        settle.feed(ax, ay, az, ms)
                    settle.tick(now_ms)
                    if not settle.done:
                        continue
                    rec.trim_ms, rec.trim_capped = settle.at_ms, settle.capped
                    kept = settle_trim(np.concatenate(held), settle.at_ms)
                    held = []
                    st.pos_ms = st.pos_ms_start = settle.at_ms
                    pieces = [kept[i : i + CHUNK] for i in range(0, len(kept), CHUNK)]
                for piece in pieces:
                    if pending:
                        pre.append(piece)
                        continue
                    verdict = take(piece)
                    if verdict != vadm.CONTINUE:
                        reason = vadm.REASONS.get(verdict)
                        break
        except ConnectionClosed:
            rec.error = "link_closed"
            reason = "error"
        state["capturing"] = False
        wake.set()
        try:
            await asyncio.wait_for(sender, FLUSH_S)
        except (TimeoutError, asyncio.TimeoutError):
            sender.cancel()
            rec.error = rec.error or "flush_timeout"
        rec.end_at = time.monotonic()
        rec.end_mic_s = st.done / RATE + (rec.trim_ms or 0) / 1000
        rec.reason = reason
        v = st.v
        rec.speech_ms = v.speech_ms if v.speech_known() else None
        rec.ack = "off" if unclaimed else ack_decide(rec.ack_mode, reason, v.speech_known(), v.speech_ms, screen_taken=self._mood_gen != rec.mood_gen0)
        if rec.ack == "chirp":
            self._chirp_until = rec.end_at + CHIRP_S
            rec.ack_end_at = self._chirp_until
        self.mood = "thinking" if rec.ack != "off" else "idle"
        self._stop.pop(rec.job, None)
        if self._busy == rec.job:
            self._busy = None
        rec.done.set()

    async def _sender(self, rec: ListenRec, ring: UplinkRing, stalls: list[tuple[float, float]], t0: float, wake: asyncio.Event, state: dict) -> None:
        """The uplink: oldest frame first; a send inside a stall window fails and is retried in order."""
        loop = asyncio.get_running_loop()
        while True:
            f = ring.head()
            if f is None:
                if not state["capturing"]:
                    return
                wake.clear()
                await wake.wait()
                continue
            if in_windows(loop.time() - t0, stalls):
                ring.failed()
                self.note("uplink.send_fail", job=rec.job, seq=f.seq, rt=f.rt)
                await asyncio.sleep(RETRY_S)
                continue
            h = dict(f.header)
            if f.rt:
                h["rt"] = f.rt
            h["wq"] = max(0, round((loop.time() - f.captured_at) * 1000))
            if h.get("end"):
                h["tr"] = {**h["tr"], "sent": ring.sent + 1, "fail": ring.fails}
                if ring.dropped:
                    h["tr"].update(drop=ring.dropped, dropms=round(ring.dropped_samples / 16))
            await self._send_bin(encode_frame(h, f.pcm))
            ring.sent_one()
            rec.frames.append((time.monotonic(), f.seq, f.samples))
            rec.up.append(h)


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
