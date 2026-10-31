"""The agent side: the relay's REST surface (as any agent uses it) and the reply policy.

- `Rest`: pair (role agent / voice), talk, say, the owner voice API. The token stays in memory.
- `ScriptedAgent`: the scenario's lines, a fixed think time (`gap_ms`, default 0) — the agent
  gap held constant so the pipeline's own latency shows.
- `LlmAgent`: an OpenAI-compatible chat endpoint (e.g. Ollama on the box) writes each reply
  from what Molty heard; its real think time is part of the gap and reported apart.
"""

from __future__ import annotations

import asyncio
import json
import time
import urllib.error
import urllib.request

UA = "molty-sim/0.1"


def _http(method: str, url: str, body: dict | None = None, token: str | None = None, headers: dict | None = None, timeout: float = 60) -> tuple[int, dict]:
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method, headers={"content-type": "application/json", "user-agent": UA, **(headers or {})})
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


async def http(method: str, url: str, body: dict | None = None, **kw) -> tuple[int, dict]:
    return await asyncio.to_thread(_http, method, url, body, **kw)


class RelayError(RuntimeError):
    def __init__(self, status: int, body: dict):
        super().__init__(f"relay {status}: {body.get('error')} {body.get('message', '')}".strip())
        self.status, self.body = status, body


class Rest:
    def __init__(self, base: str, device_id: str):
        self.base, self.device = base.rstrip("/"), device_id
        self.token: str | None = None

    async def pair(self, link_code: str, name: str, role: str | None = None) -> str:
        body = {"device_id": self.device, "link_code": link_code, "agent_name": name, "wait_s": 0}
        if role:
            body["role"] = role
        status, out = await http("POST", f"{self.base}/v1/pair", body)
        while out.get("ok") and out.get("status") == "pending":
            status, out = await http("POST", f"{self.base}/v1/pair", {"device_id": self.device, "pair_id": out["pair_id"], "wait_s": 25})
        if not out.get("agent_token"):
            raise RelayError(status, out)
        return out["agent_token"]

    async def talk(self, say: str | None, listen: bool = True, **args) -> dict:
        body = {k: v for k, v in {"say": say, "listen": listen, **args}.items() if v is not None}
        status, out = await http("POST", f"{self.base}/v1/devices/{self.device}/talk", body, token=self.token, timeout=60)
        return await self._job(status, out)

    async def say(self, text: str) -> dict:
        status, out = await http("POST", f"{self.base}/v1/devices/{self.device}/cmd", {"name": "say", "args": {"text": text}, "wait": True}, token=self.token, timeout=60)
        return await self._job(status, out)

    async def job(self, job: str) -> dict:
        """A job by id, once it is over (e.g. the relay's listen job of a shake: its turn_id)."""
        return await self._job(200, {"ok": True, "result": {"job": job, "status": "running"}})

    async def _job(self, status: int, out: dict) -> dict:
        if not out.get("ok"):
            raise RelayError(status, out)
        r = out["result"]
        while r.get("status") not in ("done", "failed"):
            st, j = await http("GET", f"{self.base}/v1/jobs/{r['job']}?wait_s=8", token=self.token, timeout=30)
            if st >= 400:
                raise RelayError(st, j)
            if j.get("status") == "failed":
                raise RelayError(st, {"error": j.get("error"), "message": j.get("detail", "")})
            if j.get("status") == "done":
                return {"job": j["job"], "status": "done", **(j.get("result") or {})}
            r = {"job": r["job"], "status": j.get("status")}
        return r

    async def voice_status(self, link_code: str) -> dict:
        _, out = await http("GET", f"{self.base}/v1/devices/{self.device}/voice", headers={"X-Molty-Link-Code": link_code})
        return out.get("result") or out


class ScriptedAgent:
    name = "scripted"

    def __init__(self, gap_ms: int = 0):
        self.gap_ms = gap_ms

    async def reply(self, scripted: str | None, heard: str, history: list[dict]) -> str | None:
        if self.gap_ms:
            await asyncio.sleep(self.gap_ms / 1000)
        return scripted


class LlmAgent:
    """Replies from an OpenAI-compatible /v1/chat/completions (no key needed for Ollama)."""

    name = "llm"
    SYSTEM = ("You are Molty, a small desk creature that talks out loud. Reply to the human in ONE short spoken "
              "sentence (under 20 words), plain words, no lists, no emoji.")

    def __init__(self, url: str, model: str, gap_ms: int = 0, timeout: float = 30):
        self.url, self.model, self.gap_ms, self.timeout = url.rstrip("/"), model, gap_ms, timeout

    async def reply(self, scripted: str | None, heard: str, history: list[dict]) -> str | None:
        msgs = [{"role": "system", "content": self.SYSTEM}]
        for h in history[-6:]:
            msgs += [{"role": "assistant", "content": h["said"]}] if h.get("said") else []
            msgs += [{"role": "user", "content": h["heard"] or "(silence)"}]
        status, out = await http("POST", f"{self.url}/v1/chat/completions",
                                 {"model": self.model, "messages": msgs, "max_tokens": 80, "temperature": 0.7, "stream": False},
                                 timeout=self.timeout)
        if status >= 400:
            return scripted
        text = (out.get("choices") or [{}])[0].get("message", {}).get("content", "").strip()
        if self.gap_ms:
            await asyncio.sleep(self.gap_ms / 1000)
        return text or scripted


def now() -> float:
    return time.monotonic()
