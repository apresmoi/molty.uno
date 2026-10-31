"""The firmware's listen uplink, ported for the sim: trace fields, shake settle, the resend ring.

Mirrors docs/AGENT-LINK.md "Turn trace" (device side):

- `level_byte` / `clipped`: the `lv` and `cl` header fields (1 byte per 100 ms chunk: RMS
  dBFS -> clamp(round(2 * (dB + 100)), 0, 200); samples saturated after the mic gain).
- `Settle` + `settle_trim`: in a shake's own listen, the audio before the IMU shows the shake
  has settled is dropped — settled = | |a| - 1 g | < 0.15 g continuously for 150 ms, from the
  mic start, capped at 600 ms; the settle point is the moment that is confirmed (the still run's
  start + 150 ms: cutting at the run's start kept the stroke's ringing tail — `molty-sim measure`);
  the first 10 ms kept are faded in.
- `UplinkRing`: frames wait here until they are sent; a failed send keeps its frame and retries
  in order; when more than `max_s` of audio waits, the oldest unsent frames are dropped (a seq
  gap) and counted.
- Synthetic shake material for the scenarios: an IMU trace (`shake_imu`) and the handling noise
  the mic picks up while the hand shakes Molty (`handling_noise`), both seeded (deterministic).
"""

from __future__ import annotations

import math
from collections import deque
from dataclasses import dataclass, field

import numpy as np

RATE = 16000
CHUNK = 1600  # 100 ms
IMU_HZ = 50

SETTLE_TOL_G = 0.15
SETTLE_STILL_MS = 150
SETTLE_CAP_MS = 600
SETTLE_FADE_MS = 10


def level_byte(pcm: np.ndarray) -> int:
    """`lv` byte of one chunk: RMS dBFS -> clamp(round(2 * (dB + 100)), 0, 200); digital silence 0."""
    x = np.asarray(pcm, dtype=np.float64)
    if x.size == 0:
        return 0
    ms = float(np.mean(np.square(x))) / (32768.0 * 32768.0)
    if ms <= 1e-12:
        return 0
    db = 10 * math.log10(ms)
    return max(0, min(200, round(2 * (db + 100))))


def byte_db(b: int) -> float:
    return b / 2 - 100


def clipped(pcm: np.ndarray) -> int:
    x = np.asarray(pcm, dtype=np.int32)
    return int(np.count_nonzero((x >= 32767) | (x <= -32768)))


def apply_gain(raw: np.ndarray, gain: int) -> np.ndarray:
    """The board's mic gain: raw int16 * gain, saturated (mole_audio.c)."""
    return np.clip(np.asarray(raw, dtype=np.int32) * int(gain), -32768, 32767).astype(np.int16)


# ---------------- shake settle ----------------

@dataclass
class Settle:
    """The firmware's shake settle detector, fed IMU samples (g) with their ms from the mic start."""

    tol_g: float = SETTLE_TOL_G
    still_ms: int = SETTLE_STILL_MS
    cap_ms: int = SETTLE_CAP_MS
    still_since: int | None = None
    at_ms: int | None = None  # the settle point: where the kept audio starts
    still_from: int | None = None  # where the still run that settled it began
    capped: bool = False

    @property
    def done(self) -> bool:
        return self.at_ms is not None

    def feed(self, ax: float, ay: float, az: float, ms: int) -> bool:
        if self.done:
            return True
        if ms >= self.cap_ms:
            return self.tick(ms)
        if abs(math.sqrt(ax * ax + ay * ay + az * az) - 1.0) < self.tol_g:
            if self.still_since is None:
                self.still_since = ms
            if ms - self.still_since >= self.still_ms:
                self.at_ms, self.still_from = ms, self.still_since
        else:
            self.still_since = None
        return self.done

    def tick(self, now_ms: int) -> bool:
        """No settle by the cap: the cap is the settle point."""
        if not self.done and now_ms >= self.cap_ms:
            self.at_ms, self.capped = self.cap_ms, True
        return self.done


def settle_trim(pcm: np.ndarray, at_ms: int, fade_ms: int = SETTLE_FADE_MS) -> np.ndarray:
    """The audio from `at_ms` on (pcm starts at the mic start), its first fade_ms faded in."""
    y = np.asarray(pcm[int(at_ms) * RATE // 1000 :], dtype=np.float64).copy()
    n = min(len(y), fade_ms * RATE // 1000)
    if at_ms > 0 and n:
        y[:n] *= (np.arange(n) + 1) / (n + 1)
    return np.round(y).astype(np.int16)


def settle_of(imu: list[tuple[float, float, float, int]], until_ms: int | None = None) -> Settle:
    s = Settle()
    for ax, ay, az, ms in imu:
        if until_ms is not None and ms > until_ms:
            break
        if s.feed(ax, ay, az, ms):
            break
    if not s.done:
        s.tick(until_ms if until_ms is not None else (imu[-1][3] if imu else 0))
    return s


# ---------------- synthetic shake ----------------

def shake_imu(peak_g: float, after_s: float, total_s: float = 3.0, seed: int = 1, hz: int = IMU_HZ) -> list[tuple[float, float, float, int]]:
    """IMU samples (ax, ay, az in g, ms from the mic start): the shake goes on for `after_s` after
    the detection (mic start) at ~5 Hz with |dyn| up to peak_g, decays over 120 ms, then Molty is
    held still (gravity on z plus 0.01 g sensor noise)."""
    rng = np.random.default_rng(seed)
    out = []
    for i in range(int(total_s * hz)):
        t = i / hz
        ms = round(t * 1000)
        if t < after_s:
            amp = peak_g * (0.6 + 0.4 * abs(math.sin(math.pi * t / max(after_s, 1e-3))))
        elif t < after_s + 0.12:
            amp = peak_g * 0.25 * (1 - (t - after_s) / 0.12)
        else:
            amp = 0.0
        dyn = amp * math.sin(2 * math.pi * 5.0 * t + 0.3)
        n = rng.normal(0, 0.01, 3)
        # a hand's stroke is 3-D: the vertical part moves |a| itself (a purely sideways one would
        # only change it quadratically)
        out.append((dyn + n[0], 0.3 * dyn + n[1], 1.0 + 0.6 * dyn + n[2], ms))
    return out


def handling_noise(after_s: float, seed: int = 1, peak: float = 0.9) -> np.ndarray:
    """What the mic hears while the hand shakes Molty (float @16 kHz, from the mic start): a thump
    per stroke (a decaying 60-140 Hz knock) plus broadband rattle, peaking near full scale, with a
    short ringing tail (60 ms) after the shake stops."""
    rng = np.random.default_rng(seed)
    n = int((after_s + 0.06) * RATE)
    y = np.zeros(n)
    stroke = 0.1  # 5 Hz shake: a stroke every 100 ms (both directions)
    t = 0.0
    while t < after_s:
        a = int(t * RATE)
        L = min(n - a, int(0.09 * RATE))
        tt = np.arange(L) / RATE
        f = rng.uniform(60, 140)
        knock = np.sin(2 * np.pi * f * tt) * np.exp(-tt / 0.025)
        rattle = rng.standard_normal(L) * np.exp(-tt / 0.012) * 0.6
        y[a : a + L] += rng.uniform(0.6, 1.0) * (knock + rattle)
        t += stroke * rng.uniform(0.85, 1.15)
    m = float(np.max(np.abs(y))) or 1.0
    return (y / m * peak).astype(np.float32)


# ---------------- the resend ring ----------------

@dataclass
class UpFrame:
    seq: int
    header: dict
    pcm: bytes
    captured_at: float  # wall (monotonic) time its last sample was captured
    samples: int
    rt: int = 0


@dataclass
class UplinkRing:
    """Frames waiting for the link, oldest first. Over `max_s` of audio waiting: the oldest go."""

    max_s: float = 10.0
    q: deque = field(default_factory=deque)
    dropped: int = 0
    dropped_samples: int = 0
    fails: int = 0
    sent: int = 0

    def waiting_samples(self) -> int:
        return sum(f.samples for f in self.q)

    def push(self, f: UpFrame) -> list[UpFrame]:
        self.q.append(f)
        out = []
        # never drop the frame just captured, nor an end frame
        while self.waiting_samples() > self.max_s * RATE and len(self.q) > 1 and not self.q[0].header.get("end"):
            d = self.q.popleft()
            self.dropped += 1
            self.dropped_samples += d.samples
            out.append(d)
        return out

    def head(self) -> UpFrame | None:
        return self.q[0] if self.q else None

    def failed(self) -> None:
        self.q[0].rt += 1
        self.fails += 1

    def sent_one(self) -> UpFrame:
        self.sent += 1
        return self.q.popleft()


def in_windows(t: float, windows: list[tuple[float, float]]) -> bool:
    return any(a <= t < b for a, b in windows)


DMA_S = 0.09  # the I2S RX DMA ring (6 x 240 frames): audio the mic keeps while nobody reads it
SEND_TIMEOUT_S = 2.0  # esp_websocket_client_send_bin's timeout (link_client.c)


def deliver_offline(speech_s: float, stalls: list[tuple[float, float]], resend: bool, frame_s: float = 0.3, retry_s: float = 0.1,
                    max_s: float = 10.0) -> dict:
    """A listen of `speech_s` of mic time through link stalls (a send attempt inside a window fails).

    resend (this firmware): the mic is read by its own task, frames wait in the ring and a failed
    one is retried in order every retry_s — nothing is lost unless > max_s waits.
    old (before): the audio task sent each 300 ms frame itself; a send inside a stall blocked for
    the 2 s send timeout, failed and the frame was gone — and while it blocked, nobody read the
    mic: all but the DMA ring's 90 ms of that time was never captured.
    Returns wall ms, audio captured / delivered (ms), frames lost, failed attempts, order."""
    fs = int(round(frame_s * RATE))
    if not resend:
        t = 0.0  # wall
        captured = delivered = fails = lost_frames = 0
        while t < speech_s - 1e-9:
            t += frame_s  # a frame's 300 ms read in real time
            captured += fs
            if in_windows(t, stalls):
                fails += 1
                lost_frames += 1
                t += SEND_TIMEOUT_S  # blocked: the mic overruns
                captured += int(DMA_S * RATE)  # only the DMA ring survives (it goes out with the next frame)
                delivered += int(DMA_S * RATE)
            else:
                delivered += fs
        return {"wall_ms": round(t * 1000), "captured_ms": round(captured / 16), "delivered_ms": round(delivered / 16),
                "lost_frames": lost_frames, "failed_attempts": fails, "dropped_by_ring": 0, "in_order": True,
                "last_frame_delay_ms": 0}
    n_frames = round(speech_s / frame_s)
    ring = UplinkRing(max_s=max_s)
    delivered: list[int] = []
    t = 0.0
    k = 0
    last_delay = 0.0
    while k < n_frames or ring.q:
        while k < n_frames and (k + 1) * frame_s <= t + 1e-9:
            ring.push(UpFrame(k, {}, b"", (k + 1) * frame_s, fs))
            k += 1
        f = ring.head()
        if f is None:
            t = (k + 1) * frame_s
            continue
        if in_windows(t, stalls):
            ring.failed()
            t += retry_s
            continue
        ring.sent_one()
        delivered.append(f.seq)
        last_delay = t - f.captured_at
    return {"wall_ms": round(n_frames * frame_s * 1000), "captured_ms": round(n_frames * fs / 16), "delivered_ms": round(len(delivered) * fs / 16),
            "lost_frames": n_frames - len(delivered), "failed_attempts": ring.fails, "dropped_by_ring": ring.dropped,
            "in_order": delivered == sorted(delivered), "last_frame_delay_ms": round(last_delay * 1000)}
