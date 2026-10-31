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


# ---------------------------------------------------------------- shake turns: partials, drafts, reacts


@dataclass
class ShakeFacts:
    """One shake turn in one arm (baseline | drafts), on the wall clock (monotonic s)."""

    arm: str
    speech_end_at: float | None  # the human's last voiced sample
    first_voice_at: float | None  # Molty's first sample at the speaker after the turn (opener or reply)
    partials: int = 0
    drafted: bool = False
    draft_error: str | None = None
    used: bool = False
    dropped: str | None = None  # draft.dropped reason
    family: str | None = None  # the drafted opener's family
    answer: str | None = None  # the family that fits the full utterance
    opener_end_at: float | None = None  # the opener's last sample left the speaker
    follow_start_at: float | None = None  # the follow-up's first sample at the speaker
    react_sent_at: float | None = None
    react_shown_at: float | None = None  # the react mood reached the device
    react_word_at: float | None = None  # when the human spoke the last word of the partial that triggered it
    react_error: str | None = None
    ack: str = "off"


def shake_metrics(f: ShakeFacts) -> dict:
    m: dict = {"arm": f.arm, "partials": f.partials, "ack": f.ack}
    if f.speech_end_at is not None and f.first_voice_at is not None:
        m["gap_ms"] = round((f.first_voice_at - f.speech_end_at) * 1000)
    if f.arm == "drafts":
        m["drafted"] = f.drafted
        m["draft_error"] = f.draft_error
        m["used"] = f.used
        m["dropped"] = f.dropped
        # an opener played whose family does not fit what the human finally said
        m["wrong_opener"] = bool(f.used and f.family is not None and f.answer is not None and f.family != f.answer)
        # the relay dropped an opener that would have been wrong (saved) / one that would have fit (lost)
        m["saved"] = bool(f.dropped and f.family is not None and f.family != f.answer)
        m["lost"] = bool(f.dropped in ("new_words", "meaning", "revised") and f.family is not None and f.family == f.answer)
        if f.opener_end_at is not None and f.follow_start_at is not None:
            m["seam_ms"] = round((f.follow_start_at - f.opener_end_at) * 1000)
        if f.react_sent_at is not None:
            m["react_error"] = f.react_error
            if f.react_shown_at is not None:
                m["react_rtt_ms"] = round((f.react_shown_at - f.react_sent_at) * 1000)
                if f.react_word_at is not None:
                    m["react_lag_ms"] = round((f.react_shown_at - f.react_word_at) * 1000)
    return m


def drafts_summary(rows: list[dict]) -> dict:
    """Both arms side by side: the perceived gap with vs without drafts, the draft record, react timing."""
    base = [r for r in rows if r.get("arm") == "baseline"]
    dr = [r for r in rows if r.get("arm") == "drafts"]
    by_id = {r["id"]: r for r in base}
    paired = [by_id[r["id"]]["gap_ms"] - r["gap_ms"] for r in dr if r["id"] in by_id and r.get("gap_ms") is not None and by_id[r["id"]].get("gap_ms") is not None]
    drafted = [r for r in dr if r.get("drafted")]
    used = [r for r in drafted if r.get("used")]
    reasons: dict = {}
    for r in drafted:
        if r.get("dropped"):
            reasons[r["dropped"]] = reasons.get(r["dropped"], 0) + 1
    used_gaps = [r.get("gap_ms") for r in used]
    return {
        "gap_baseline_ms": stats([r.get("gap_ms") for r in base]),
        "gap_drafts_ms": stats([r.get("gap_ms") for r in dr]),
        "gap_drafts_used_ms": stats(used_gaps),
        "gap_saved_ms": stats(paired),
        "turns_with_partials": sum(1 for r in dr if r.get("partials")),
        "drafted": len(drafted),
        "used": len(used),
        "use_rate": round(len(used) / len(drafted), 2) if drafted else None,
        "dropped": reasons,
        "wrong_openers": sum(1 for r in used if r.get("wrong_opener")),
        "wrong_opener_rate": round(sum(1 for r in used if r.get("wrong_opener")) / len(used), 2) if used else None,
        "saved_by_relay": sum(1 for r in drafted if r.get("saved")),
        "lost_by_relay": sum(1 for r in drafted if r.get("lost")),
        "draft_errors": sum(1 for r in dr if r.get("draft_error")),
        "seam_ms": stats([r.get("seam_ms") for r in used]),
        "reacts": sum(1 for r in dr if "react_error" in r),
        "react_errors": [r["react_error"] for r in dr if r.get("react_error")],
        "react_rtt_ms": stats([r.get("react_rtt_ms") for r in dr]),
        "react_lag_ms": stats([r.get("react_lag_ms") for r in dr]),
    }
