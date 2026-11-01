"""Molty's reflex through a hosted model, with the owner's own key, which stays on this box.

Same contract as reflex.ReflexModel: classify(text) -> the stt.reflex verdict. `load()` picks the
engine from the environment: TYPESAFE_API_KEY (Jev), else OPENAI_API_KEY. The relay never sees a
key. An engine that refuses the key rests for a while instead of being asked at every word.
"""

from __future__ import annotations

import json
import logging
import os
import time
import urllib.error
import urllib.request

from .reflex import CATEGORIES, PICKS, reflex_input

log = logging.getLogger("molty_voice")

JEV_URL = "https://api.typesafe.ai/v1/systemone"
JEV_MODEL = "jev-latest"
OPENAI_URL = "https://api.openai.com/v1/decisions"
OPENAI_MODEL = "gpt-6-luna"
TIMEOUT_S = 5.0
REST_S = 600.0

REFLEX_QUESTION = "Molty is a small creature on the desk, listening to its human, who is still talking: this is only what they said so far. Which visible reaction should Molty show right now?"
REFLEX_CRITERIA = {
    "surprised": "astonishing or unexpected news, disbelief, shock (no way, guess what, you won't believe)",
    "happy": "good news, joy, excitement, pride, gratitude, love, celebrating",
    "sad": "sad or bad news: loss, grief, illness, disappointment, loneliness, a bad day",
    "laugh": "a joke, a pun, something funny or silly, or the human laughing",
    "curious": "a question, or the human wondering about something",
    "none": "ordinary talk: instructions, facts, plans, small talk, or unclear / mixed feelings — no visible reaction",
}
CATEGORY_QUESTION = "What kind of thing is the human saying to Molty so far?"
CATEGORY_CRITERIA = {
    "question": "a simple question with a quick answer (a fact, the time, the weather, yes or no)",
    "request": "asks Molty to do something: set, remind, play, send, turn on, add, call, show, stop",
    "hard": "needs real thought: explain why or how, advice, a decision, a plan, a comparison, an opinion, or something emotionally heavy",
    "other": "statements, stories, feelings, small talk, thanks — nothing to answer or do yet",
}


def jev_body(text: str) -> dict:
    return {
        "model": JEV_MODEL,
        "state": {"human_said_so_far": text},
        "questions": {
            "reaction": {"type": "choice", "instructions": REFLEX_QUESTION, "criteria": REFLEX_CRITERIA},
            "category": {"type": "choice", "instructions": CATEGORY_QUESTION, "criteria": CATEGORY_CRITERIA},
        },
    }


def openai_body(text: str) -> dict:
    def choices(criteria: dict) -> list:
        return [{"value": k, "description": v} for k, v in criteria.items()]

    return {
        "model": OPENAI_MODEL,
        "input": f"The human, so far: {json.dumps(text)}",
        "questions": [
            {"type": "choice", "name": "reaction", "instructions": REFLEX_QUESTION, "choices": choices(REFLEX_CRITERIA)},
            {"type": "choice", "name": "category", "instructions": CATEGORY_QUESTION, "choices": choices(CATEGORY_CRITERIA)},
        ],
        "prompt_cache_key": "molty-reflex",
    }


def _probs(raw: object, allowed: tuple) -> dict:
    """{k: p} from either answer shape (a map, or a list of {value, probability}), unknown keys dropped."""
    if isinstance(raw, list):
        raw = {e.get("value"): e.get("probability") for e in raw if isinstance(e, dict)}
    if not isinstance(raw, dict):
        return {}
    return {k: round(v, 3) for k, v in raw.items() if k in allowed and isinstance(v, (int, float))}


def parse(engine: str, body: dict) -> dict:
    """The verdict's fields from an engine's answer; ValueError on a refusal or junk."""
    answers = body.get("answers")
    if engine == "jev":
        a, c = (answers.get("reaction"), answers.get("category")) if isinstance(answers, dict) else (None, None)
    else:
        by_name = {x.get("name"): x for x in answers if isinstance(x, dict)} if isinstance(answers, list) else {}
        a, c = by_name.get("reaction"), by_name.get("category")
    if not isinstance(a, dict) or a.get("choice") not in PICKS:
        raise ValueError("no reaction in the answer")
    out = {"pick": a["choice"], "conf": a.get("confidence", 0), "p": _probs(a.get("probabilities"), PICKS)}
    if isinstance(c, dict) and c.get("choice") in CATEGORIES:
        out.update(cat=c["choice"], cat_conf=c.get("confidence", 0))
    tokens = body.get("usage", {}).get("input_tokens") if isinstance(body.get("usage"), dict) else None
    if isinstance(tokens, int):
        out["tokens"] = tokens
    if isinstance(body.get("model"), str):
        out["model"] = body["model"][:40]
    return out


class HostedReflex:
    """classify() blocks on the network: call it off the event loop (asyncio.to_thread)."""

    def __init__(self, engine: str, key: str, post=None):
        self.engine = engine
        self._key = key
        self._post = post or self._http
        self._rest_until = 0.0

    def _http(self, url: str, body: dict) -> dict:
        req = urllib.request.Request(url, json.dumps(body).encode(), {"Content-Type": "application/json", "Authorization": f"Bearer {self._key}"})
        with urllib.request.urlopen(req, timeout=TIMEOUT_S) as res:
            return json.load(res)

    def classify(self, text: str) -> dict:
        now = time.monotonic()
        if now < self._rest_until:
            raise RuntimeError("resting after a refused call")
        t = reflex_input(text)
        url, body = (JEV_URL, jev_body(t)) if self.engine == "jev" else (OPENAI_URL, openai_body(t))
        t0 = time.perf_counter()
        try:
            answer = self._post(url, body)
        except urllib.error.HTTPError as e:
            if e.code in (400, 401, 403, 404):
                self._rest_until = now + REST_S
                log.warning("reflex: %s refused the call (HTTP %d): resting for %d s", self.engine, e.code, REST_S)
            raise RuntimeError(f"http_{e.code}") from None
        return {**parse(self.engine, answer), "hosted": True, "ms": round((time.perf_counter() - t0) * 1000, 1), "chars": len(t)}


def load(env=os.environ) -> HostedReflex | None:
    """The engine this box has a key for, or None."""
    if env.get("TYPESAFE_API_KEY"):
        return HostedReflex("jev", env["TYPESAFE_API_KEY"])
    if env.get("OPENAI_API_KEY"):
        return HostedReflex("openai", env["OPENAI_API_KEY"])
    return None
