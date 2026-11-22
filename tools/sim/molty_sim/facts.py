"""The scripted agent with a fact sheet (`--agent facts`): the pipeline is measured, not the agent.

It answers ONLY with lines of the scenario's `facts` sheet: the fact whose keys best match what
Molty heard (the words after the last correction such as "wait, actually" win), `unknown` when
none matches, `idle` when nothing was heard. Its think time per turn is drawn from a seeded
spread (median about 4 s, some about 12 s, some never), so every arm meets the same agent.

It drives the relay the way the skill says a real agent does (skills/molty/references/talking-fast.md):
one `talk` per turn; with `quick`, a quick phrase (`cmd quick`) before it thinks; with `keep`,
when the talk result carries first words (`opener`), `cmd keep {words}` up to the line's first
clause end at once, then a reply that starts with the kept words.
"""

from __future__ import annotations

import asyncio
import math
import random
import time

from .align import FILLERS, norm

THINK_MEDIAN_S = 4.0
THINK_SIGMA = 0.35  # log-normal spread around the median (about 2.3 to 7 s for most turns)
SLOW_S = (11.0, 13.0)
P_NEVER = 0.06
P_SLOW = 0.12
NEVER_WAIT_S = 20.0  # an agent that never answers: the human waits this long, then speaks again
CORRECTIONS = ("actually", "wait", "sorry", "rather", "instead")
QUICK_ID = "one_sec"


def think_s(seed: int, turn_id: str) -> float | None:
    """The agent's think time for this turn under this seed (None: it never answers)."""
    rng = random.Random(f"{seed}:{turn_id}:think")
    u = rng.random()
    if u < P_NEVER:
        return None
    if u < P_NEVER + P_SLOW:
        return round(rng.uniform(*SLOW_S), 2)
    return round(min(9.0, THINK_MEDIAN_S * math.exp(rng.gauss(0, THINK_SIGMA))), 2)


def tap_s(seed: int, turn_id: str, lo: float, hi: float) -> float:
    """When the human taps, in seconds after the say's first sample."""
    return round(random.Random(f"{seed}:{turn_id}:tap").uniform(lo, hi), 2)


def words(text: str) -> list[str]:
    return [w for w in (norm(t) for t in text.replace(",", " ").split()) if w and w not in FILLERS]


def clause_words(line: str) -> int:
    """Words up to the first clause end of a first-words line (the skill: prefer a clause end)."""
    ws = line.split()
    return next((i + 1 for i, w in enumerate(ws) if w[-1:] in ",.;:!?"), len(ws))


def pick(sheet: dict[str, dict], heard: str) -> str:
    """The fact id that answers `heard` (keys counted; multi-word keys as phrases)."""
    ws = words(heard)
    if not ws:
        return "idle"
    last = max((i for i, w in enumerate(ws) if w in CORRECTIONS), default=-1)
    if 0 <= last < len(ws) - 1:
        ws = ws[last + 1 :]
    text = f" {' '.join(ws)} "
    best, score = "unknown", 0
    for fid, f in sheet.items():
        if fid in ("unknown", "idle"):
            continue
        n = sum(1 for k in f["keys"] if f" {' '.join(words(k))} " in text)
        if n > score:
            best, score = fid, n
    return best


class FactAgent:
    name = "facts"

    def __init__(self, sheet: dict[str, dict], rest=None, seed: int = 0, quick: bool = False, keep: bool = False,
                 never_wait_s: float = NEVER_WAIT_S, sleep=asyncio.sleep):
        self.sheet, self.rest, self.seed = sheet, rest, seed
        self.quick, self.keep, self.never_wait_s, self.sleep = quick, keep, never_wait_s, sleep
        self.last: dict | None = None  # what it did for the latest turn (the report's `agent` column)

    async def _cmd(self, name: str, args: dict) -> dict:
        _, out = await self.rest.call("POST", f"/v1/devices/{self.rest.device}/cmd", {"name": name, "args": args})
        return out.get("result") or {"error": out.get("error")}

    async def reply(self, scripted: str | None, heard: str, history: list[dict]) -> str | None:
        prev = history[-1] if history else {}
        turn, res = prev.get("turn"), prev.get("res") or {}
        think = think_s(self.seed, turn.id if turn else "closing")
        d: dict = {"turn": turn.id if turn else None, "think_s": think, "never": think is None, "pick": None, "text": None,
                   "replied_at": None, "quick": None, "keep": None}
        self.last = d
        kept = ""
        if self.keep and res.get("opener"):  # first words: keep up to the first clause end, before any thinking
            k = await self._cmd("keep", {"words": clause_words(res["opener"])})
            d["keep"] = {x: k.get(x) for x in ("keep", "late", "said", "error")}
            kept = k.get("said") or ""
        elif self.quick and heard.strip() and (think is None or think > 1.0):
            q = await self._cmd("quick", {"id": QUICK_ID})
            d["quick"] = q.get("text") or q.get("skipped") or q.get("error")
        if think is None:
            await self.sleep(self.never_wait_s)
            return None
        await self.sleep(think)
        d["pick"] = pick(self.sheet, heard)
        say = self.sheet[d["pick"]]["say"]
        d["text"] = f"{kept} {say}".strip() if kept else say
        d["answer"] = say
        d["replied_at"] = time.monotonic()
        return d["text"]
