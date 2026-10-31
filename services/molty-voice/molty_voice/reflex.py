"""Molty's reflex on the box: a small emotion classifier over the human's words while they talk.

The relay asks for it per listen (`stt.start` with `reflex: true`, docs/AGENT-LINK.md "Reflex").
At each reflex chunk of the text so far — the same rule as the relay's core/reflex.ts reflexDue —
the box classifies the text and sends `{"t":"stt.reflex","id","n","pick","conf","p","ms","chars","tokens","model"}`
right after that word's `stt.partial`: no extra network hop, the box already has the words.

Model: SamLowe/roberta-base-go_emotions-onnx (MIT; RoBERTa fine-tuned on GoEmotions, int8 ONNX,
~125 MB), on the CPU through onnxruntime (~35 ms a chunk with 4 threads on the 4090 box) — nothing
on the GPU next to Kyutai. Its 28 labels fold into Molty's reactions (GO_MAP). Optional deps:
`pip install 'molty-voice[reflex]'`; without them the box simply does not offer the reflex.

`python -m molty_voice.reflex bench` reads JSON lines `{"text": ...}` on stdin and writes one verdict
per line (tools/sim reflex-bench runs it on the box).
"""

from __future__ import annotations

import json
import logging
import re
import sys
import time

log = logging.getLogger("molty_voice")

EVERY_WORDS = 6
MIN_WORDS = 2
PICKS = ("surprised", "happy", "sad", "laugh", "curious", "none")
DEFAULT_MODEL = "SamLowe/roberta-base-go_emotions-onnx"
MAX_CHARS = 400

# GoEmotions labels -> Molty's reactions (a reaction's score: the max of its labels' sigmoids).
GO_MAP = {
    "surprised": ("surprise", "realization"),
    "happy": ("joy", "excitement", "gratitude", "love", "admiration", "optimism", "pride", "relief"),
    "sad": ("sadness", "grief", "disappointment", "remorse"),
    "laugh": ("amusement",),
    "curious": ("curiosity", "confusion"),
    "none": ("neutral",),
}

CATEGORIES = ("question", "request", "hard", "other")
# the relay's keywordCategory rules (core/reflex.ts), word for word
_HARD = re.compile(r"\b(explain|why (is|are|do|does|did|would|should)|how (does|do|did|come|would|could)|what('s| is) the difference|difference between|compare|pros and cons|should i|what should|what would you|do you think|your opinion|advice|help me (decide|figure|understand|plan|think)|plan (my|a|the)|i don't know what to do|meaning of)\b", re.I)
_REQUEST = re.compile(r"^(please |hey molty,? |molty,? )?(set|remind|play|turn|tell me|show|send|call|add|make|find|open|start|stop|cancel|book|order|text|email|write|read|put|schedule|pause|skip|check)\b|\b(can you|could you|would you|will you|please|i need you to|i want you to)\b", re.I)
_QUESTION = re.compile(r"^(what|why|how|when|where|who|which|can you|could you|do you|did you|are you|is it|is there|will you|would you|should i|have you)\b", re.I)
_QMARK = re.compile(r"\?[\"')\]]*\s*$")


def rule_category(text: str) -> str:
    t = reflex_input(text)
    parts = [x for x in re.split(r"(?<=[.?!])\s+", t) if x]
    last = (parts[-1] if parts else t).strip()
    if _HARD.search(last):
        return "hard"
    if _REQUEST.search(last):
        return "request"
    if _QMARK.search(last) or _QUESTION.search(last):
        return "question"
    return "other"


def category(text: str, label_probs: dict[str, float]) -> tuple[str, float]:
    """Local category: the rules, and the classifier's curiosity / confusion for a question the rules missed."""
    cat = rule_category(text)
    if cat != "other":
        return cat, 1.0
    q = max(label_probs.get("curiosity", 0.0), label_probs.get("confusion", 0.0))
    return ("question", round(q, 3)) if q >= 0.5 else ("other", round(1 - q, 3))


_SENT_END = re.compile(r"[.?!][\"')\]]*$")
_ELLIPSIS = re.compile(r"(\.\.\.|…)[\"')\]]*$")


def sentence_end(text: str) -> bool:
    t = text.strip()
    return not _ELLIPSIS.search(t) and bool(_SENT_END.search(t))


def reflex_due(prev_n: int, text: str) -> int | None:
    """The chunk's word count when one is due (same rule as the relay's reflexDue), else None."""
    n = len(text.split())
    if n < MIN_WORDS:
        return None
    added = n - prev_n
    if added >= EVERY_WORDS or (added >= 1 and sentence_end(text)):
        return n
    return None


def reflex_input(text: str) -> str:
    t = text.strip()
    if len(t) <= MAX_CHARS:
        return t
    cut = t[-MAX_CHARS:]
    sp = cut.find(" ")
    return cut[sp + 1:] if sp >= 0 else cut


def fold(label_probs: dict[str, float]) -> tuple[str, float, dict[str, float]]:
    """GoEmotions probabilities -> (pick, its score, a score per reaction)."""
    p = {k: max((label_probs.get(lab, 0.0) for lab in labs), default=0.0) for k, labs in GO_MAP.items()}
    pick = max(PICKS, key=lambda k: p[k])
    return pick, p[pick], {k: round(v, 2) for k, v in p.items()}


class ReflexModel:
    """The ONNX classifier. classify() is CPU work: call it off the event loop (asyncio.to_thread)."""

    def __init__(self, repo: str = DEFAULT_MODEL, threads: int = 4):
        import numpy as np
        import onnxruntime as ort
        from huggingface_hub import hf_hub_download
        from tokenizers import Tokenizer

        self._np = np
        self.repo = repo
        cfg = json.load(open(hf_hub_download(repo, "onnx/config.json")))
        self.labels = [cfg["id2label"][str(i)] for i in range(len(cfg["id2label"]))]
        self.tok = Tokenizer.from_file(hf_hub_download(repo, "onnx/tokenizer.json"))
        self.tok.enable_truncation(256)
        so = ort.SessionOptions()
        so.intra_op_num_threads = threads
        self.sess = ort.InferenceSession(hf_hub_download(repo, "onnx/model_quantized.onnx"), so, providers=["CPUExecutionProvider"])
        self.classify("warm up")

    def classify(self, text: str) -> dict:
        np = self._np
        t = reflex_input(text)
        t0 = time.perf_counter()
        e = self.tok.encode(t)
        logits = self.sess.run(None, {"input_ids": np.array([e.ids], np.int64), "attention_mask": np.array([e.attention_mask], np.int64)})[0][0]
        probs = 1.0 / (1.0 + np.exp(-logits))
        lp = {lab: float(probs[i]) for i, lab in enumerate(self.labels)}
        pick, conf, p = fold(lp)
        cat, cat_conf = category(t, lp)
        if cat == "request" and pick == "curious":
            # "can you set a timer?": a request, not curiosity (the relay's keywords rule too)
            pick, conf = "none", p["none"]
        return {"pick": pick, "conf": round(conf, 3), "p": p, "cat": cat, "cat_conf": cat_conf, "ms": round((time.perf_counter() - t0) * 1000, 1), "chars": len(t), "tokens": len(e.ids), "model": self.repo.split("/")[-1][:80]}


def load(repo: str = DEFAULT_MODEL, threads: int = 4) -> ReflexModel | None:
    """The model, or None (deps missing, download failed): the box then offers no reflex."""
    try:
        return ReflexModel(repo, threads)
    except Exception as e:  # noqa: BLE001 - optional feature: never stops the box
        log.warning("reflex model unavailable (%s: %s): the box offers no reflex — pip install 'molty-voice[reflex]'", type(e).__name__, e)
        return None


class ReflexTracker:
    """One listen's chunks: feed every text so far, get the chunk's n when one is due."""

    def __init__(self, max_chunks: int = 12):
        self.n = 0
        self.chunks = 0
        self.max = max_chunks

    def due(self, text: str) -> int | None:
        if self.chunks >= self.max:
            return None
        n = reflex_due(self.n, text)
        if n is None:
            return None
        self.n = n
        self.chunks += 1
        return n


def main(argv: list[str] | None = None) -> int:
    args = sys.argv[1:] if argv is None else argv
    if not args or args[0] != "bench":
        print("usage: python -m molty_voice.reflex bench  < lines of {\"text\": ...}", file=sys.stderr)
        return 2
    m = ReflexModel()
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        item = json.loads(line)
        out = m.classify(item["text"])
        print(json.dumps(out), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
