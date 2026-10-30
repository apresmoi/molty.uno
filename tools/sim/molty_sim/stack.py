"""The pieces around the sim: the ssh tunnel to the box's Kyutai, a local `wrangler dev` relay, a
second molty-voice bridge (this repo's voicebox/, its own config file) paired to the sim device.

Nothing here touches the box's services: the tunnel is a client connection to 127.0.0.1:8089/8090
on the box, and the bridge runs on this machine with its own --config (never the systemd one).
"""

from __future__ import annotations

import asyncio
import os
import re
import signal
import socket
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]  # the repo
RELAY_DIR = ROOT / "relay"


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def port_open(port: int, host: str = "127.0.0.1") -> bool:
    try:
        with socket.create_connection((host, port), timeout=0.5):
            return True
    except OSError:
        return False


class Proc:
    """A child process whose output goes to a log file; stopped with SIGTERM (then SIGKILL)."""

    def __init__(self, name: str, args: list[str], log: Path, cwd: Path | None = None, env: dict | None = None):
        self.name, self.log = name, log
        self._f = open(log, "ab")
        self.p = subprocess.Popen(args, cwd=cwd, env={**os.environ, **(env or {})}, stdout=self._f, stderr=subprocess.STDOUT,
                                  stdin=subprocess.DEVNULL, start_new_session=True)

    def alive(self) -> bool:
        return self.p.poll() is None

    def text(self) -> str:
        return self.log.read_text(errors="replace") if self.log.exists() else ""

    def stop(self) -> None:
        if self.alive():
            try:
                os.killpg(self.p.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                self.p.wait(8)
            except subprocess.TimeoutExpired:
                os.killpg(self.p.pid, signal.SIGKILL)
        self._f.close()


async def wait_until(pred, timeout: float, what: str, proc: Proc | None = None) -> None:
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if pred():
            return
        if proc is not None and not proc.alive():
            raise RuntimeError(f"{proc.name} exited while waiting for {what}; log: {proc.log}")
        await asyncio.sleep(0.2)
    raise TimeoutError(f"timed out waiting for {what}")


async def tunnel(host: str, tts_port: int, stt_port: int, log: Path) -> Proc | None:
    """ssh -N -L to the box's Kyutai TTS (8089) / STT (8090); None when both ports already answer."""
    if port_open(tts_port) and port_open(stt_port):
        return None
    p = Proc("ssh tunnel", ["ssh", "-N", "-o", "ExitOnForwardFailure=yes", "-o", "ServerAliveInterval=15",
                            "-L", f"{tts_port}:127.0.0.1:8089", "-L", f"{stt_port}:127.0.0.1:8090", host], log)
    await wait_until(lambda: port_open(tts_port) and port_open(stt_port), 20, "the ssh tunnel", p)
    return p


async def relay(state: Path, log: Path, port: int | None = None) -> tuple[Proc, str]:
    """`wrangler dev --local` (workerd + local DO storage), VOICE_PROVIDER=none: only the box speaks."""
    wr = RELAY_DIR / "node_modules" / ".bin" / "wrangler"
    if not wr.exists():
        raise RuntimeError("relay deps missing: run `cd relay && pnpm install --frozen-lockfile` first")
    port = port or free_port()
    p = Proc("wrangler dev", [str(wr), "dev", "--local", "--ip", "127.0.0.1", "--port", str(port), "--persist-to", str(state),
                              "--var", "VOICE_PROVIDER:none", "--log-level", "info"], log, cwd=RELAY_DIR,
             env={"CI": "1", "WRANGLER_SEND_METRICS": "false"})

    def healthy():
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{port}/healthz", timeout=1) as r:
                return r.status == 200
        except OSError:
            return False

    await wait_until(healthy, 90, "wrangler dev", p)
    return p, f"http://127.0.0.1:{port}"


def bridge_cmd(config: Path, *extra: str) -> list[str]:
    return [sys.executable, "-m", "molty_voice", "--config", str(config), *extra]


def pair_bridge(config: Path, relay_http: str, device: str, link_code: str, log: Path) -> None:
    r = subprocess.run(bridge_cmd(config, "pair", "--relay", relay_http, "--device", device, "--link-code", link_code, "--name", "voice box sim"),
                       capture_output=True, text=True, timeout=150)
    with open(log, "a") as f:
        f.write(r.stdout + r.stderr)
    if r.returncode != 0:
        raise RuntimeError(f"molty-voice pair failed (exit {r.returncode}); see {log}")


async def bridge(config: Path, log: Path, stt_url: str, tts_url: str, tuning: list[str]) -> Proc:
    p = Proc("molty-voice", bridge_cmd(config, "run", "--stt-url", stt_url, "--tts-url", tts_url, *tuning), log)
    await wait_until(lambda: "connected to the relay" in p.text(), 30, "the bridge to connect", p)
    return p


BRIDGE_LINE = re.compile(r"^(\S+ \S+) \w+ (.*)$")
