"""molty-voice pair | run | unpair."""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import re
import sys
import urllib.error
import urllib.request
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


def cmd_run(a: argparse.Namespace) -> int:
    path = Path(a.config) if a.config else default_path()
    cfg = load(path)
    for k in ("stt_url", "tts_url", "api_key", "default_voice", "name"):
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
    if a.no_stt:
        cfg.stt = False
    if a.no_tts:
        cfg.tts = False
    if a.relay:
        cfg.relay = http_base(a.relay)
    if not (cfg.relay and cfg.device_id and cfg.token):
        print(f"not paired: run `molty-voice pair` first (config: {path})", file=sys.stderr)
        return 2
    if cfg.default_voice not in cfg.voices:
        cfg.voices.insert(0, cfg.default_voice)
    logging.getLogger("molty_voice").info("molty-voice %s for %s via %s", __version__, cfg.device_id, cfg.relay)
    try:
        asyncio.run(Bridge(cfg).run_forever())
    except Fatal as e:
        print(str(e), file=sys.stderr)
        return 3
    except KeyboardInterrupt:
        return 0
    return 0


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
    pr.add_argument("--no-stt", action="store_true", help="serve TTS only")
    pr.add_argument("--no-tts", action="store_true", help="serve STT only")

    sub.add_parser("unpair", help="revoke this box's token and forget it")
    a = p.parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if a.verbose else logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    logging.getLogger("websockets").setLevel(logging.WARNING)
    return {"pair": cmd_pair, "run": cmd_run, "unpair": cmd_unpair}[a.cmd](a)
