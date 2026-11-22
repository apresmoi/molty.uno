"""The judge: a strong text model reads one conversation's timed transcript, the labels and the
fact sheet, and scores a fixed rubric. Pluggable: any command that reads the prompt on stdin and
prints JSON (`--judge 'claude -p --model opus ...'`, or a local model's CLI). Three votes, the
median of each number. No API key is ever passed by the sim.
"""

from __future__ import annotations

import json
import re
import shlex
import statistics
import subprocess
from pathlib import Path

SYSTEM = "You are a strict evaluator of a voice assistant's conversation. Output only one JSON object, nothing else."

RUBRIC = """Rubric (fixed):
- invented: how many times Molty states a fact that is NOT in the fact sheet (small talk, fillers like
  "one sec", and saying it does not know are not facts). An integer.
- answered_right: of the turns labelled hand_off or refuse, how many got the right answer from Molty
  (the fact named in `truth`; for truth `unknown`, saying it does not know). An integer.
- smooth: 1 to 5, how smooth the turn taking felt from the timings: 5 = answers soon after the human
  stops, no dead air over 3 s, nobody cut off; 1 = long dead air, cut-offs, talking over the human.
- one_voice: 1 to 5, did it feel like one Molty (no "my agent says", no change of persona).
- worst: one short sentence naming the worst moment.
Answer as {"invented": int, "answered_right": int, "smooth": int, "one_voice": int, "worst": "..."}."""


def transcript(rows: list[dict], sheet: dict[str, dict]) -> str:
    """One conversation (one repeat), on the clock of the human's last word of each turn."""
    out = ["Fact sheet (the only true facts):"] + [f"- {k}: {v['say']}" for k, v in sheet.items()]
    out += ["", "Conversation. Times are seconds after the human stopped speaking in that turn."]
    for r in rows:
        if r.get("said") and r["i"] == 0:
            out.append(f"Molty: {r['said']!r}")
        out.append(f"[turn {r['id']}, label {r.get('label')}, truth {r.get('truth')}]")
        out.append(f"Human: {r.get('text') or '(silence)'!r}  (Molty heard: {r.get('heard')!r}; turn ended: {r.get('reason')}"
                   + ("; CUT OFF while still talking" if r.get("false_cutoff") else "") + ")")
        if r.get("tap_stop_ms") is not None:
            out.append(f"  (the human tapped Molty to interrupt; Molty kept talking {r['tap_stop_ms'] / 1000:.1f} s after the tap)")
        for v in r.get("voices") or []:
            out.append(f"  +{v['at_s']:.1f} s Molty ({v['kind']}): {v['text']!r}")
        if not any(v["kind"] == "answer" for v in r.get("voices") or []):
            out.append("  (Molty never answered this turn)")
    return "\n".join(out)


def prompt(rows: list[dict], sheet: dict[str, dict]) -> str:
    return f"{SYSTEM}\n\n{RUBRIC}\n\n{transcript(rows, sheet)}\n"


def parse(text: str) -> dict | None:
    for m in re.finditer(r"\{.*?\}", text, re.S):
        try:
            d = json.loads(m.group(0))
        except ValueError:
            continue
        if isinstance(d, dict) and "invented" in d:
            return d
    return None


def judge(cmd: str, rows: list[dict], sheet: dict[str, dict], votes: int = 3, cwd: Path | None = None, timeout: float = 300) -> dict:
    p = prompt(rows, sheet)
    got: list[dict] = []
    errors: list[str] = []
    for _ in range(votes):
        try:
            r = subprocess.run(shlex.split(cmd), input=p, capture_output=True, text=True, timeout=timeout, cwd=cwd)
        except (OSError, subprocess.TimeoutExpired) as e:
            errors.append(type(e).__name__)
            continue
        d = parse(r.stdout)
        if d is None:
            errors.append(f"exit {r.returncode}: no JSON")
        else:
            got.append(d)
    out: dict = {"votes": len(got), "errors": errors, "raw": got}
    for k in ("invented", "answered_right", "smooth", "one_voice"):
        v = [d[k] for d in got if isinstance(d.get(k), (int, float))]
        out[k] = statistics.median(v) if v else None
    out["worst"] = [d.get("worst") for d in got]
    return out


def merge(js: list[dict]) -> dict:
    """Several conversations' verdicts: totals of the counts, the median of the 1-5 scores."""
    js = [j for j in js if j and j.get("votes")]
    if not js:
        return {}
    num = lambda k: [j[k] for j in js if j.get(k) is not None]  # noqa: E731
    return {"votes": sum(j["votes"] for j in js), "conversations": len(js), "invented": sum(num("invented")),
            "answered_right": sum(num("answered_right")), "smooth": statistics.median(num("smooth")) if num("smooth") else None,
            "one_voice": statistics.median(num("one_voice")) if num("one_voice") else None}
