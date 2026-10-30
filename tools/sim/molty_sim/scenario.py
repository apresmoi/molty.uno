"""Scenario files (YAML or JSON): a conversation of turns the simulated human plays.

    name: default
    defaults:            # every turn inherits these (a turn's own keys win)
      voices: [vctk/p225_023.wav, ...]   # round-robin over turns without `voice`
      lead_s: 0.6        # mic open -> the human starts speaking
      level_db: -24      # active-speech RMS of the human at the mic (dBFS)
      noise: {kind: pink, db: -62}       # room noise under everything (kind: pink|white|brown|none)
      listen_ms: null    # null = the device's own window setting; 0 = unlimited
    turns:
      - id: router
        say: "What are you working on?"          # Molty's line before it listens (the agent's reply)
        user: "I was thinking {pause 0.7} about the router, um {pause 0.5} and moving it."
        expect: turn_end                          # turn_end (default with speech) | no_speech
        tags: [thinking-pause, filler]
      - id: recorded
        wav: recordings/yes.wav                   # pre-recorded human (any rate, mono/stereo PCM16)
        text: "yes please"                         # its transcript, for WER
      - id: quiet
        silence_s: 10                              # nobody speaks (expect no_speech)
      - id: persona                                # reserved: an LLM writes the line (not implemented yet)
        user: {llm: {persona: "...", prompt: "..."}}
    closing: "Okay, talk soon."                    # Molty's last line after the last turn (optional)

User text markup: `{pause S}` inserts S seconds of silence at that point (between words, or
before the first word = a later start). Fillers are plain words ("um", "uh").
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import yaml

PAUSE_RE = re.compile(r"\{\s*pause\s+([0-9]*\.?[0-9]+)\s*\}")
TOKEN_RE = re.compile(r"\{[^}]*\}|[^\s{}]+")
EXPECTS = ("turn_end", "no_speech")
DEFAULT_VOICES = [
    "vctk/p225_023.wav",
    "expresso/ex01-ex02_default_001_channel1_168s.wav",
    "vctk/p226_023.wav",
    "expresso/ex03-ex01_calm_001_channel2_1081s.wav",
    "vctk/p228_023.wav",
    "expresso/ex03-ex01_confused_001_channel1_909s.wav",
    "vctk/p232_023.wav",
    "expresso/ex01-ex02_fast_001_channel1_104s.wav",
]


class ScenarioError(ValueError):
    pass


@dataclass(frozen=True)
class Markup:
    """Spoken words, and pauses: (index of the word they come before, seconds)."""

    words: tuple[str, ...]
    pauses: tuple[tuple[int, float], ...]

    @property
    def text(self) -> str:
        return " ".join(self.words)


def parse_markup(s: str) -> Markup:
    words: list[str] = []
    pauses: list[tuple[int, float]] = []
    for tok in TOKEN_RE.findall(s):
        if tok.startswith("{"):
            m = PAUSE_RE.fullmatch(tok)
            if not m:
                raise ScenarioError(f"unknown markup {tok!r} (only {{pause S}})")
            sec = float(m.group(1))
            if not 0 < sec <= 30:
                raise ScenarioError(f"pause {sec} s out of range (0, 30]")
            if pauses and pauses[-1][0] == len(words):
                pauses[-1] = (len(words), pauses[-1][1] + sec)  # two pauses in a row add up
            else:
                pauses.append((len(words), sec))
        else:
            words.append(tok)
    return Markup(tuple(words), tuple(pauses))


@dataclass
class Turn:
    id: str
    index: int
    say: str | None
    kind: str  # "tts" | "wav" | "silence" | "llm"
    markup: Markup | None = None
    wav: Path | None = None
    text: str = ""  # reference transcript (what the human says)
    silence_s: float = 0.0
    voice: str | None = None
    lead_s: float = 0.6
    level_db: float = -24.0
    noise: dict = field(default_factory=lambda: {"kind": "pink", "db": -62.0})
    listen_ms: int | None = None
    expect: str = "turn_end"
    tags: list[str] = field(default_factory=list)
    llm: dict | None = None


@dataclass
class Scenario:
    name: str
    turns: list[Turn]
    closing: str | None = None
    source: Path | None = None


TURN_KEYS = {"id", "say", "user", "wav", "text", "silence_s", "voice", "lead_s", "level_db", "noise", "listen_ms", "expect", "tags"}
DEFAULT_KEYS = {"voices", "lead_s", "level_db", "noise", "listen_ms", "voice"}


def _num(v: Any, what: str, lo: float, hi: float) -> float:
    if isinstance(v, bool) or not isinstance(v, (int, float)) or not lo <= v <= hi:
        raise ScenarioError(f"{what} must be a number in [{lo}, {hi}], got {v!r}")
    return float(v)


def parse(data: dict, source: Path | None = None) -> Scenario:
    if not isinstance(data, dict) or not isinstance(data.get("turns"), list) or not data["turns"]:
        raise ScenarioError("a scenario needs a non-empty `turns` list")
    d = data.get("defaults") or {}
    unknown = set(d) - DEFAULT_KEYS
    if unknown:
        raise ScenarioError(f"unknown defaults: {sorted(unknown)}")
    voices = d.get("voices") or DEFAULT_VOICES
    turns: list[Turn] = []
    seen: set[str] = set()
    for i, raw in enumerate(data["turns"]):
        if not isinstance(raw, dict):
            raise ScenarioError(f"turn {i}: must be a mapping")
        unknown = set(raw) - TURN_KEYS
        if unknown:
            raise ScenarioError(f"turn {i}: unknown keys {sorted(unknown)}")
        tid = raw.get("id", f"t{i + 1:02d}")
        if not isinstance(tid, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,40}", tid):
            raise ScenarioError(f"turn {i}: id must be a short string (quote YAML words like yes/no), got {tid!r}")
        if tid in seen:
            raise ScenarioError(f"duplicate turn id {tid!r}")
        seen.add(tid)
        g = lambda k, dv=None: raw.get(k, d.get(k, dv))  # noqa: E731
        t = Turn(
            id=tid,
            index=i,
            say=raw.get("say"),
            kind="tts",
            voice=raw.get("voice") or d.get("voice") or voices[i % len(voices)],
            lead_s=_num(g("lead_s", 0.6), f"{tid}.lead_s", 0, 30),
            level_db=_num(g("level_db", -24.0), f"{tid}.level_db", -60, 0),
            noise=dict(g("noise", {"kind": "pink", "db": -62.0}) or {"kind": "none"}),
            listen_ms=g("listen_ms"),
            tags=list(raw.get("tags") or []),
        )
        if t.listen_ms is not None:
            t.listen_ms = int(_num(t.listen_ms, f"{tid}.listen_ms", 0, 30000))
        if t.noise.get("kind", "pink") not in ("pink", "white", "brown", "none"):
            raise ScenarioError(f"{tid}: noise.kind must be pink|white|brown|none")
        sources = [k for k in ("user", "wav", "silence_s") if raw.get(k) is not None]
        if len(sources) != 1:
            raise ScenarioError(f"turn {tid}: exactly one of user / wav / silence_s")
        if "user" in sources:
            u = raw["user"]
            if isinstance(u, dict) and "llm" in u:
                t.kind, t.llm = "llm", dict(u["llm"])
            elif isinstance(u, str) and u.strip():
                t.markup = parse_markup(u)
                if not t.markup.words:
                    raise ScenarioError(f"turn {tid}: user text has no words")
                t.text = t.markup.text
            else:
                raise ScenarioError(f"turn {tid}: user must be text or {{llm: ...}}")
        elif "wav" in sources:
            t.kind = "wav"
            base = source.parent if source else Path.cwd()
            t.wav = (base / raw["wav"]).resolve()
            t.text = str(raw.get("text") or "")
        else:
            t.kind = "silence"
            t.silence_s = _num(raw["silence_s"], f"{tid}.silence_s", 0.1, 300)
        t.expect = raw.get("expect") or ("no_speech" if t.kind == "silence" else "turn_end")
        if t.expect not in EXPECTS:
            raise ScenarioError(f"turn {tid}: expect must be one of {EXPECTS}")
        turns.append(t)
    return Scenario(name=str(data.get("name") or (source.stem if source else "scenario")), turns=turns,
                    closing=data.get("closing"), source=source)


def load(path: str | Path) -> Scenario:
    p = Path(path)
    text = p.read_text()
    data = json.loads(text) if p.suffix == ".json" else yaml.safe_load(text)
    return parse(data, p.resolve())
