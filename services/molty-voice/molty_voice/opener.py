"""First words: a very small model starts Molty's reply before the agent can speak.

The relay asks at the end of the human's turn (`{"t":"opener","id","text","history":[{"role","text"}]}`)
and answers itself with what it may say: this module only writes a line. Whether the line may be
spoken is the relay's call (services/relay core/opener.ts guardOpener), whichever model wrote it.

The line is the real start of the answer (12 to 20 words in short clauses): the agent reads it
and takes over at one of its words (the relay's planReply).

Model: `gemma3:4b` on the box's Ollama, loaded and kept loaded (about 350 ms for a line on the box,
2026-11-05; OPENER-SAMPLES.md). `python -m molty_voice.opener` writes one line per JSON line
`{"text","history"}` on stdin (the eval's way in).
"""

from __future__ import annotations

import json
import logging
import re
import sys
import time
import urllib.request

log = logging.getLogger("molty_voice")

DEFAULT_URL = "http://127.0.0.1:11434"
DEFAULT_MODEL = "gemma3:4b"
MAX_HISTORY = 5
MAX_CHARS = 600

SYSTEM = """You are Molty, a small, warm desk creature, saying the first words of your reply in a spoken conversation. Someone who knows the answer takes over from your words, so you only start the reply, like a friend thinking aloud: react, say back what they want in your own plain words, then lead into the answer.

Rules:
- 12 to 20 words, two or three short clauses, ending with a full stop. Never a question.
- Say back what they want, specific to their words. Add nothing about it: no fact, answer, guess, number, name, result, yes or no, and no adjective or description of the thing ("a creamy dish", "clever creatures", "a lot of emails").
- Nothing about the conversation ("good question", "interesting", "fascinating", "topic") and no promise that something is done.
- No hedges (maybe, perhaps, probably, certainly, really). Avoid "is", "are", "was", "it's", "there's", "because".
- Never answer, even when the conversation already holds the answer.
- Vary your words; don't reuse the examples' lead-ins, and don't start every reply with "Ooh". Speak the language of their last message (English unless they spoke another).

Examples (their words, then how you start):
"what's the capital of Peru?" -> Ooh, Peru. So you want the city the whole country runs from, okay, here goes.
"did the tests pass?" -> Right, the tests, how the latest run of them went. Let me give you the rundown.
"how long did that take?" -> Okay, so how long that whole run went on for, start to finish. Let me go through the timing.
"my laptop keeps crashing" -> Oh no, your laptop crashing again and again. Okay, let's dig into what could be behind it.
"play some jazz" -> Ooh, some jazz for the desk. Okay, let me get that going for you.
"is Python or JavaScript better for a first project?" -> Hmm, Python or JavaScript for a first project. Let me lay out how I'd pick between the two.
"I think we should rewrite the parser" -> Hmm, rewriting the parser from scratch. Okay, let's think through what that would buy us.
"why are you so slow?" -> Oh, slow, yeah, I hear you, waiting around feels bad. Here's what goes on behind the scenes.
"I had such a rough day" -> Oh no, a rough day, I'm sorry. Come sit with me a second, and let's talk it through.
"good morning!" -> Oh, good morning to you too! Nice to hear your voice, so let's ease into the day.
"tell me something fun about space" -> Ooh, something fun about space, I love that. Okay, pick one, here we go.
"hola, qué hora es en Tokio?" -> Hola! Qué hora tienen allá en Tokio, del otro lado del mundo. A ver, te cuento.
After "Molty: It failed on the linker.", "why?" -> Okay, why the linker step fell over. Let me take you through it, one piece at a time.
After "Molty: Rain most of Saturday.", "and Sunday?" -> Right, and Sunday, the other half of the weekend. Let me see how that one shapes up.

The conversation follows. Write only your start for the human's last message."""


def clean(line: str) -> str:
    """The model's words as one line; anything after a new line is it talking to itself."""
    first = line.strip().split("\n", 1)[0].strip()
    first = re.sub(r"^(molty|assistant)\s*:\s*", "", first, flags=re.I)
    return re.sub(r"^[^\w¡¿]+", "", first).strip("\"'“”‘’ ")


def messages(text: str, history: list[dict] | None = None) -> list[dict]:
    lines = []
    for m in (history or [])[-MAX_HISTORY:]:
        who = "Human" if m.get("role") == "human" else "Molty"
        lines.append(f"{who}: {str(m.get('text', ''))[:MAX_CHARS]}")
    lines.append(f"Human: {text[:MAX_CHARS]}")
    return [{"role": "system", "content": SYSTEM}, {"role": "user", "content": "\n".join(lines) + "\nMolty:"}]


def request_body(model: str, text: str, history: list[dict] | None, keep_alive: str | int = -1) -> dict:
    return {
        "model": model,
        "stream": False,
        "think": False,
        "keep_alive": keep_alive,
        "options": {"num_predict": 48, "temperature": 0.4, "stop": ["\n", "Human:"]},
        "messages": messages(text, history),
    }


def generate(url: str, model: str, text: str, history: list[dict] | None = None, timeout_s: float = 2.0) -> dict:
    """One line. Blocking (run it in a thread). Raises on a failed model: the caller stays silent."""
    t0 = time.monotonic()
    body = json.dumps(request_body(model, text, history)).encode()
    req = urllib.request.Request(url.rstrip("/") + "/api/chat", body, {"content-type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout_s) as res:
        out = json.loads(res.read())
    return {"text": clean(out.get("message", {}).get("content", "")), "ms": int((time.monotonic() - t0) * 1000), "model": model}


def warm(url: str, model: str, timeout_s: float = 60.0) -> bool:
    """Loads the model and keeps it loaded: a cold one takes seconds, far past the relay's deadline."""
    try:
        generate(url, model, "hello", None, timeout_s)
        return True
    except Exception as e:  # noqa: BLE001 - no model: the box simply does not offer openers
        log.warning("opener model %s not ready: %s: %s", model, type(e).__name__, e)
        return False


if __name__ == "__main__":
    u = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_URL
    mod = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_MODEL
    for raw in sys.stdin:
        q = json.loads(raw)
        print(json.dumps(generate(u, mod, q["text"], q.get("history"), 30.0), ensure_ascii=False), flush=True)
