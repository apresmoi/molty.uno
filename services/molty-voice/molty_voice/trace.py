"""Turn trace of one streaming transcription (docs/AGENT-LINK.md "Turn trace"), and listen hints.

The bridge hands an `SttTrace` to its `SttSession` (`observer=`): the session calls it on every
Kyutai Word / EndWord / Step it processes (never on the flush after `stt.end`). It records

- `act`     the model's own speech-activity spans (prs[2] below 0.3 for 3 steps in a row starts
            one, at the first of those steps; prs[2] back at or above 0.3 ends it), audio seconds;
- `pause`   the pause EMA (PausePredictor.value) after every step, sent downsampled to <= 300
            bytes (max over each group of `pause_every` steps, 1 byte = round(ema * 255), hex);
- `words`   each Word with its start_time and the EndWord's stop_time (audio s), the last 64;
- `end`     the end-of-turn decision: the step it fired at, which rule fired it (`punct`: the last
            word ends a sentence, `hold`: a mid-sentence pause outlasted the hold, `unmute`: hold 0,
            Unmute's own rule), the word judged, the hold that elapsed and the EMA;
- `ns`      the no-speech decision: when, how long it had been quiet, the EMA, why.

Hints (`stt.hint`, the relay forwards them to Molty for its listener reactions): `pause` on the
first step of a mid-turn pause (the EMA crosses the threshold after >= 1 word, and that step did
not end the turn), `question` when a Word ending in `?` arrives. At most one hint per
HINT_GAP_S of audio per stream. Nothing here changes a decision: it only reads PausePredictor.
"""

from __future__ import annotations

import json
import math

from .kyutai import FRAME_S, PausePredictor, sentence_complete

HINT_GAP_S = 1.5
MAX_WORDS = 64
MAX_WORD_CHARS = 24
MAX_SPANS = 32
MAX_PAUSE_BYTES = 300
# the box part of a turn trace stays under this (the relay caps the whole trace at 4 KB): the
# oldest words, then the oldest activity spans, go first
BUDGET_BYTES = 3000


def _r(x: float) -> float:
    return round(float(x), 2)


class SttTrace:
    """Observer of one SttSession (see the module doc). `to_json()` is the box part of the turn trace."""

    def __init__(self, threshold: float = 0.6, skip_steps: int = PausePredictor.SKIP_STEPS, hold_s: float = 1.2, step_s: float = FRAME_S):
        self.thr, self.skip, self.hold_s, self.step_s = threshold, int(skip_steps), float(hold_s), step_s
        self.steps = 0
        self.ema: list[int] = []
        self.spans: list[list[float | None]] = []  # [start, end|None]
        self._run = 0
        self._active = False
        self.words: list[list] = []  # [text, start, stop|None]
        self.words_n = 0
        self.end: dict | None = None
        self.ns: dict | None = None
        self._prev_ema = 1.0
        self._last_hint = -math.inf

    # ---------------- what the session reports ----------------

    def now(self) -> float:
        """Audio seconds the model has processed (its steps)."""
        return self.steps * self.step_s

    def word(self, text: str, start: object = None) -> str | None:
        """A Word arrived. Returns "question" when it ends a question (and the hint gate allows)."""
        self.words_n += 1
        s = float(start) if isinstance(start, (int, float)) and not isinstance(start, bool) and math.isfinite(start) else None
        self.words.append([str(text)[:MAX_WORD_CHARS], _r(s) if s is not None else None, None])
        if len(self.words) > MAX_WORDS:
            del self.words[0]
        w = str(text).strip().rstrip("\"')»”’)]")
        if w.endswith("?") and sentence_complete(w):
            return self._hint("question")
        return None

    def end_word(self, stop: object = None) -> None:
        if self.words and isinstance(stop, (int, float)) and not isinstance(stop, bool) and math.isfinite(stop):
            self.words[-1][2] = _r(stop)

    def step(self, p: PausePredictor, prs2: float, turn_end: bool = False, no_speech: bool = False) -> str | None:
        """After PausePredictor.update() (and no_speech()) for one Step. Returns "pause" for a pause hint."""
        self.steps += 1
        t0 = (self.steps - 1) * self.step_s
        self.ema.append(max(0, min(255, round(p.value * 255))))
        hint = None
        if self.steps > self.skip:
            v = max(0.0, float(prs2))
            if v < PausePredictor.ACTIVE_BELOW:
                self._run += 1
                if self._run == PausePredictor.ACTIVE_STEPS and not self._active:
                    self._active = True
                    self.spans.append([_r(t0 - (PausePredictor.ACTIVE_STEPS - 1) * self.step_s), None])
            else:
                self._run = 0
                if self._active:
                    self._active = False
                    self.spans[-1][1] = _r(t0)
            if len(self.spans) > MAX_SPANS:
                del self.spans[0]
            if turn_end and self.end is None:
                self.end = {"at": _r(self.now()), "rule": self.rule(p), "word": p.last_word[:MAX_WORD_CHARS],
                            "held_s": _r(p.held * self.step_s), "ema": _r(p.value)}
            elif (not turn_end and not p.fired and p.words > 0 and self.thr > 0
                  and p.value > self.thr >= self._prev_ema):
                hint = self._hint("pause")
            if no_speech and self.ns is None:
                quiet = (p.steps - p.quiet_since) * self.step_s
                self.ns = {"at": _r(self.now()), "quiet_s": _r(quiet), "ema": _r(p.value),
                           "why": f"no word, no voice activity for {quiet:.1f} s"}
        self._prev_ema = p.value
        return hint

    @staticmethod
    def rule(p: PausePredictor) -> str:
        """Why PausePredictor fired (read right after update() returned True)."""
        if p.hold_s <= 0:
            return "unmute"
        return "punct" if sentence_complete(p.last_word) else "hold"

    def _hint(self, kind: str) -> str | None:
        t = self.now()
        if t - self._last_hint < HINT_GAP_S - 1e-9:
            return None
        self._last_hint = t
        return kind

    # ---------------- the trace ----------------

    def to_json(self, final_ms: int | None = None) -> dict:
        n = len(self.ema)
        every = max(1, math.ceil(n / MAX_PAUSE_BYTES)) if n else 1
        pause = "".join(f"{max(self.ema[i : i + every]):02x}" for i in range(0, n, every))
        out: dict = {"v": 1, "step_ms": round(self.step_s * 1000), "skip": self.skip, "thr": self.thr, "hold_s": self.hold_s,
                     "audio_s": _r(self.now()),
                     "act": [[a, b if b is not None else _r(self.now())] for a, b in self.spans],
                     "pause": pause, "pause_every": every,
                     "words": [list(w) for w in self.words]}
        if self.words_n > len(self.words):
            out["words_n"] = self.words_n
        if self.end:
            out["end"] = dict(self.end)
        if self.ns:
            out["ns"] = dict(self.ns)
        if final_ms is not None:
            out["final_ms"] = int(final_ms)
        size = lambda: len(json.dumps(out, separators=(",", ":")))  # noqa: E731
        while size() > BUDGET_BYTES and (out["words"] or out["act"]):
            if out["words"]:
                out["words"].pop(0)
                out["words_n"] = self.words_n
            else:
                out["act"].pop(0)
        return out
