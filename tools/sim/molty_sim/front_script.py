"""`molty-sim front --script <yaml>`: a scripted conversation through the voice front, and its checks.

A script (tools/sim/scenarios/front-*.yaml) is a list of the person's turns, each with what they say
(the fake provider's transcript; a WAV for --live), how they say it (a pause, its length, voiced or
not), what the fake provider's front does with it (answers itself, or hands off with a holding line),
and what a fake AGENT that behaves like the real one answers (every utterance, seconds later, a chatty
line in_reply_to that turn, sometimes twice). The checks read the relay's activity log and the sim
device (both live and fake) and, with the fake provider, its own log:

  one_reply      every turn of the person gets exactly one front response (its answer, or a holding line)
  agent_waiting  an agent line is voiced only for the turn the front handed it (front.waiting)
  never_twice    no agent line is ever spoken twice
  not_over_them  no response is created while the person speaks (fake provider only)
  not_cut        the conversation never ends while a reply is being made, nor while the front waits for an
                 answer that comes
  first_audio    Molty's first sound after each turn within the bound
  no_session     no session is opened for a line that is only context (the fake agent always replies to a turn)
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import yaml

from .voice import Clip, read_wav, resample

RATE = 16000


@dataclass
class Turn:
    heard: str  # what the person says (the fake provider's transcript)
    front: str = "answer"  # answer | handoff: what the fake provider's front does
    said: str = ""  # the front's own words for it (a holding line for a handoff)
    pause_s: float = 0.6  # silence before the person speaks (from the mic's opening)
    speech_s: float | None = None  # how long they speak (default: by the words)
    voiced: bool = True  # False: breath / noise the model may take for speech, with no pitch
    wav: str | None = None  # --live: the recording
    auto_create_ms: int | None = None  # the fake provider: its own auto response this late (the run 5 race)
    agent: list[tuple[float, str]] = field(default_factory=list)  # (seconds after the utterance, line), in_reply_to this turn


@dataclass
class Script:
    name: str
    turns: list[Turn]
    first_audio_ms: int = 2500


def load(path: str | Path) -> Script:
    d = yaml.safe_load(Path(path).read_text())
    turns = []
    for t in d["turns"]:
        agent = t.get("agent") or []
        agent = [agent] if isinstance(agent, dict) else agent
        turns.append(Turn(heard=t["heard"], front=t.get("front", "answer"), said=t.get("said", ""), pause_s=float(t.get("pause_s", 0.6)),
                          speech_s=t.get("speech_s"), voiced=bool(t.get("voiced", True)), wav=t.get("wav"), auto_create_ms=t.get("auto_create_ms"),
                          agent=[(float(a["after_s"]), str(a["say"])) for a in agent]))
    return Script(name=d.get("name", Path(path).stem), turns=turns, first_audio_ms=int(d.get("first_audio_ms", 2500)))


def fake_script(s: Script) -> list[dict]:
    """The fake provider's turns: {heard, said, handoff, auto_create_ms?}."""
    out = []
    for t in s.turns:
        said = t.said or (f"Ok, let me see... {t.heard.rstrip('?')}, one sec." if t.front == "handoff" else "Okay!")
        out.append({"heard": t.heard, "said": said, "handoff": t.front == "handoff", **({"auto_create_ms": t.auto_create_ms} if t.auto_create_ms is not None else {})})
    return out


def clip(t: Turn, seed: int) -> Clip:
    """The turn on the mic: a pause, then speech (a voiced, syllable shaped buzz; or pitchless noise), then quiet."""
    if t.wav:
        x, rate = read_wav(Path(t.wav))
        x = resample(x, rate, RATE) if rate != RATE else x
        return Clip(pcm=np.concatenate([np.zeros(int(t.pause_s * RATE), np.float32), x]).astype(np.float32), text=t.heard, seed=seed)
    sec = t.speech_s or max(0.6, 0.32 * len(t.heard.split()))
    n = int(sec * RATE)
    tt = np.arange(n) / RATE
    env = 0.5 + 0.5 * np.sin(2 * np.pi * 4 * tt)
    if t.voiced:
        x = 0.25 * env * np.sign(np.sin(2 * np.pi * 140 * tt)) * (0.6 + 0.4 * np.sin(2 * np.pi * 3 * tt))
    else:
        x = 0.12 * env * np.random.default_rng(seed).standard_normal(n)
    pcm = np.concatenate([np.zeros(int(t.pause_s * RATE)), x, np.zeros(int(0.3 * RATE))]).astype(np.float32)
    return Clip(pcm=pcm, text=t.heard, speech=[(t.pause_s, t.pause_s + sec)], seed=seed)


def _norm(s: str) -> str:
    return re.sub(r"[^a-z0-9 ]+", "", re.sub(r"\s+", " ", s.lower().replace("’", "'"))).strip()


def check(script: Script, entries: list[dict], fake_log: list[dict] | None = None) -> tuple[list[dict], list[str]]:
    """The per-turn rows and the violations, from the relay's activity log (any order) and, with the fake provider, its log."""
    entries = sorted(entries, key=lambda e: e.get("ts", 0))
    turns = [e for e in entries if e.get("kind") == "trace" and e.get("ev") == "front"]
    utter = {e.get("job"): e["data"].get("text", "") for e in entries if e.get("kind") == "device" and e.get("ev") == "utterance"}
    jobs = [e for e in entries if e.get("kind") == "job" and e.get("op") in ("say", "talk")]
    replies = [e for e in jobs if not e.get("agent") and e.get("ok") and "created" in ((e["data"].get("timings") or {}).get("front") or {})]
    voiced = [e for e in jobs if e.get("agent") and e.get("ok") and e["data"].get("said")]
    calls = [e for e in entries if e.get("kind") == "call" and e.get("op") in ("say", "talk")]
    bad: list[str] = []
    rows: list[dict] = []

    def turn_of(ts: float) -> int | None:
        k = None
        for i, t in enumerate(turns):
            if t["ts"] <= ts:
                k = i
        return k

    by_turn: dict[int, list[dict]] = {}
    for r in replies:
        k = turn_of(r["ts"])
        if k is not None:
            by_turn.setdefault(k, []).append(r)
    for i, t in enumerate(turns):
        rs = by_turn.get(i, [])
        waiting = bool(t["data"].get("waiting"))
        fa = t["data"].get("reply_first_audio")
        agents = [v for v in voiced if turn_of(v["ts"]) == i]
        rows.append({"turn": i + 1, "heard": utter.get(t.get("job"), ""), "state": ("handed off" if waiting else "answered") if rs else "NO REPLY",
                     "said": " / ".join([r["data"].get("said", "") for r in rs] + [v["data"].get("said", "") for v in agents]),
                     "first_audio_ms": fa, "handoff": waiting})
        if len(rs) != 1:
            bad.append(f"one_reply: turn {i + 1} got {len(rs)} front responses")
        if fa is None or fa > script.first_audio_ms:
            bad.append(f"first_audio: turn {i + 1}: {fa} ms (bound {script.first_audio_ms})")
    seen_waiting: set[int] = set()
    for v in voiced:
        k = turn_of(v["ts"])
        if k is None or not turns[k]["data"].get("waiting") or k in seen_waiting:
            bad.append(f"agent_waiting: the agent's line {v['data'].get('said', '')[:50]!r} was voiced for turn {None if k is None else k + 1}, which the front did not hand off (or had answered already)")
        if k is not None:
            seen_waiting.add(k)
    spoken = [_norm(j["data"]["said"]) for j in jobs if j.get("ok") and j["data"].get("said")]  # everything Molty said, in full
    for line in {a for t in script.turns for _, a in t.agent}:
        core = _norm(line)[:40]
        n = sum(1 for x in spoken if core and core in x)
        if n > 1:
            bad.append(f"never_twice: {line[:50]!r} was spoken {n} times")
    for end in (e for e in entries if e.get("kind") == "voice_box" and e.get("ev") == "front.end"):
        k = turn_of(end["ts"])
        if k is None:
            continue
        if not any(r["ts"] <= end["ts"] for r in by_turn.get(k, [])):
            bad.append(f"not_cut: the conversation ended ({end['data'].get('reason')}) while turn {k + 1}'s reply was being made")
        job = turns[k].get("job")
        if turns[k]["data"].get("waiting") and any(c["ts"] > end["ts"] and (c.get("args") or {}).get("in_reply_to") == job for c in calls):
            bad.append(f"not_cut: the conversation ended ({end['data'].get('reason')}) while the front waited for turn {k + 1}'s answer")
    starts = [e["ts"] for e in entries if e.get("kind") == "voice_box" and e.get("ev") == "front.start"]
    for o in (e for e in entries if e.get("kind") == "voice_box" and e.get("ev") == "front.session" and e["data"].get("state") == "open"):
        if not any(-1500 <= o["ts"] - s <= 4000 for s in starts):
            bad.append("no_session: a box session was opened with no conversation starting (for a line that is only context)")
    if fake_log is not None:
        for e in fake_log:
            if e.get("ev") == "create" and e.get("speaking"):
                bad.append(f"not_over_them: a {e.get('kind')} response was created while the person spoke")
            if e.get("ev") == "close" and e.get("active"):
                bad.append("not_cut: the provider session closed with a response in flight")
    return rows, bad


def table(rows: list[dict]) -> str:
    out = [f"{'#':>2}  {'heard':28}  {'state':10}  {'first audio':>11}  {'hand-off':8}  said"]
    for r in rows:
        fa = f"{r['first_audio_ms']} ms" if r["first_audio_ms"] is not None else "-"
        out.append(f"{r['turn']:>2}  {r['heard'][:28]:28}  {r['state']:10}  {fa:>11}  {'yes' if r['handoff'] else 'no':8}  {r['said'][:90]}")
    return "\n".join(out)
