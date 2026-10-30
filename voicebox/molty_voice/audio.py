"""PCM helpers: PCM16 bytes <-> float, and a streaming rational resampler.

Molty and the relay speak PCM16 mono @16 kHz; Kyutai's moshi-server speaks float32
mono @24 kHz. 16k -> 24k is up 3 / down 2, 24k -> 16k is up 2 / down 3. The
resampler is a windowed-sinc polyphase FIR (numpy only) that keeps its filter
history between chunks, so a stream cut into arbitrary chunks resamples exactly
like the whole signal would.
"""

from __future__ import annotations

from math import gcd

import numpy as np

DEVICE_RATE = 16000
MODEL_RATE = 24000


def pcm16_to_float(data: bytes) -> np.ndarray:
    """Little-endian PCM16 bytes -> float32 in [-1, 1). A trailing odd byte is dropped."""
    n = len(data) // 2
    return np.frombuffer(data[: n * 2], dtype="<i2").astype(np.float32) / 32768.0


def float_to_pcm16(x: np.ndarray) -> bytes:
    """float32 in [-1, 1] -> little-endian PCM16 bytes (clipped)."""
    y = np.clip(np.asarray(x, dtype=np.float32), -1.0, 32767.0 / 32768.0)
    return (np.round(y * 32768.0)).astype("<i2").tobytes()


def _lowpass(up: int, down: int, half_width: int = 16) -> np.ndarray:
    """Windowed-sinc low-pass for the upsampled rate: cutoff at the lower Nyquist, gain `up`."""
    m = max(up, down)
    n = 2 * half_width * m + 1
    t = np.arange(n) - (n - 1) / 2
    h = np.sinc(t / m) * np.kaiser(n, 8.0)
    return (h / h.sum() * up).astype(np.float64)


class Resampler:
    """Streaming resampler from `src` to `dst` Hz (rational ratio)."""

    def __init__(self, src: int, dst: int):
        g = gcd(src, dst)
        self.up, self.down = dst // g, src // g
        self.h = _lowpass(self.up, self.down)
        self.taps = len(self.h)
        self.hist_len = -(-self.taps // self.up)  # input samples spanning the filter
        self.hist = np.zeros(self.hist_len, dtype=np.float64)
        self.u0 = -self.hist_len * self.up  # upsampled index of hist[0]
        self.next_out = 0  # next output sample index n (upsampled index n * down)
        self.total_in = 0

    def process(self, x: np.ndarray) -> np.ndarray:
        x = np.asarray(x, dtype=np.float64)
        if x.size == 0:
            return np.zeros(0, dtype=np.float32)
        xin = np.concatenate([self.hist, x])
        u = np.zeros(len(xin) * self.up, dtype=np.float64)
        u[:: self.up] = xin
        v = np.convolve(u, self.h, mode="valid")  # v[i] = y at upsampled index u0 + i + taps - 1
        first = self.u0 + self.taps - 1
        last = first + len(v) - 1
        n_hi = last // self.down
        out = np.zeros(0, dtype=np.float32)
        if n_hi >= self.next_out:
            idx = np.arange(self.next_out, n_hi + 1) * self.down - first
            out = v[idx].astype(np.float32)
            self.next_out = n_hi + 1
        self.total_in += x.size
        self.hist = xin[-self.hist_len :]
        self.u0 += (len(xin) - self.hist_len) * self.up
        return out

    def flush(self) -> np.ndarray:
        """The tail still inside the filter: what the whole signal would have produced."""
        want = (self.total_in * self.up) // self.down
        pending = want - self.next_out
        if pending <= 0:
            return np.zeros(0, dtype=np.float32)
        return self.process(np.zeros(self.hist_len, dtype=np.float64))[:pending]
