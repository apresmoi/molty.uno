"""Per-turn metrics and the run summary — pure functions over recorded facts (tested in tests/)."""

from __future__ import annotations

import math
import statistics
from dataclasses import dataclass

from .align import FILLERS, norm

CUT_TOLERANCE_S = 0.15  # speech left after the end below this is a clipped tail, not a cut-off


def words_for_wer(text: str) -> list[str]:
    return [w for w in (norm(t) for t in text.split()) if w and w not in FILLERS]


def wer(ref: str, hyp: str) -> float | None:
    """Word error rate (fillers and punctuation ignored); None without a reference."""
    r, h = words_for_wer(ref), words_for_wer(hyp)
    if not r:
        return None if not h else 1.0
    d = list(range(len(h) + 1))
    for i in range(1, len(r) + 1):
        prev, d[0] = d[0], i
        for j in range(1, len(h) + 1):
            cur = min(d[j] + 1, d[j - 1] + 1, prev + (r[i - 1] != h[j - 1]))
            prev, d[j] = d[j], cur
    return d[len(h)] / len(r)


@dataclass
class TurnFacts:
    """What happened in one turn, on two clocks: mic time (s from the mic opening) and wall (monotonic)."""

    expect: str
    reason: str | None  # the relay's listen reason
    speech_end_s: float | None  # the human stopped (mic time); None = nobody spoke
    speech_after_end_s: float  # speech in the clip after the listen ended (mic time)
    stop_mic_s: float | None  # listen_stop reached the device (mic time)
    end_mic_s: float | None  # the device's end frame left (mic time)
    mic_open_at: float | None  # wall
    end_at: float | None  # wall: end frame left
    next_play_at: float | None  # wall: the next say's first sample reaches the speaker
    ack: str = "off"


def turn_metrics(f: TurnFacts) -> dict:
    m: dict = {}
    spoke = f.speech_end_s is not None
    ended = f.stop_mic_s if f.stop_mic_s is not None else f.end_mic_s
    if spoke and ended is not None:
        m["eos_to_turn_end_ms"] = round((ended - f.speech_end_s) * 1000)
    if spoke and f.end_mic_s is not None:
        m["eos_to_end_frame_ms"] = round((f.end_mic_s - f.speech_end_s) * 1000)
    m["false_cutoff"] = bool(spoke and f.reason in ("turn_end", "silence", "max") and f.speech_after_end_s > CUT_TOLERANCE_S)
    m["speech_lost_ms"] = round(f.speech_after_end_s * 1000) if spoke else 0
    m["false_no_speech"] = bool(spoke and f.reason == "no_speech")
    m["missed_no_speech"] = bool(f.expect == "no_speech" and f.reason != "no_speech")
    m["no_speech_after_ms"] = round(f.end_mic_s * 1000) if (f.reason == "no_speech" and f.end_mic_s is not None) else None
    m["ack"] = f.ack
    if spoke and f.mic_open_at is not None and f.next_play_at is not None:
        m["gap_ms"] = round((f.next_play_at - (f.mic_open_at + f.speech_end_s)) * 1000)
    if spoke and f.mic_open_at is not None and f.end_at is not None and f.ack != "off":
        m["ack_after_eos_ms"] = round((f.end_at - (f.mic_open_at + f.speech_end_s)) * 1000)
    m["ok"] = f.reason == f.expect and not m["false_cutoff"]
    return m


def pct(xs: list[float], p: float) -> float | None:
    xs = sorted(x for x in xs if x is not None and not (isinstance(x, float) and math.isnan(x)))
    if not xs:
        return None
    k = (len(xs) - 1) * p / 100
    lo, hi = math.floor(k), math.ceil(k)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


def stats(xs: list) -> dict | None:
    v = [x for x in xs if x is not None]
    if not v:
        return None
    return {"n": len(v), "median": round(statistics.median(v), 1), "p90": round(pct(v, 90), 1), "min": round(min(v), 1), "max": round(max(v), 1), "mean": round(statistics.fmean(v), 1)}


SUMMARY_KEYS = [
    ("eos_to_turn_end_ms", "end of speech -> turn end (listen_stop at device)"),
    ("stt_final_ms", "STT final after the end frame (relay)"),
    ("gap_ms", "perceived gap: human stops -> Molty's next voice"),
    ("ack_after_eos_ms", "human stops -> device ack (chirp/thinking)"),
    ("tts_first_audio_ms", "say: request -> first audio at the device"),
    ("relay_first_audio_ms", "say: relay first_audio timing"),
    ("wer", "transcript WER (fillers ignored)"),
    ("cue_err_ms", "say cue - aligned word onset (per word)"),
    ("cue_abs_err_ms", "|say cue - aligned word onset| (per word)"),
]


def summarize(turns: list[dict], cue_errs: list[int]) -> dict:
    s: dict = {"turns": len(turns)}
    for k, _ in SUMMARY_KEYS:
        if k == "cue_err_ms":
            s[k] = stats(cue_errs)
        elif k == "cue_abs_err_ms":
            s[k] = stats([abs(e) for e in cue_errs])
        else:
            s[k] = stats([t.get(k) for t in turns])
    s["false_cutoffs"] = sum(1 for t in turns if t.get("false_cutoff"))
    s["false_no_speech"] = sum(1 for t in turns if t.get("false_no_speech"))
    s["missed_no_speech"] = sum(1 for t in turns if t.get("missed_no_speech"))
    s["reason_ok"] = sum(1 for t in turns if t.get("ok"))
    s["errors"] = sum(1 for t in turns if t.get("error"))
    s["underruns"] = sum(t.get("say_underruns", 0) or 0 for t in turns)
    s["acks"] = {a: sum(1 for t in turns if t.get("ack") == a) for a in ("chirp", "thinking", "off")}
    return s
