"""`molty-sim measure`: offline numbers for the turn-trace fixes — no voice box, no relay.

1. shake noise: a hard shake's handling noise ahead of speech (scenarios/trace.yaml shake-noise,
   synthetic and seeded), through the firmware's settle rule: what the listen would send in its
   first 600 ms with and without the trim.
2. Wi-Fi stall: a 9 s listen with a 3 s uplink stall, the old uplink (send in the audio task, 2 s
   timeout, a failed frame dropped, the mic unread while blocked) vs the resend ring.
3. mic gain: a loud speech-like clip whose raw level clips at gain 8 (peak 1.22x full scale, the
   field clip of 2026-10-31), saturated samples at gain 8 vs 6.
"""

from __future__ import annotations

import math

import numpy as np

from . import vad as vadm
from .uplink import RATE, apply_gain, clipped, deliver_offline, handling_noise, settle_of, settle_trim, shake_imu

WINDOW_MS = 600


def speechlike(seconds: float, seed: int = 7, f0: float = 140.0) -> np.ndarray:
    """A voiced, syllabic signal (float, peak ~1): harmonics of a jittered f0 under a ~4 Hz
    syllable envelope with short gaps — speech's crest factor and rhythm, not its words."""
    rng = np.random.default_rng(seed)
    n = int(seconds * RATE)
    t = np.arange(n) / RATE
    f = f0 * (1 + 0.08 * np.sin(2 * np.pi * 0.7 * t) + 0.01 * rng.standard_normal(n).cumsum() / math.sqrt(n))
    ph = 2 * np.pi * np.cumsum(f) / RATE
    y = sum((0.8 ** k) * np.sin(k * ph + rng.uniform(0, 6.28)) for k in range(1, 12))
    env = np.zeros(n)
    at = 0.0
    while at < seconds:
        d = rng.uniform(0.12, 0.3)
        a, b = int(at * RATE), min(n, int((at + d) * RATE))
        env[a:b] = np.sin(np.linspace(0, np.pi, b - a)) ** 0.6 * rng.uniform(0.5, 1.0)
        at += d + rng.uniform(0.03, 0.2)
    y = y * env
    return (y / (np.max(np.abs(y)) or 1)).astype(np.float32)


def _db(x: np.ndarray) -> float:
    x = np.asarray(x, dtype=np.float64) / 32768.0
    return round(10 * math.log10(float(np.mean(x * x)) + 1e-12), 1) if x.size else -120.0


def shake_noise(peak_g: float = 8.2, after_s: float = 0.45, lead_s: float = 0.8, seed: int = 11) -> dict:
    rng = np.random.default_rng(seed)
    n = int((lead_s + 2.0) * RATE)
    speech = np.zeros(n, np.float32)
    s = speechlike(2.0, seed=seed) * (10 ** (-24 / 20)) * math.sqrt(2)
    speech[int(lead_s * RATE) : int(lead_s * RATE) + len(s)] = s[: n - int(lead_s * RATE)]
    room = rng.standard_normal(n).astype(np.float32) * 10 ** (-62 / 20)
    hand = np.zeros(n, np.float32)
    h = handling_noise(after_s, seed=seed)
    hand[: len(h)] = h
    mic = np.clip(np.round((speech + room + hand) * 32768), -32768, 32767).astype(np.int16)
    st = settle_of(shake_imu(peak_g, after_s, seed=seed))
    kept = settle_trim(mic, st.at_ms)  # what goes up, from mic time at_ms
    first = mic[: WINDOW_MS * RATE // 1000]
    kept_first = kept[: max(0, WINDOW_MS - st.at_ms) * RATE // 1000]
    hand16 = hand * 32768.0
    e_all = float(np.sum(np.square(hand16, dtype=np.float64)))
    e_left = float(np.sum(np.square(hand16[st.at_ms * RATE // 1000 :], dtype=np.float64)))

    def voiced_ms(x: np.ndarray) -> int:
        v = vadm.Vad(vadm.VadCfg(max_ms=30000))
        for i in range(0, len(x), 1600):
            v.feed(x[i : i + 1600])
        return v.speech_ms

    lead = int(lead_s * RATE)
    # the other reading of the rule: cut where the still run began (150 ms earlier)
    det = st.still_from if st.still_from is not None else st.at_ms
    kept_det = settle_trim(mic, det)
    e_det = float(np.sum(np.square(hand16[det * RATE // 1000 :], dtype=np.float64)))
    return {
        "peak_g": peak_g, "shake_after_s": after_s, "speech_from_ms": round(lead_s * 1000),
        "settle_ms": st.at_ms, "settle_capped": st.capped,
        "first_600ms_rms_dbfs": {"untrimmed": _db(first), "trimmed": _db(kept_first)},
        "first_600ms_clipped_samples": {"untrimmed": clipped(first), "trimmed": clipped(kept_first)},
        "handling_energy_sent_pct": {"untrimmed": 100.0, "trimmed": round(100 * e_left / (e_all or 1), 2)},
        "vad_voiced_ms_before_speech": {"untrimmed": voiced_ms(mic[:lead]), "trimmed": voiced_ms(kept[: max(0, lead - st.at_ms * RATE // 1000)])},
        "speech_kept": st.at_ms <= lead_s * 1000,
        "if_cut_at_still_start": {"settle_ms": det, "handling_energy_sent_pct": round(100 * e_det / (e_all or 1), 2),
                                "vad_voiced_ms_before_speech": voiced_ms(kept_det[: max(0, lead - det * RATE // 1000)]),
                                "speech_kept": det <= lead_s * 1000},
    }


def wifi_stall(listen_s: float = 9.0, at_s: float = 2.0, for_s: float = 3.0) -> dict:
    w = [(at_s, at_s + for_s)]
    return {"listen_s": listen_s, "stall": [at_s, at_s + for_s], "old": deliver_offline(listen_s, w, resend=False),
            "resend": deliver_offline(listen_s, w, resend=True)}


def mic_gain(peak_x_full_scale_at_8: float = 1.22, seed: int = 5) -> dict:
    x = speechlike(4.0, seed=seed)  # peak 1.0
    raw = np.round(x * (peak_x_full_scale_at_8 * 32767 / 8)).astype(np.int16)
    out = {}
    for g in (8, 6):
        y = apply_gain(raw, g)
        out[f"gain_{g}"] = {"clipped_samples": clipped(y), "peak_permille": round(int(np.max(np.abs(y.astype(np.int32)))) * 1000 / 32767),
                            "active_rms_dbfs": _db(y[np.abs(y) > 300])}
    out["clip_length_s"] = 4.0
    out["raw_peak_x_full_scale_at_gain_8"] = peak_x_full_scale_at_8
    return out


def measure() -> dict:
    return {"shake_noise": shake_noise(), "shake_quick": shake_noise(peak_g=3.0, after_s=0.1, lead_s=0.4, seed=12),
            "wifi_stall": wifi_stall(), "mic_gain": mic_gain()}
