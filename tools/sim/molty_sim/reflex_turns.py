"""molty-sim reflex-turns: Molty's reflex over whole spoken turns (scenarios/reflex-turns.yaml).

Each turn is spoken in real time, one partial per word, through the relay's own ReflexSession
(services/relay/tools/reflex-turns.ts, Node type stripping) with the room's fire gate emulated, and
the listen closes `end_ms` after the last word. Configs compare cadence / gates / liveliness:

  label=engine[:liveliness][@relay_dir]   e.g. before=jev@/tmp/main/services/relay  after=jev:lively

(`@relay_dir` runs another checkout's relay, e.g. main's, for a before / after table.) Columns, by
turn kind (react / explain / neutral): fires per turn, turns with a reaction, time from the first
word to the first reaction (median), decisions that landed after the turn closed, engine calls per
turn, false fires on neutral turns, and the longest stretch of an explain turn with no reaction.
"""

from __future__ import annotations

import json
import os
import statistics
import subprocess
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path

import yaml

SIM_DIR = Path(__file__).resolve().parent.parent
REPO = SIM_DIR.parent.parent
RELAY_DIR = REPO / "services" / "relay"
DEFAULT_SET = SIM_DIR / "scenarios" / "reflex-turns.yaml"
KINDS = ("react", "explain", "neutral")
KEY_ENV = {"jev": "TYPESAFE_API_KEY", "openai": "OPENAI_API_KEY"}


@dataclass
class Turn:
    id: str
    kind: str
    text: str
    wpm: float
    end_ms: int


@dataclass
class Config:
    label: str
    engine: str
    liveliness: str | None = None
    relay_dir: Path = RELAY_DIR


def load(path: str | Path = DEFAULT_SET) -> list[Turn]:
    d = yaml.safe_load(Path(path).read_text())
    wpm, end_ms = float(d.get("wpm", 150)), int(d.get("end_ms", 600))
    turns = [Turn(str(t["id"]), str(t["kind"]), str(t["text"]), float(t.get("wpm", wpm)), int(t.get("end_ms", end_ms))) for t in d["turns"]]
    for t in turns:
        if t.kind not in KINDS:
            raise ValueError(f"bad kind: {t}")
    return turns


def parse_config(s: str) -> Config:
    label, _, rest = s.partition("=")
    if not rest:
        raise ValueError(f"config {s!r}: label=engine[:liveliness][@relay_dir]")
    rest, _, d = rest.partition("@")
    engine, _, lv = rest.partition(":")
    return Config(label, engine, lv or None, Path(d) if d else RELAY_DIR)


def run_config(c: Config, turns: list[Turn]) -> list[dict]:
    lines = "".join(json.dumps({"id": t.id, "kind": t.kind, "text": t.text, "wpm": t.wpm, "end_ms": t.end_ms}) + "\n" for t in turns)
    tool = RELAY_DIR / "tools" / "reflex-turns.ts"
    # the tool imports ../src/core/reflex.ts next to it: copy it into the other checkout when asked
    if c.relay_dir != RELAY_DIR:
        (c.relay_dir / "tools" / "reflex-turns.ts").write_text(tool.read_text())
    cmd = ["node", "--experimental-strip-types", "--no-warnings", "--import", "./tools/ts-resolve.mjs", "tools/reflex-turns.ts", c.engine] + ([c.liveliness] if c.liveliness else [])
    out = subprocess.run(cmd, cwd=c.relay_dir, input=lines, capture_output=True, text=True, check=True, timeout=1800)
    return [json.loads(x) for x in out.stdout.splitlines() if x.strip()]


@dataclass
class Row:
    label: str
    react_fires: float
    react_hit: str
    first_ms: float | None
    closed: int
    explain_fires: float
    explain_gap_s: float | None
    neutral_false: str
    calls: float


def _main_decisions(r: dict) -> list[dict]:
    # the engine's decisions (a fallback's keywords answer carries the fire)
    return [d for d in r["decisions"] if d.get("fired") is not None or d.get("err")]


def summarize(label: str, results: list[dict]) -> Row:
    by = {k: [r for r in results if r["kind"] == k] for k in KINDS}
    react, explain, neutral = by["react"], by["explain"], by["neutral"]
    firsts = [r["fires"][0]["at"] for r in react if r["fires"]]
    gaps = []
    for r in explain:
        marks = [0] + [f["at"] for f in r["fires"]] + [r["speech_ms"]]
        gaps.append(max(b - a for a, b in zip(marks, marks[1:])) / 1000)
    closed = sum(1 for r in results for d in r["decisions"] if d.get("fired") == "closed")
    calls = [len([d for d in r["decisions"] if d.get("fired") is not None]) for r in results]
    return Row(
        label,
        statistics.mean(len(r["fires"]) for r in react) if react else 0.0,
        f"{sum(1 for r in react if r['fires'])}/{len(react)}",
        statistics.median(firsts) if firsts else None,
        closed,
        statistics.mean(len(r["fires"]) for r in explain) if explain else 0.0,
        statistics.mean(gaps) if gaps else None,
        f"{sum(len(r['fires']) for r in neutral)} on {sum(1 for r in neutral if r['fires'])}/{len(neutral)}",
        statistics.mean(calls) if calls else 0.0,
    )


def table(rows: list[Row]) -> str:
    out = [
        "| config | react: fires/turn | react: turns with a reaction | first reaction (median ms from first word) | decided after the turn closed | explain: fires/turn | explain: longest still stretch (s) | neutral: false fires | decisions/turn |",
        "|---|---|---|---|---|---|---|---|---|",
    ]
    for r in rows:
        out.append(
            f"| {r.label} | {r.react_fires:.1f} | {r.react_hit} | {'—' if r.first_ms is None else round(r.first_ms)} | {r.closed} | "
            f"{r.explain_fires:.1f} | {'—' if r.explain_gap_s is None else f'{r.explain_gap_s:.1f}'} | {r.neutral_false} | {r.calls:.1f} |"
        )
    return "\n".join(out)


def detail(label: str, results: list[dict]) -> str:
    lines = [f"{label}:"]
    for r in results:
        fires = ", ".join(f"{f['pick']}{'~' if f.get('soft') else ''}@{f['at']}" for f in r["fires"]) or "-"
        lines.append(f"  {r['kind']:7s} {r['id']:10s} {r['words']:3d}w {r['speech_ms']:6d}ms  {fires}")
    return "\n".join(lines)


def main(configs: list[str], set_path: str, show_detail: bool, out_json: str | None) -> int:
    turns = load(set_path)
    cs = [parse_config(s) for s in configs]
    for c in cs:
        if c.engine in KEY_ENV and not os.environ.get(KEY_ENV[c.engine]):
            print(f"{c.label}: {KEY_ENV[c.engine]} not set")
            return 3
    with ThreadPoolExecutor(max_workers=len(cs)) as ex:
        results = list(ex.map(lambda c: run_config(c, turns), cs))
    rows = [summarize(c.label, r) for c, r in zip(cs, results)]
    print(f"{len(turns)} turns ({set_path})\n")
    print(table(rows))
    if show_detail:
        for c, r in zip(cs, results):
            print()
            print(detail(c.label, r))
    if out_json:
        Path(out_json).write_text(json.dumps({c.label: r for c, r in zip(cs, results)}, indent=1))
    return 0
