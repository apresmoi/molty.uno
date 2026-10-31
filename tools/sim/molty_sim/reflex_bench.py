"""molty-sim reflex-bench: Molty's reflex engines on labelled partial sentences (scenarios/reflex.yaml).

Each engine runs the code it runs in production, one sentence at a time:
  keywords / jev / openai  the relay's core/reflex.ts, via services/relay/tools/reflex-bench.ts (Node)
  local                    the box's molty_voice/reflex.py, over ssh on the voice box (CPU, its own venv)
and the table compares them: reaction accuracy (after the relay's gate at the chosen liveliness:
a confident pick in full, a mild one as its soft gesture — counted as that reaction), false fires on
neutral sentences, utterance-category accuracy, p50 / p90 latency, input tokens, and the cost of
1000 partials. Latency: remote engines include the network from this machine; local is the box's
inference only (its verdict rides the box's existing socket right after the partial: no extra hop).
"""

from __future__ import annotations

import json
import os
import shlex
import statistics
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

import yaml

SIM_DIR = Path(__file__).resolve().parent.parent
REPO = SIM_DIR.parent.parent
RELAY_DIR = REPO / "services" / "relay"
VOICE_DIR = REPO / "services" / "molty-voice"
DEFAULT_SET = SIM_DIR / "scenarios" / "reflex.yaml"

ENGINES = ("local", "jev", "openai", "keywords")
REACTIONS = ("surprised", "happy", "sad", "laugh", "curious", "none")
CATEGORIES = ("question", "request", "hard", "other")
# the relay's gate for the box's raw verdicts, per liveliness (core/reflex.ts REFLEX_LIVELINESS: gate.local
# and mild; tests/test_reflex_bench.py checks them against the source)
LIVELINESS = ("calm", "normal", "lively")
LOCAL_GATE = {"calm": 0.35, "normal": 0.3, "lively": 0.25}
MILD = {"calm": None, "normal": (0.6, 0.3), "lively": (0.75, 0.2)}
# $ per million input tokens (core/reflex.ts PRICE_PER_MTOK); local and keywords cost nothing per call
PRICE_PER_MTOK = {"jev": 0.042, "openai": 0.1}
KEY_ENV = {"jev": "TYPESAFE_API_KEY", "openai": "OPENAI_API_KEY"}


@dataclass
class Item:
    text: str
    reaction: str
    category: str


@dataclass
class Score:
    engine: str
    n: int = 0
    reaction_ok: int = 0
    category_ok: int = 0
    category_n: int = 0
    neutral: int = 0
    false_fires: int = 0
    errors: int = 0
    ms: list[float] = field(default_factory=list)
    tokens: list[int] = field(default_factory=list)
    misses: list[tuple[str, str, str]] = field(default_factory=list)

    @property
    def accuracy(self) -> float:
        return self.reaction_ok / self.n if self.n else 0.0

    @property
    def cat_accuracy(self) -> float | None:
        return self.category_ok / self.category_n if self.category_n else None

    def pct(self, q: float) -> float | None:
        if not self.ms:
            return None
        s = sorted(self.ms)
        return s[min(len(s) - 1, int(round(q * (len(s) - 1))))]

    @property
    def cost_per_1000(self) -> float:
        if self.engine not in PRICE_PER_MTOK or not self.tokens:
            return 0.0
        return statistics.mean(self.tokens) * PRICE_PER_MTOK[self.engine] / 1e6 * 1000


def load(path: str | Path = DEFAULT_SET) -> list[Item]:
    d = yaml.safe_load(Path(path).read_text())
    items = [Item(str(x["text"]), str(x["reaction"]), str(x["category"])) for x in d["items"]]
    for it in items:
        if it.reaction not in REACTIONS or it.category not in CATEGORIES:
            raise ValueError(f"bad label: {it}")
    return items


def gate_local(v: dict, liveliness: str = "normal") -> dict:
    """The relay's reflexGate on a box verdict: in full at the gate; below it a mild pick (soft) when
    `none` is unsure and a reaction plausible; else none. A request is never curiosity."""
    choice, conf, p = v.get("pick", "none"), float(v.get("conf", 0)), v.get("p") or {}
    base = {k: x for k, x in v.items() if k not in ("raw", "soft")}
    if choice != "none" and conf >= LOCAL_GATE[liveliness]:
        out = {**base, "pick": choice}
    else:
        alts = [k for k in REACTIONS if k != "none" and k in p]
        alt = max(alts, key=lambda k: p[k]) if alts else None
        mild = MILD[liveliness]
        if mild and alt and "none" in p and p["none"] < mild[0] and p[alt] >= mild[1]:
            out = {**base, "pick": alt, "soft": True, **({"raw": choice} if choice != alt else {})}
        else:
            out = {**base, "pick": "none", **({"raw": choice} if choice != "none" else {})}
    if out["pick"] == "curious" and v.get("cat") == "request":
        return {**base, "pick": "none", "raw": choice}
    return out


def score(engine: str, items: list[Item], verdicts: list[dict]) -> Score:
    s = Score(engine)
    for it, v in zip(items, verdicts):
        s.n += 1
        if v.get("error"):
            s.errors += 1
        pick = v.get("pick", "none")
        if pick == it.reaction:
            s.reaction_ok += 1
        else:
            s.misses.append((it.text, it.reaction, pick))
        if it.reaction == "none":
            s.neutral += 1
            if pick != "none":
                s.false_fires += 1
        if v.get("cat") is not None:
            s.category_n += 1
            s.category_ok += v["cat"] == it.category
        if isinstance(v.get("ms"), (int, float)):
            s.ms.append(float(v["ms"]))
        if isinstance(v.get("tokens"), int):
            s.tokens.append(v["tokens"])
    return s


def _lines(items: list[Item]) -> str:
    return "".join(json.dumps({"text": it.text}) + "\n" for it in items)


def run_relay_engine(engine: str, items: list[Item], liveliness: str = "normal") -> list[dict]:
    cmd = ["node", "--experimental-strip-types", "--no-warnings", "--import", "./tools/ts-resolve.mjs", "tools/reflex-bench.ts", engine, liveliness]
    out = subprocess.run(cmd, cwd=RELAY_DIR, input=_lines(items), capture_output=True, text=True, check=True, timeout=600)
    return [json.loads(x) for x in out.stdout.splitlines() if x.strip()]


def run_local(items: list[Item], box: str, lab: str = "~/molty-voice/reflex-lab", liveliness: str = "normal") -> list[dict]:
    """The box's classifier in its own venv (`lab`/.venv with molty-voice[reflex] deps): never the running service."""
    subprocess.run(["ssh", box, f"mkdir -p {lab}/src/molty_voice"], check=True, timeout=60)
    subprocess.run(["rsync", "-a", "--delete", "--exclude", "__pycache__", f"{VOICE_DIR / 'molty_voice'}/", f"{box}:{lab}/src/molty_voice/"], check=True, timeout=120)
    remote = f"cd {lab}/src && ../.venv/bin/python -m molty_voice.reflex bench"
    out = subprocess.run(["ssh", box, remote], input=_lines(items), capture_output=True, text=True, check=True, timeout=900)
    return [gate_local(json.loads(x), liveliness) for x in out.stdout.splitlines() if x.strip()]


def fmt_ms(x: float | None) -> str:
    return "—" if x is None else (f"{x:.1f}" if x < 10 else f"{x:.0f}")


def table(scores: list[Score], skipped: dict[str, str] | None = None) -> str:
    head = "| engine | reaction acc. | false fires (neutral) | category acc. | p50 ms | p90 ms | input tokens | $ / 1000 partials | errors |"
    rows = [head, "|---|---|---|---|---|---|---|---|---|"]
    for s in scores:
        cat = s.cat_accuracy
        rows.append(
            f"| {s.engine} | {s.reaction_ok}/{s.n} ({s.accuracy:.0%}) | {s.false_fires}/{s.neutral} | "
            f"{'—' if cat is None else f'{s.category_ok}/{s.category_n} ({cat:.0%})'} | {fmt_ms(s.pct(0.5))} | {fmt_ms(s.pct(0.9))} | "
            f"{round(statistics.mean(s.tokens)) if s.tokens else '—'} | {s.cost_per_1000:.4f} | {s.errors} |"
        )
    for e, why in (skipped or {}).items():
        rows.append(f"| {e} | skipped: {why} | | | | | | | |")
    return "\n".join(rows)


def bench(engines: list[str], items: list[Item], box: str, liveliness: str = "normal") -> tuple[list[Score], dict[str, str]]:
    scores, skipped = [], {}
    for e in engines:
        if e in KEY_ENV and not os.environ.get(KEY_ENV[e]):
            skipped[e] = f"{KEY_ENV[e]} not set"
            continue
        verdicts = run_local(items, box, liveliness=liveliness) if e == "local" else run_relay_engine(e, items, liveliness)
        if len(verdicts) != len(items):
            raise RuntimeError(f"{e}: {len(verdicts)} verdicts for {len(items)} items")
        scores.append(score(e, items, verdicts))
    return scores, skipped


def misses(scores: list[Score]) -> str:
    out = []
    for s in scores:
        out.append(f"{s.engine} misses:")
        out += [f"  {want:9s} -> {got:9s} {text}" for text, want, got in s.misses]
    return "\n".join(out)


def main(engines: list[str], set_path: str, box: str, show_misses: bool, out_json: str | None, liveliness: str = "normal") -> int:
    items = load(set_path)
    scores, skipped = bench(engines, items, box, liveliness)
    print(f"{len(items)} labelled partials ({shlex.quote(str(set_path))}), liveliness {liveliness}\n")
    print(table(scores, skipped))
    if show_misses:
        print()
        print(misses(scores))
    if out_json:
        Path(out_json).write_text(json.dumps({"items": len(items), "skipped": skipped, "scores": [
            {"engine": s.engine, "accuracy": s.accuracy, "category_accuracy": s.cat_accuracy, "false_fires": s.false_fires, "p50_ms": s.pct(0.5), "p90_ms": s.pct(0.9),
             "tokens": statistics.mean(s.tokens) if s.tokens else None, "usd_per_1000": s.cost_per_1000, "errors": s.errors, "misses": s.misses} for s in scores]}, indent=2))
    return 0
