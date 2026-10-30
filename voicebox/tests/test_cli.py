import json
import os
import stat
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

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
