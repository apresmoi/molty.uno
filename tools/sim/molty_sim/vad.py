"""The device's end-of-speech VAD, ported 1:1 from device/firmware/main/audio/vad.{h,c}.

20 ms frames, RMS in dBFS against an adaptive noise floor (minimum of 100 ms block minima
over floor_window_ms), calibration replay, hysteresis. Used by the sim device exactly where
the firmware uses it: on every 100 ms mic chunk, unless the listen came with vad:false
(the relay turns it off when the voice box decides the end of turn).
"""

from __future__ import annotations

import math
from dataclasses import dataclass, replace

import numpy as np

RATE = 16000
FRAME_MS = 20
FRAME_SAMPLES = RATE * FRAME_MS // 1000
BLOCK_FRAMES = 5
RING_MAX = 50
CALIB_MAX = 50
SILENT_DB = -120.0

CONTINUE, END_SILENCE, END_NO_SPEECH, END_MAX = 0, 1, 2, 3
WAITING, SPEECH, TRAILING = 0, 1, 2
REASONS = {END_SILENCE: "silence", END_NO_SPEECH: "no_speech", END_MAX: "max"}


@dataclass(frozen=True)
class VadCfg:
    enabled: bool = True
    max_ms: int = 10000
    end_silence_ms: int = 700
    no_speech_ms: int = 4000
    min_speech_ms: int = 160
    min_record_ms: int = 600
    calib_ms: int = 300
    floor_window_ms: int = 3000
    on_db: float = 10.0
    off_db: float = 5.0
    abs_min_dbfs: float = -60.0
    init_floor_max_dbfs: float = -50.0


def _clamp(x, lo, hi):
    return lo if x < lo else hi if x > hi else x


class Vad:
    def __init__(self, cfg: VadCfg | None = None):
        c = cfg or VadCfg()
        fw = _clamp(c.floor_window_ms, 100, RING_MAX * 100)
        cap = CALIB_MAX * FRAME_MS
        c = replace(c, floor_window_ms=fw, calib_ms=_clamp(c.calib_ms, FRAME_MS, fw if fw < cap else cap),
                    off_db=min(c.off_db, c.on_db))
        self.cfg = c
        self.floor_db = self.last_db = self.peak_db = SILENT_DB
        self.block_min = math.inf
        self.block_n = 0
        self.ring = [0.0] * RING_MAX
        self.ring_pos = self.ring_len = 0
        self.calib: list[float] = []
        self.t_ms = 0
        self.phase = WAITING
        self.speech_ms = 0
        self.silence_ms = 0
        self.verdict = CONTINUE
        self._acc = 0
        self._acc_n = 0

    def _floor_now(self) -> float:
        m = self.block_min
        for i in range(self.ring_len):
            m = min(m, self.ring[i])
        return m

    def _floor_push(self, db: float) -> None:
        self.block_min = min(self.block_min, db)
        self.block_n += 1
        if self.block_n < BLOCK_FRAMES:
            return
        cap = self.cfg.floor_window_ms // 100
        self.ring[self.ring_pos] = self.block_min
        self.ring_pos = (self.ring_pos + 1) % cap
        if self.ring_len < cap:
            self.ring_len += 1
        self.block_n = 0
        self.block_min = math.inf

    def _floor_capped(self) -> float:
        f = self._floor_now()
        if self.t_ms < self.cfg.floor_window_ms and f > self.cfg.init_floor_max_dbfs:
            f = self.cfg.init_floor_max_dbfs
        return f

    def _classify(self, db: float) -> None:
        c = self.cfg
        was = self.phase == SPEECH
        if db > c.abs_min_dbfs and db > self.floor_db + (c.off_db if was else c.on_db):
            self.phase = SPEECH
            self.speech_ms += FRAME_MS
            self.silence_ms = 0
        elif self.phase != WAITING:
            self.phase = TRAILING
            self.silence_ms += FRAME_MS

    def _frame(self, db: float) -> int:
        c = self.cfg
        self.last_db = db
        self.peak_db = max(self.peak_db, db)
        calibrated = self.t_ms >= c.calib_ms
        if calibrated and c.enabled:
            self.floor_db = self._floor_capped()
            self._classify(db)
        elif c.enabled and len(self.calib) < CALIB_MAX:
            self.calib.append(db)
        self._floor_push(db)
        self.t_ms += FRAME_MS
        if not calibrated:
            self.floor_db = self._floor_now()
            if c.enabled and self.t_ms >= c.calib_ms:
                self.floor_db = self._floor_capped()
                for d in self.calib:
                    self._classify(d)
                self.calib = []
        if c.enabled:
            spoke = self.speech_ms >= c.min_speech_ms
            if spoke and self.phase == TRAILING and self.silence_ms >= c.end_silence_ms and self.t_ms >= c.min_record_ms:
                return END_SILENCE
            if not spoke and self.phase != SPEECH and self.t_ms >= c.no_speech_ms:
                return END_NO_SPEECH
        if self.t_ms >= c.max_ms:
            return END_MAX
        return CONTINUE

    def feed(self, pcm: np.ndarray) -> int:
        """pcm: int16 samples. Returns the verdict (sticky once not CONTINUE)."""
        x = np.asarray(pcm, dtype=np.int64)
        i = 0
        while i < len(x) and self.verdict == CONTINUE:
            take = min(FRAME_SAMPLES - self._acc_n, len(x) - i)
            seg = x[i : i + take]
            self._acc += int(np.dot(seg, seg))
            self._acc_n += take
            i += take
            if self._acc_n < FRAME_SAMPLES:
                continue
            ms = self._acc / (FRAME_SAMPLES * 32768.0 * 32768.0)
            db = 10 * math.log10(ms) if ms > 1e-12 else SILENT_DB
            self._acc = self._acc_n = 0
            self.verdict = self._frame(db)
        return self.verdict

    def speech_known(self) -> bool:
        return self.cfg.enabled and self.t_ms >= self.cfg.calib_ms
