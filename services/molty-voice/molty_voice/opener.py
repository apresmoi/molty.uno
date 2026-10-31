"""First words: a very small model starts Molty's reply before the agent can speak.

The relay asks at the end of the human's turn (`{"t":"opener","id","text","history":[{"role","text"}]}`)
and answers itself with what it may say: this module only writes a line. Whether the line may be
spoken is the relay's call (services/relay core/opener.ts guardOpener), whichever model wrote it.

Model: `gemma3:4b` on the box's Ollama, loaded and kept loaded (about 30 ms to the first word,
about 60 ms for a line; research/openers-eval.md). `python -m molty_voice.opener` writes one line per
JSON line `{"text","history"}` on stdin (the eval's way in).
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

SYSTEM = """You are Molty, a small desk creature in a spoken conversation. Someone smarter will finish your sentence in a moment: you only say the FIRST few words of Molty's reply, so they flow into the rest.

Rules:
- At most 6 words. Name their topic. Use the earlier messages to know what "it" or "that" means.
- Speak as Molty answering them: never repeat their sentence back. "me" in their words means "you" in yours.
- Never give the answer, a fact, a name or a number they did not say. Never say something is done. No yes or no.
- It starts a sentence, so end with a comma. Never ask a question. Don't start every line the same way.
- Same language as their last message. Plain words only. Never copy their whole sentence.

Examples:
Human: what's the capital of Peru?
Molty: Hmm, Peru's capital,
Human: how long did that take?
Molty: Let me see how long,
Human: my laptop keeps crashing
Molty: Ugh, crashing again,
Human: play some jazz
Molty: Some jazz, right,
Human: remind me to call mom
Molty: Calling mom, sure,
Human: show me the photos
Molty: Here come the photos,
Human: can you make it faster?
Molty: Making it faster,
Human: say hello
Molty: Hello there,
Human: good morning!
Molty: Hey, good morning,
Human: I think we should rewrite the parser
Molty: Hmm, rewriting the parser,

With earlier messages:
Human: can you check the build?
Molty: It failed on the linker.
Human: why?
Molty: So why the linker failed,"""


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
        "options": {"num_predict": 18, "temperature": 0.3, "stop": ["\n", "Human:"]},
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
