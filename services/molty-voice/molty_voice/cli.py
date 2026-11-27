"""molty-voice pair | run | unpair | check."""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import os
import re
import sys
import urllib.error
import urllib.request
from logging.handlers import RotatingFileHandler
from pathlib import Path

from . import __version__

from .bridge import Bridge, Fatal
from .config import Config, default_path, load, save

DEVICE_RE = re.compile(r"^molty-[a-z2-9]{10}$")
LINK_RE = re.compile(r"^[A-Z2-9]{8}$")


def http_base(relay: str) -> str:
    return re.sub(r"^ws(s?)://", r"http\1://", relay.strip().rstrip("/"))


def post_json(url: str, body: dict, timeout: float, method: str = "POST", token: str | None = None) -> tuple[int, dict]:
    data = json.dumps(body).encode()
    req = urllib.request.Request(url, data=data if method != "DELETE" else None, method=method, headers={"content-type": "application/json", "user-agent": f"molty-voice/{__version__}"})
    if token:
        req.add_header("authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"{}")
        except ValueError:
            return e.code, {"ok": False, "error": f"http_{e.code}"}


def cmd_pair(a: argparse.Namespace) -> int:
    if not DEVICE_RE.match(a.device):
        print("--device must look like molty-xxxxxxxxxx", file=sys.stderr)
        return 2
    code = a.link_code.strip().upper()
    if not LINK_RE.match(code):
        print("--link-code is the 8-character code from Molty's console (agent page)", file=sys.stderr)
        return 2
    base = http_base(a.relay)
    status, out = post_json(f"{base}/v1/pair", {"device_id": a.device, "link_code": code, "agent_name": a.name, "role": "voice", "wait_s": 0}, 30)
    while True:
        if not out.get("ok"):
            print(f"pairing failed: {out.get('error', status)}{': ' + out['message'] if out.get('message') else ''}", file=sys.stderr)
            return 1
        if out.get("agent_token"):
            break
        if out.get("status") == "pending":
            if not getattr(a, "_shown", False):
                print(f"Molty shows {out.get('code')} — tap Molty to accept the voice box (expires in {out.get('expires_in_s', 120)} s)…", flush=True)
                a._shown = True
            status, out = post_json(f"{base}/v1/pair", {"device_id": a.device, "pair_id": out["pair_id"], "wait_s": 25}, 40)
            continue
        print(f"unexpected answer from the relay (HTTP {status})", file=sys.stderr)
        return 1
    path = Path(a.config) if a.config else default_path()
    cfg = load(path)
    cfg.relay, cfg.device_id, cfg.token, cfg.name = base, a.device, out["agent_token"], a.name
    save(cfg, path)
    print(f"paired with {a.device}. The token is stored in {path} (mode 0600). Start it with: molty-voice run")
    return 0


def cmd_unpair(a: argparse.Namespace) -> int:
    path = Path(a.config) if a.config else default_path()
    cfg = load(path)
    if not cfg.token:
        print("not paired", file=sys.stderr)
        return 1
    status, out = post_json(f"{http_base(cfg.relay)}/v1/token", {}, 20, method="DELETE", token=cfg.token)
    if not out.get("ok") and status != 401:
        print(f"the relay said: {out.get('error', status)}", file=sys.stderr)
        return 1
    cfg.token = ""
    save(cfg, path)
    print("unpaired; the token was revoked and removed")
    return 0


LOG_NAME = "molty-voice.log"


class _PrivateLog(RotatingFileHandler):
    """The run's log file, readable by its owner only (0600), rotated files too."""

    def _open(self):
        return os.fdopen(os.open(self.baseFilename, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600), "a", encoding=self.encoding)


class _Redact(logging.Filter):
    """Belt and braces: no secret the run holds (token, key) ever reaches the file, whatever a line says."""

    def __init__(self, secrets: list[str]):
        super().__init__()
        self.secrets = [x for x in secrets if x and len(x) >= 8]

    def filter(self, record: logging.LogRecord) -> bool:
        msg = record.getMessage()
        for x in self.secrets:
            msg = msg.replace(x, "<redacted>")
        record.msg, record.args = msg, None
        return True


def log_to_file(path: Path, secrets: list[str], max_bytes: int = 1 << 20) -> logging.Handler:
    """Every run appends its log lines (molty_voice's, INFO and up: never audio) to `path`, next to the
    config, capped at two files of max_bytes (the current one and the one before)."""
    path.parent.mkdir(parents=True, exist_ok=True)
    lg = logging.getLogger("molty_voice")
    for old in [x for x in lg.handlers if isinstance(x, _PrivateLog)]:
        lg.removeHandler(old)
        old.close()
    h = _PrivateLog(path, maxBytes=max_bytes, backupCount=1, encoding="utf-8")
    h.setLevel(logging.INFO)
    h.setFormatter(logging.Formatter("%(asctime)s %(levelname)s %(message)s"))
    h.addFilter(_Redact(secrets))
    lg.addHandler(h)
    if lg.getEffectiveLevel() > logging.INFO:
        lg.setLevel(logging.INFO)
    return h


def cmd_run(a: argparse.Namespace) -> int:
    path = Path(a.config) if a.config else default_path()
    cfg = load(path)
    for k in ("stt_url", "tts_url", "api_key", "default_voice", "name", "engine", "realtime_url", "realtime_model", "realtime_voice", "realtime_turn"):
        v = getattr(a, k, None)
        if v:
            setattr(cfg, k, v)
    if a.voices:
        cfg.voices = [v.strip() for v in a.voices.split(",") if v.strip()]
    if a.turn_end is not None:
        cfg.turn_end = a.turn_end
    if a.turn_end_skip is not None:
        cfg.turn_end_skip_steps = a.turn_end_skip
    if a.turn_end_hold is not None:
        cfg.turn_end_hold_s = max(0.0, a.turn_end_hold)
    if a.reflex is not None:
        cfg.reflex = a.reflex
    if a.opener is not None:
        cfg.opener = a.opener
    if a.no_stt:
        cfg.stt = False
    if a.no_tts:
        cfg.tts = False
    if a.relay:
        cfg.relay = http_base(a.relay)
    if not (cfg.relay and cfg.device_id and cfg.token):
        print(f"not paired: run `molty-voice pair` first (config: {path})", file=sys.stderr)
        return 2
    if a.daily_minutes is not None:
        cfg.realtime_daily_minutes = max(0.0, a.daily_minutes)
    if cfg.engine not in ("kyutai", "realtime"):
        print("--engine is kyutai or realtime", file=sys.stderr)
        return 2
    if cfg.engine == "realtime" and not cfg.front_key():
        print("the realtime engine needs your provider key: set MOLTY_REALTIME_KEY (or OPENAI_API_KEY) in this shell, "
              f"or put \"realtime_key\" in {path} (mode 0600)", file=sys.stderr)
        return 2
    if cfg.default_voice not in cfg.voices:
        cfg.voices.insert(0, cfg.default_voice)
    log_to_file(path.parent / LOG_NAME, [cfg.token, cfg.front_key(), cfg.api_key])
    logging.getLogger("molty_voice").info("molty-voice %s (%s engine) for %s via %s", __version__, cfg.engine, cfg.device_id, cfg.relay)
    try:
        asyncio.run(Bridge(cfg, usage_path=path.parent / "realtime-usage.json").run_forever())
    except Fatal as e:
        print(str(e), file=sys.stderr)
        return 3
    except KeyboardInterrupt:
        return 0
    return 0


def cmd_check(a: argparse.Namespace) -> int:
    """Speaks a line through TTS and hears it back through STT: no relay, no pairing needed."""
    cfg = load(Path(a.config) if a.config else None)
    if a.engine:
        cfg.engine = a.engine
    if cfg.engine == "realtime":
        return check_realtime(cfg)
    b = Bridge(cfg)
    for _ in range(3):  # the TTS now and then clips a line; the check is strict about the words
        asyncio.run(b._make_prompt())
        if b.stt_prompt is not None:
            print(f"ok: STT heard TTS say {cfg.stt_prompt!r}")
            return 0
    print(f"failed: is STT at {cfg.stt_url} and TTS at {cfg.tts_url} up? See the logs above.", file=sys.stderr)
    return 1


def check_realtime(cfg: Config) -> int:
    """Opens one realtime session, has it say a line, counts the audio: proves the key, the model and the voice."""
    from .realtime import RealtimeFront

    got = {"bytes": 0, "end": None, "text": ""}

    async def js(m: dict) -> None:
        if m.get("t") == "tts.end":
            got["end"] = m
        if m.get("t") == "tts.text":
            got["text"] = m.get("text", "")

    async def bn(b: bytes) -> None:
        got["bytes"] += len(b)

    async def go() -> None:
        f = RealtimeFront(cfg, js, bn)
        if not f.key:
            raise SystemExit("no key: set MOLTY_REALTIME_KEY (or OPENAI_API_KEY), or realtime_key in the config")
        await f.speak("check", "Hello, I am Molty.", "exact")
        for _ in range(200):
            if got["end"]:
                break
            await asyncio.sleep(0.1)
        await f.end("check")

    asyncio.run(go())
    end = got["end"] or {}
    if got["bytes"] and not end.get("error"):
        print(f"ok: the realtime model said {got['text']!r} ({got['bytes'] // 32} ms of audio; first audio {end.get('ms', {}).get('first_audio')} ms)")
        return 0
    print(f"failed: {end.get('error') or 'no audio'}; see the log lines above", file=sys.stderr)
    return 1


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="molty-voice", description="Your own STT/TTS for molty.UNO: bridges a local Kyutai moshi-server to the relay.")
    p.add_argument("--config", help=f"config file (default {default_path()})")
    p.add_argument("-v", "--verbose", action="store_true")
    sub = p.add_subparsers(dest="cmd", required=True)

    pp = sub.add_parser("pair", help="pair this box with a Molty (tap Molty to accept)")
    pp.add_argument("--relay", required=True, help="relay origin, e.g. https://molty-relay.example.workers.dev")
    pp.add_argument("--device", required=True, help="device id, molty-xxxxxxxxxx")
    pp.add_argument("--link-code", required=True, help="the 8-character code from Molty's console")
    pp.add_argument("--name", default="voice box", help="name shown next to the code (default: voice box)")

    pr = sub.add_parser("run", help="connect to the relay and serve STT/TTS (reconnects on its own)")
    d = Config()
    pr.add_argument("--relay", help="override the paired relay origin")
    pr.add_argument("--stt-url", help=f"Kyutai STT websocket (default {d.stt_url})")
    pr.add_argument("--tts-url", help=f"Kyutai TTS websocket (default {d.tts_url})")
    pr.add_argument("--api-key", help="kyutai-api-key header (default public_token)")
    pr.add_argument("--default-voice", help=f"voice when the agent names none (default {d.default_voice})")
    pr.add_argument("--voices", help="comma-separated voices offered to the console")
    pr.add_argument("--name", help="name reported to the relay")
    pr.add_argument("--turn-end", type=float, help=f"end-of-turn threshold on the STT's pause prediction (default {d.turn_end}; 0 = off)")
    pr.add_argument("--turn-end-skip", type=int, help=f"model steps (80 ms) at a listen's start the end-of-turn rule ignores (default {d.turn_end_skip_steps})")
    pr.add_argument("--turn-end-hold", type=float, help=f"seconds a pause must last before it ends a turn mid-sentence, i.e. when the last word has no . ? ! (default {d.turn_end_hold_s}; 0 = end at the first paused step)")
    pr.add_argument("--opener", action=argparse.BooleanOptionalAction, default=None,
                    help=f"offer first words: a small Ollama model starts Molty's reply (default {d.opener})")
    pr.add_argument("--reflex", action=argparse.BooleanOptionalAction, default=None,
                    help=f"run Molty's reflex classifier on the CPU (needs molty-voice[reflex]; default {d.reflex})")
    pr.add_argument("--engine", choices=["kyutai", "realtime"], help=f"kyutai (local GPU STT + TTS) or realtime (a realtime voice model with your own key, no GPU; default {d.engine})")
    pr.add_argument("--realtime-model", help=f"realtime model (default {d.realtime_model})")
    pr.add_argument("--realtime-voice", help=f"realtime voice (default {d.realtime_voice})")
    pr.add_argument("--realtime-turn", choices=["semantic", "server"], help=f"how the model ends the person's turn (default {d.realtime_turn})")
    pr.add_argument("--realtime-url", help="realtime websocket (default the provider's; a local fake for tests)")
    pr.add_argument("--daily-minutes", type=float, help=f"realtime minutes a day at most, 0 = no cap (default {d.realtime_daily_minutes})")
    pr.add_argument("--no-stt", action="store_true", help="serve TTS only")
    pr.add_argument("--no-tts", action="store_true", help="serve STT only")

    sub.add_parser("unpair", help="revoke this box's token and forget it")
    pc = sub.add_parser("check", help="test STT and TTS with a round trip (realtime: one spoken line), without the relay")
    pc.add_argument("--engine", choices=["kyutai", "realtime"], help="check this engine (default: the config's)")
    a = p.parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if a.verbose else logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    logging.getLogger("websockets").setLevel(logging.WARNING)
    return {"pair": cmd_pair, "run": cmd_run, "unpair": cmd_unpair, "check": cmd_check}[a.cmd](a)
