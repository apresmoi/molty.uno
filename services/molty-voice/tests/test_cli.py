import json
import os
import stat
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

from molty_voice import cli
from molty_voice.cli import main
from molty_voice.config import load

DEVICE = "molty-k7m2q9xa4c"
TOKEN = DEVICE + "." + "Z" * 43


def fake_relay(calls):
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["content-length"])))
            calls.append(body)
            if "pair_id" in body:
                code, out = 200, {"ok": True, "agent_token": TOKEN, "device_id": DEVICE, "agent_name": "voice box", "code": "4821", "role": "voice"}
            else:
                code, out = 202, {"ok": True, "status": "pending", "pair_id": "p_" + "a" * 22, "code": "4821", "expires_in_s": 120}
            data = json.dumps(out).encode()
            self.send_response(code)
            self.send_header("content-type", "application/json")
            self.send_header("content-length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    srv = HTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def test_pair_asks_for_role_voice_shows_the_code_and_stores_the_token_0600(tmp_path, capsys):
    calls = []
    srv = fake_relay(calls)
    cfg = tmp_path / "molty-voice" / "config.json"
    try:
        rc = main(["--config", str(cfg), "pair", "--relay", f"http://127.0.0.1:{srv.server_port}", "--device", DEVICE, "--link-code", "k7m2w9c4"])
    finally:
        srv.shutdown()
    assert rc == 0
    assert calls[0] == {"device_id": DEVICE, "link_code": "K7M2W9C4", "agent_name": "voice box", "role": "voice", "wait_s": 0}
    assert calls[1]["pair_id"] == "p_" + "a" * 22
    out = capsys.readouterr()
    assert "4821" in out.out
    assert TOKEN not in out.out + out.err
    assert stat.S_IMODE(os.stat(cfg).st_mode) == 0o600
    c = load(cfg)
    assert (c.token, c.device_id, c.relay) == (TOKEN, DEVICE, f"http://127.0.0.1:{srv.server_port}")
    assert TOKEN not in repr(c)


def test_run_refuses_when_not_paired(tmp_path):
    assert main(["--config", str(tmp_path / "none.json"), "run"]) == 2


def test_run_passes_the_end_of_turn_flags_to_the_bridge(tmp_path, monkeypatch):
    from molty_voice import cli
    from molty_voice.config import Config, save

    path = tmp_path / "c.json"
    save(Config(relay="http://r", device_id=DEVICE, token=TOKEN), path)
    seen = []

    class FakeBridge:
        def __init__(self, cfg):
            seen.append(cfg)

        async def run_forever(self):
            return None

    monkeypatch.setattr(cli, "Bridge", FakeBridge)
    assert main(["--config", str(path), "run"]) == 0
    assert seen[-1].turn_end_hold_s == 1.2  # the default
    assert main(["--config", str(path), "run", "--turn-end-hold", "0", "--turn-end", "0.7", "--turn-end-skip", "8"]) == 0
    assert (seen[-1].turn_end_hold_s, seen[-1].turn_end, seen[-1].turn_end_skip_steps) == (0.0, 0.7, 8)
    assert main(["--config", str(path), "run", "--turn-end-hold", "2.5"]) == 0
    assert seen[-1].turn_end_hold_s == 2.5


def test_check_passes_when_the_round_trip_does(monkeypatch, tmp_path, capsys):
    tried = []

    async def make_prompt(self):
        tried.append(1)
        if len(tried) == 2:
            self.stt_prompt = object()

    monkeypatch.setattr(cli.Bridge, "_make_prompt", make_prompt)
    assert cli.main(["--config", str(tmp_path / "none.json"), "check"]) == 0
    assert len(tried) == 2 and "ok" in capsys.readouterr().out


def test_check_fails_when_the_round_trip_never_does(monkeypatch, tmp_path):
    async def make_prompt(self):
        pass

    monkeypatch.setattr(cli.Bridge, "_make_prompt", make_prompt)
    assert cli.main(["--config", str(tmp_path / "none.json"), "check"]) == 1
