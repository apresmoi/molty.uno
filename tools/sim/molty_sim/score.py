"""The scoreboard (docs/talk-proposal.html section 9) for labelled turns: pure functions over wall-clock facts.

Clock: monotonic seconds, the same in the sim device and the run. Molty's voice is what the
speaker model plays (device.SayRec), cut into voiced spans by level; the human's voice is the
clip's voiced intervals from the moment the mic opened (the human keeps talking after a cut).

- first sound: the human's last voiced sample -> Molty's first voiced sample of words (a filler,
  a quick phrase, first words or the answer; never the chirp, reported apart as `chirp_ms`).
- grounded answer: -> the first voiced sample of the agent's answer, when it is the right fact.
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

from .metrics import pct
from .voice import frame_db

RATE = 16000
VOICE_FRAME = 320  # 20 ms
VOICE_REL_DB = -35.0  # a frame within 35 dB of the say's loudest is voice
VOICE_FLOOR_DB = -55.0
VOICE_GAP_S = 0.10  # voiced frames closer than this are one span
TALK_OVER_TOL_MS = 40
TAP_STOP_MS = 300  # interruption recovery: Molty stopped within this of the tap
GROUNDED = ("hand_off", "refuse")
NOT_APPLICABLE = ("agent_free_turns", "hand_off_precision", "hand_off_recall", "one_voice")


def voiced_spans(pcm: np.ndarray, start: float, play_time=None) -> list[tuple[float, float]]:
    """Wall intervals of voice in a say's audio: `play_time(sample)` maps a sample to the wall
    (the speaker model, underruns included); without it the audio plays from `start` unbroken."""
    x = np.asarray(pcm, dtype=np.float32) / 32768.0 if np.asarray(pcm).dtype == np.int16 else np.asarray(pcm, dtype=np.float32)
    db = frame_db(x, VOICE_FRAME)
    if db.size == 0:
        return []
    on = db > max(db.max() + VOICE_REL_DB, VOICE_FLOOR_DB)
    at = play_time or (lambda smp: start + smp / RATE)
    spans: list[list[float]] = []
    for i in np.nonzero(on)[0]:
        a = at(int(i) * VOICE_FRAME)
        b = at(int(i) * VOICE_FRAME + VOICE_FRAME - 1)
        if a is None or b is None:
            continue
        b += 1 / RATE
        if spans and a - spans[-1][1] < VOICE_GAP_S:
            spans[-1][1] = max(spans[-1][1], b)
        else:
            spans.append([a, b])
    return [(a, b) for a, b in spans]


def overlap_s(xs: list[tuple[float, float]], ys: list[tuple[float, float]]) -> float:
    return sum(max(0.0, min(b, d) - max(a, c)) for a, b in xs for c, d in ys)


@dataclass
class Voice:
    """One thing Molty said in a turn's answer window."""

    kind: str  # answer | filler | quick | opener
    text: str
    spans: list[tuple[float, float]]


@dataclass
class ScoreFacts:
    label: str | None
    truth: str | None
    pick: str | None  # the agent's fact (None: it never answered)
    think_s: float | None
    eos_at: float | None  # wall: the human's last voiced sample (None: nobody spoke)
    human: list[tuple[float, float]]  # wall: the human's voiced intervals this turn
    voices: list[Voice]  # what Molty said between this listen's end and the next listen
    ack_at: float | None = None  # wall: the device's chirp / thinking (None: off)
    cut: bool = False
    pauses: bool = False  # the human pauses mid-sentence in this turn
    reason_ok: bool = True
    answer: str | None = None  # the agent's answer (its fact's line)
    tap_at: float | None = None  # the human tapped the say before this listen
    tapped: list[tuple[float, float]] = field(default_factory=list)  # that say's voiced spans


def ms(a: float | None, b: float | None) -> int | None:
    return round((b - a) * 1000) if a is not None and b is not None else None


def _numbers_names(text: str) -> set[str]:
    out = set()
    for i, w in enumerate(text.replace(",", " ").split()):
        t = w.strip(".!?;:'\"()")
        if any(c.isdigit() for c in t) or (i and t[:1].isupper() and t not in ("I",)):
            out.add(t)
    return out


def score_turn(f: ScoreFacts) -> dict:
    m: dict = {"label": f.label, "truth": f.truth, "pick": f.pick}
    spoke = f.eos_at is not None
    starts = [(v.spans[0][0], v) for v in f.voices if v.spans]
    first = min(starts, key=lambda x: x[0], default=None)
    if spoke and first:
        m["first_sound_ms"] = ms(f.eos_at, first[0])
        m["first_kind"] = first[1].kind
    lead = min(((a, v) for a, v in starts if v.kind != "answer"), key=lambda x: x[0], default=None)
    if spoke and lead:
        m["lead_in_ms"], m["lead_in_kind"] = ms(f.eos_at, lead[0]), lead[1].kind
    if spoke and f.ack_at is not None:
        m["chirp_ms"] = ms(f.eos_at, f.ack_at)
    ans = next((v for v in f.voices if v.kind == "answer" and v.spans), None)
    m["answered"] = ans is not None
    m["correct"] = (f.pick == f.truth) if f.truth is not None and ans is not None else None
    if spoke and ans is not None and m["correct"]:
        m["grounded_ms"] = ms(f.eos_at, ans.spans[0][0])
        if f.think_s is not None:
            m["grounded_less_think_ms"] = m["grounded_ms"] - round(f.think_s * 1000)
    molty = [s for v in f.voices for s in v.spans]
    m["talk_over_ms"] = round(overlap_s(f.human, molty) * 1000)
    m["pause_takeover"] = f.cut if f.pauses else None  # the turn was ended inside a mid-sentence pause
    if f.tap_at is not None:
        after = [b for a, b in f.tapped if b > f.tap_at]
        m["tap_stop_ms"] = ms(f.tap_at, max(after)) if after else 0
        m["tap_recovered"] = m["tap_stop_ms"] <= TAP_STOP_MS and bool(m["correct"])
    if f.label in GROUNDED and ans is not None and f.answer:
        need = _numbers_names(f.answer)
        said = " ".join(v.text for v in f.voices if v.kind == "answer")
        m["fidelity"] = all(n in said for n in need) if need else None
    fails = []
    if not f.reason_ok:
        fails.append("reason")
    if f.cut:
        fails.append("cut")
    if m["talk_over_ms"] > TALK_OVER_TOL_MS:
        fails.append("talk_over")
    if f.truth is not None and not m["answered"]:
        fails.append("no_answer")
    elif m["correct"] is False:
        fails.append("wrong")
    if f.tap_at is not None and not m.get("tap_recovered"):
        fails.append("tap")
    m["fails"], m["passed"] = fails, not fails
    return m


def _st(xs: list) -> dict | None:
    v = [x for x in xs if x is not None]
    if not v:
        return None
    return {"n": len(v), "p50": round(pct(v, 50)), "p90": round(pct(v, 90)), "min": round(min(v)), "max": round(max(v))}


def _rate(hit: int, n: int) -> dict:
    return {"hit": hit, "n": n, "rate": round(hit / n, 3) if n else None}


def board(rows: list[dict]) -> dict:
    """The scoreboard over every scored turn of every repeat."""
    g = [r for r in rows if r.get("label") in GROUNDED]
    pauses = [r for r in rows if r.get("pause_takeover") is not None]
    taps = [r for r in rows if "tap_recovered" in r]
    fid = [r for r in rows if r.get("fidelity") is not None]
    leads: dict = {}
    for r in rows:
        if r.get("lead_in_kind"):
            leads[r["lead_in_kind"]] = leads.get(r["lead_in_kind"], 0) + 1
    by_turn: dict[str, dict] = {}
    for r in rows:
        t = by_turn.setdefault(r["id"], {"label": r.get("label"), "runs": 0, "passed": 0, "fails": {}, "first": [], "grounded": []})
        t["runs"] += 1
        t["passed"] += bool(r.get("passed"))
        for x in r.get("fails") or []:
            t["fails"][x] = t["fails"].get(x, 0) + 1
        t["first"].append(r.get("first_sound_ms"))
        t["grounded"].append(r.get("grounded_ms"))
    turns = {k: {"label": v["label"], "passed": f"{v['passed']}/{v['runs']}", "fails": v["fails"],
                 "first_sound_ms": _st(v["first"]), "grounded_ms": _st(v["grounded"])} for k, v in by_turn.items()}
    return {
        "turns": len(rows),
        "first_sound_ms": _st([r.get("first_sound_ms") for r in rows]),
        "first_kinds": {k: sum(1 for r in rows if r.get("first_kind") == k) for k in ("filler", "quick", "opener", "answer")},
        "lead_in_ms": _st([r.get("lead_in_ms") for r in rows]),
        "lead_in_kinds": leads,
        "chirp_ms": _st([r.get("chirp_ms") for r in rows]),
        "grounded_ms": _st([r.get("grounded_ms") for r in g]),
        "grounded_less_think_ms": _st([r.get("grounded_less_think_ms") for r in g]),
        "grounded_turns": {"n": len(g), "correct": sum(1 for r in g if r.get("correct")), "wrong": sum(1 for r in g if r.get("correct") is False),
                           "unanswered": sum(1 for r in g if not r.get("answered"))},
        "false_cutoffs": _rate(sum(1 for r in rows if r.get("false_cutoff")), len(rows)),
        "talk_over_ms": {"total": sum(r.get("talk_over_ms") or 0 for r in rows), "turns": sum(1 for r in rows if (r.get("talk_over_ms") or 0) > TALK_OVER_TOL_MS),
                         "worst": max((r.get("talk_over_ms") or 0 for r in rows), default=0)},
        "pause_takeover": _rate(sum(1 for r in pauses if r["pause_takeover"]), len(pauses)),
        "interruption_recovery": _rate(sum(1 for r in taps if r["tap_recovered"]), len(taps)),
        "tap_stop_ms": _st([r.get("tap_stop_ms") for r in taps]),
        "answer_fidelity": _rate(sum(1 for r in fid if r["fidelity"]), len(fid)),
        "passed": _rate(sum(1 for r in rows if r.get("passed")), len(rows)),
        "not_applicable_today": list(NOT_APPLICABLE),
        "by_turn": turns,
    }


def _fmt(st: dict | None) -> str:
    return "-" if not st else f"{st['p50']} / {st['p90']} (n={st['n']})"


def _fr(r: dict) -> str:
    return "-" if not r["n"] else f"{r['hit']}/{r['n']} ({100 * r['rate']:.0f} %)"


def board_text(b: dict, title: str = "", judge: dict | None = None) -> str:
    gt = b["grounded_turns"]
    lines = [title] if title else []
    lines += [
        f"first sound p50 / p90 (ms)           {_fmt(b['first_sound_ms'])}   first was {b['first_kinds']}",
        f"filler / quick / first words (ms)    {_fmt(b['lead_in_ms'])}   {b['lead_in_kinds']}",
        f"chirp after the human stops (ms)     {_fmt(b['chirp_ms'])}",
        f"grounded answer p50 / p90 (ms)       {_fmt(b['grounded_ms'])}   of {gt['n']}: {gt['correct']} right, {gt['wrong']} wrong, {gt['unanswered']} never answered",
        f"  minus the agent's think time (ms)  {_fmt(b['grounded_less_think_ms'])}",
        f"false cut-offs                       {_fr(b['false_cutoffs'])}",
        f"talk-over                            {b['talk_over_ms']['total']} ms in {b['talk_over_ms']['turns']} turns (worst {b['talk_over_ms']['worst']} ms)",
        f"pause takeover rate                  {_fr(b['pause_takeover'])}",
        f"interruption recovery (tap)          {_fr(b['interruption_recovery'])}   tap -> Molty quiet {_fmt(b['tap_stop_ms'])}",
        f"answer fidelity (numbers, names)     {_fr(b['answer_fidelity'])}",
        f"turns passed                         {_fr(b['passed'])}",
    ]
    if judge:
        lines.append(f"judge ({judge.get('votes', 0)} votes, median)       invented {judge.get('invented')}, answered right {judge.get('answered_right')}, "
                     f"smooth {judge.get('smooth')}/5, one voice {judge.get('one_voice')}/5")
    lines.append(f"not applicable today (no talker)     {', '.join(b['not_applicable_today'])}")
    lines += ["", f"{'turn':14s} {'label':9s} {'passed':7s} {'first sound p50/p90':22s} {'grounded p50/p90':22s} fails"]
    pp = lambda st: f"{st['p50']} / {st['p90']}" if st else "-"  # noqa: E731
    for k, t in b["by_turn"].items():
        lines.append(f"{k:14s} {t['label'] or '-':9s} {t['passed']:7s} {pp(t['first_sound_ms']):22s} {pp(t['grounded_ms']):22s} {t['fails'] or ''}")
    return "\n".join(lines)
