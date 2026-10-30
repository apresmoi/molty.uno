"""The simulated human's voice: Kyutai TTS (the box, tunnelled) or a WAV, assembled into a mic signal.

A turn's text is synthesised as ONE stream (natural prosody), then `{pause S}` silences are cut
in at the quietest point just before the word that follows them. Kyutai's word timings
(start_s less its one-step lag, `molty_voice.kyutai.synthesize`) give the ground-truth onset of
every word on the mic timeline. Synthesis is cached in .cache/ by (words, voice), so the same
scenario replays the same audio run after run — A/B tuning compares like with like.
"""

from __future__ import annotations

import hashlib
import json
import math
import wave
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from molty_voice.audio import Resampler
from molty_voice.kyutai import synthesize

from .scenario import Markup, Turn

RATE = 16000
FRAME = 320  # 20 ms


@dataclass
class Clip:
    """The human's speech on the mic timeline (seconds from the moment the mic opened)."""

    pcm: np.ndarray  # float32 @16 kHz, speech only (no noise), starting at t=0 of the mic
    text: str = ""
    words: list[tuple[str, float]] = field(default_factory=list)  # (word, onset_s)
    speech: list[tuple[float, float]] = field(default_factory=list)  # voiced intervals
    noise: dict = field(default_factory=lambda: {"kind": "none"})
    seed: int = 0

    @property
    def speech_end_s(self) -> float | None:
        return self.speech[-1][1] if self.speech else None

    @property
    def speech_start_s(self) -> float | None:
        return self.speech[0][0] if self.speech else None

    def speech_after(self, t: float) -> float:
        """Seconds of speech in the clip after mic time t."""
        return sum(max(0.0, b - max(a, t)) for a, b in self.speech)


def frame_db(x: np.ndarray, frame: int = FRAME) -> np.ndarray:
    n = len(x) // frame
    if n == 0:
        return np.zeros(0)
    r = np.sqrt(np.mean(np.square(x[: n * frame].reshape(n, frame).astype(np.float64)), axis=1))
    return 20 * np.log10(r + 1e-9)


def active_level_db(x: np.ndarray) -> float:
    """RMS (dBFS) over the frames within 30 dB of the loudest: the level of the speech itself."""
    db = frame_db(x)
    if db.size == 0:
        return -120.0
    act = db[db > db.max() - 30]
    return float(10 * np.log10(np.mean(10 ** (act / 10))))


def speech_intervals(x: np.ndarray, rel_db: float = -22.0, min_gap_s: float = 0.12, min_len_s: float = 0.06) -> list[tuple[float, float]]:
    """Voiced intervals of a clean clip: frames within rel_db of its active level, short gaps merged."""
    db = frame_db(x)
    if db.size == 0:
        return []
    thr = max(active_level_db(x) + rel_db, -70.0)
    on = db > thr
    fs = FRAME / RATE
    out: list[list[float]] = []
    for i, v in enumerate(on):
        if not v:
            continue
        a, b = i * fs, (i + 1) * fs
        if out and a - out[-1][1] < min_gap_s:
            out[-1][1] = b
        else:
            out.append([a, b])
    return [(round(a, 3), round(b, 3)) for a, b in out if b - a >= min_len_s]


def resample(x: np.ndarray, src: int, dst: int = RATE) -> np.ndarray:
    if src == dst:
        return np.asarray(x, dtype=np.float32)
    rs = Resampler(src, dst)
    return np.concatenate([rs.process(x), rs.flush()]).astype(np.float32)


def read_wav(path: Path) -> tuple[np.ndarray, int]:
    with wave.open(str(path), "rb") as w:
        if w.getsampwidth() != 2:
            raise ValueError(f"{path}: only PCM16 WAVs")
        ch, rate = w.getnchannels(), w.getframerate()
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float32) / 32768.0
    if ch > 1:
        x = x.reshape(-1, ch).mean(axis=1)
    return x, rate


def write_wav(path: Path, x: np.ndarray, rate: int = RATE) -> None:
    y = np.asarray(x)
    if y.dtype != np.int16:
        y = np.clip(np.round(np.asarray(y, dtype=np.float32) * 32768.0), -32768, 32767).astype("<i2")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(y.astype("<i2").tobytes())


# ---------------- noise ----------------

def noise_block(kind: str, seconds: float, seed: int) -> np.ndarray:
    """Unit-RMS noise (float32 @16 kHz), shaped in the frequency domain: white, pink (1/f), brown (1/f^2)."""
    n = int(seconds * RATE)
    rng = np.random.default_rng(seed)
    if kind == "white":
        y = rng.standard_normal(n)
    else:
        spec = rng.standard_normal(n // 2 + 1) + 1j * rng.standard_normal(n // 2 + 1)
        f = np.fft.rfftfreq(n, 1 / RATE)
        f[0] = f[1]
        spec /= np.sqrt(f) if kind == "pink" else f
        spec[f < 20] = 0  # no DC / sub-audio rumble
        y = np.fft.irfft(spec, n)
    y = y / (np.sqrt(np.mean(y**2)) + 1e-12)
    return y.astype(np.float32)


class Mic:
    """What the sim device's microphone hears: the clip (from t=0) over looping room noise."""

    def __init__(self, clip: Clip | None):
        self.clip = clip
        kind = (clip.noise.get("kind") if clip else "none") or "none"
        db = float(clip.noise.get("db", -62.0)) if clip else -120.0
        self.noise = None if kind == "none" else noise_block(kind, 7.3, clip.seed) * (10 ** (db / 20))
        self.pos = 0

    def read(self, n: int) -> np.ndarray:
        """Next n samples as int16 (the mic keeps going after the clip: noise, or digital silence)."""
        out = np.zeros(n, dtype=np.float32)
        if self.clip is not None and self.pos < len(self.clip.pcm):
            seg = self.clip.pcm[self.pos : self.pos + n]
            out[: len(seg)] += seg
        if self.noise is not None:
            L = len(self.noise)
            idx = (np.arange(self.pos, self.pos + n) % L)
            out += self.noise[idx]
        self.pos += n
        return np.clip(np.round(out * 32768.0), -32768, 32767).astype(np.int16)


# ---------------- synthesis ----------------

class Synth:
    def __init__(self, tts_url: str, cache_dir: Path, api_key: str = "public_token"):
        self.url, self.api_key, self.cache = tts_url, api_key, cache_dir
        self.cache.mkdir(parents=True, exist_ok=True)

    async def tts(self, words: tuple[str, ...], voice: str) -> tuple[np.ndarray, list[tuple[str, float]]]:
        """(float32 @16 kHz, [(word, start_s)]) for the text, synthesised once and cached."""
        key = hashlib.sha1(json.dumps({"w": list(words), "v": voice, "r": 2}).encode()).hexdigest()[:16]
        f = self.cache / f"tts-{key}.npz"
        if f.exists():
            z = np.load(f)
            return z["pcm"], [tuple(w) for w in json.loads(str(z["words"]))]
        chunks: list[np.ndarray] = []
        timing: list[tuple[str, float]] = []

        async def on_audio(x):
            chunks.append(x)

        async def on_word(t, s):
            timing.append((t, round(s, 3)))

        await synthesize(self.url, self.api_key, " ".join(words), voice, on_audio, on_word=on_word)
        pcm = resample(np.concatenate(chunks), 24000)
        np.savez(f, pcm=pcm, words=json.dumps(timing))
        return pcm, timing

    async def clip(self, turn: Turn) -> Clip:
        seed = int(hashlib.sha1(turn.id.encode()).hexdigest()[:8], 16)
        if turn.kind == "silence":
            return Clip(np.zeros(0, np.float32), noise=turn.noise, seed=seed)
        if turn.kind == "wav":
            x, rate = read_wav(turn.wav)
            x = resample(x, rate)
            words: list[tuple[str, float]] = []
        elif turn.kind == "tts":
            x, timing = await self.tts(turn.markup.words, turn.voice)
            x, words = assemble(x, timing, turn.markup)
        else:
            raise NotImplementedError("LLM-written user lines are reserved in the scenario format, not implemented yet")
        x = x * (10 ** ((turn.level_db - active_level_db(x)) / 20))
        peak = float(np.max(np.abs(x))) if x.size else 0
        if peak > 0.98:
            x *= 0.98 / peak
        lead = np.zeros(int(turn.lead_s * RATE), np.float32)
        x = np.concatenate([lead, x]).astype(np.float32)
        words = [(w, round(s + turn.lead_s, 3)) for w, s in words]
        return Clip(x, text=turn.text, words=words, speech=speech_intervals(x), noise=turn.noise, seed=seed)


def quietest(x: np.ndarray, a: int, b: int, win: int = 160) -> int:
    """Sample index in [a, b) at the centre of the quietest `win` window."""
    a, b = max(0, a), min(len(x), b)
    if b - a <= win:
        return max(0, min(len(x), (a + b) // 2))
    e = np.convolve(np.square(x[a:b].astype(np.float64)), np.ones(win), mode="valid")
    return a + int(np.argmin(e)) + win // 2


def assemble(x: np.ndarray, timing: list[tuple[str, float]], m: Markup) -> tuple[np.ndarray, list[tuple[str, float]]]:
    """Trim the TTS's quiet lead/tail, cut the markup's pauses in; returns (pcm, [(word, onset_s)])."""
    n = len(m.words)
    onsets = [s for _, s in timing[:n]]
    while len(onsets) < n:  # the server timed fewer words than sent: spread the rest
        onsets.append(onsets[-1] + 0.3 if onsets else 0.0)
    db = frame_db(x)
    thr = (db.max() - 40) if db.size else -60
    loud = np.nonzero(db > thr)[0]
    start = max(0, int(loud[0]) * FRAME - 160) if loud.size else 0
    end = min(len(x), (int(loud[-1]) + 1) * FRAME + int(0.12 * RATE)) if loud.size else len(x)
    start = min(start, int(onsets[0] * RATE)) if onsets else start
    x = x[start:end]
    onsets = [o - start / RATE for o in onsets]
    cuts: list[tuple[int, int]] = []  # (sample position in x, silence samples)
    for k, sec in m.pauses:
        if k == 0:
            pos = 0
        elif k >= n:
            pos = len(x)
        else:
            # the quietest point just before word k — but past most of word k-1 (a short word
            # must not be skipped: the silence goes between k-1 and k, never before k-1)
            o, prev = onsets[k], onsets[k - 1]
            a = max(o - 0.2, prev + 0.6 * (o - prev))
            pos = quietest(x, int(a * RATE), int((o + 0.03) * RATE))
        cuts.append((pos, int(sec * RATE)))
    parts, last = [], 0
    for pos, sil in sorted(cuts):
        parts += [x[last:pos], np.zeros(sil, np.float32)]
        last = pos
    parts.append(x[last:])
    y = np.concatenate(parts).astype(np.float32)
    # word i moves by every pause that comes before it (pause k sits before word k)
    words = [(w, round(max(0.0, o + sum(int(sec * RATE) for k, sec in m.pauses if k <= i) / RATE), 3))
             for i, (w, o) in enumerate(zip(m.words, onsets))]
    return y, words


def level(x: np.ndarray) -> float:
    return 20 * math.log10(float(np.sqrt(np.mean(np.square(x)))) + 1e-9) if len(x) else -120.0
