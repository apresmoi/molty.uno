"""A stand-in realtime provider: the server side of the realtime WebSocket events the front uses.

For the bridge's tests and the local end-to-end run (tools/sim `molty-sim front`): it speaks the
same event names and shapes as the real API (molty_voice/realtime.py lists them), with a plain energy
detector as its turn detection, a tone as its voice and canned words. Never used by `molty-voice run`
unless `--realtime-url` points at it.
"""

from __future__ import annotations

import asyncio
import base64
import json
import logging
import re

import numpy as np
from websockets.asyncio.server import serve

RATE = 24000
_log = logging.getLogger("molty_voice.fake_realtime")
_log.setLevel(logging.WARNING)  # its handshake log would show the Authorization header it was sent


class FakeRealtime:
    def __init__(self, *, first_audio_ms: int = 300, speed: float = 3.0, silence_ms: int = 500, transcript: str | None = None,
                 reply=None, threshold: float = 0.02):
        self.first_audio_ms = first_audio_ms
        self.speed = speed  # audio is generated this many times faster than real time
        self.silence_ms = silence_ms
        self.transcript = transcript  # what the "person" said (None: "hello molty")
        # (transcript) -> (words to say, call hand_to_agent?)
        self.reply = reply or (lambda heard: ("Let me check on that.", True) if "?" in heard or "what" in heard.lower() else ("Hi there!", False))
        self.threshold = threshold
        self.sessions: list[dict] = []
        self.port = 0
        self._server = None

    async def __aenter__(self):
        self._server = await serve(self.handler, "127.0.0.1", 0, max_size=1 << 24, logger=_log).__aenter__()
        self.port = self._server.sockets[0].getsockname()[1]
        return self

    async def __aexit__(self, *exc):
        await self._server.__aexit__(*exc)

    @property
    def url(self) -> str:
        return f"ws://127.0.0.1:{self.port}/v1/realtime"

    async def handler(self, ws):
        sess = {"auth": bool(ws.request.headers.get("authorization")), "path": ws.request.path, "updates": [], "items": [], "creates": [],
                "appended": 0, "commits": 0, "clears": 0, "closed": False, "cancels": 0}
        self.sessions.append(sess)
        st = {"buf": [], "speech": False, "quiet": 0, "audio_ms": 0, "start_ms": 0, "active": None, "n": 0, "create": True}
        lock = asyncio.Lock()

        async def send(ev):
            async with lock:
                await ws.send(json.dumps(ev))

        def nid(p):
            st["n"] += 1
            return f"{p}_{st['n']}"

        async def commit():
            item = nid("item")
            st["speech"], st["quiet"] = False, 0
            sess["commits"] += 1
            st["buf"] = []
            await send({"type": "input_audio_buffer.committed", "item_id": item, "previous_item_id": None})
            heard = self.transcript if self.transcript is not None else "hello molty"
            if st["create"]:
                asyncio.create_task(respond(heard, None))

            async def transcribe():
                await asyncio.sleep(0.25)
                for w in heard.split():
                    await send({"type": "conversation.item.input_audio_transcription.delta", "item_id": item, "content_index": 0, "delta": w + " "})
                await send({"type": "conversation.item.input_audio_transcription.completed", "item_id": item, "content_index": 0, "transcript": heard})
            asyncio.create_task(transcribe())

        async def respond(heard: str, req: dict | None):
            if st["active"] is not None:
                await send({"type": "error", "error": {"type": "invalid_request_error", "code": "conversation_already_has_active_response",
                                                       "message": "Conversation already has an active response in progress"}})
                return
            rid = nid("resp")
            meta = (req or {}).get("metadata") or None
            st["active"] = rid
            await send({"type": "response.created", "response": {"id": rid, "status": "in_progress", "metadata": meta}})
            instr = (req or {}).get("instructions") or ""
            m = re.search(r"word for word, and nothing else: (.*)$", instr, re.S)
            tool = False
            if m:
                words = m.group(1).strip()
            elif "has not answered yet" in instr:
                words = "Still waiting on that, one moment."
            elif req is not None and sess["items"] and sess["items"][-1].startswith("Inner voice: "):
                words = "So, " + sess["items"][-1][len("Inner voice: "):]
            else:
                words, tool = self.reply(heard)
            await asyncio.sleep(self.first_audio_ms / 1000)
            n = max(1, len(words.split()))
            total = int(RATE * 0.3 * n)  # 300 ms a word
            sent = 0
            parts = words.split()
            said = 0
            for k in range(0, total, RATE // 10):
                if st["active"] != rid:
                    break
                # the words' text runs a little ahead of their audio (two words), as a model's text does
                while said < len(parts) and said * int(RATE * 0.3) <= k + 2 * int(RATE * 0.3):
                    await send({"type": "response.output_audio_transcript.delta", "response_id": rid, "delta": parts[said] + " "})
                    said += 1
                pcm = (np.sin(np.arange(k, min(total, k + RATE // 10)) * 2 * np.pi * 220 / RATE) * 6000).astype("<i2").tobytes()
                await send({"type": "response.output_audio.delta", "response_id": rid, "item_id": "x", "delta": base64.b64encode(pcm).decode()})
                sent += len(pcm) // 2
                await asyncio.sleep(0.1 / self.speed)
            cancelled = st["active"] != rid
            st["active"] = None
            await send({"type": "response.output_audio_transcript.done", "response_id": rid, "transcript": words})
            out = [{"type": "function_call", "name": "hand_to_agent", "call_id": "call_1", "arguments": "{}"}] if tool else []
            await send({"type": "response.done", "response": {"id": rid, "status": "cancelled" if cancelled else "completed", "metadata": meta, "output": out,
                                                              "usage": {"input_token_details": {"audio_tokens": st["audio_ms"] // 100, "text_tokens": 300, "cached_tokens_details": {"audio_tokens": 0}},
                                                                        "output_token_details": {"audio_tokens": sent * 20 // RATE, "text_tokens": n}}}})

        try:
            async for raw in ws:
                ev = json.loads(raw)
                t = ev.get("type")
                if t == "session.update":
                    sess["updates"].append(ev["session"])
                    td = (((ev["session"].get("audio") or {}).get("input") or {}).get("turn_detection"))
                    if isinstance(td, dict) and "create_response" in td:
                        st["create"] = bool(td["create_response"])
                    await send({"type": "session.updated", "session": ev["session"]})
                elif t == "input_audio_buffer.append":
                    pcm = np.frombuffer(base64.b64decode(ev["audio"]), dtype="<i2").astype(np.float32) / 32768
                    sess["appended"] += len(pcm)
                    for i in range(0, len(pcm), RATE // 50):  # 20 ms steps
                        w = pcm[i : i + RATE // 50]
                        st["audio_ms"] += 20
                        loud = float(np.sqrt(np.mean(w * w))) > self.threshold if w.size else False
                        if loud and not st["speech"]:
                            st["speech"], st["quiet"] = True, 0
                            await send({"type": "input_audio_buffer.speech_started", "audio_start_ms": st["audio_ms"], "item_id": "pending"})
                        elif st["speech"]:
                            st["quiet"] = 0 if loud else st["quiet"] + 20
                            if st["quiet"] >= self.silence_ms:
                                await send({"type": "input_audio_buffer.speech_stopped", "audio_end_ms": st["audio_ms"], "item_id": "pending"})
                                await commit()
                elif t == "input_audio_buffer.commit":
                    await commit()
                elif t == "input_audio_buffer.clear":
                    sess["clears"] += 1
                    st["buf"], st["speech"], st["quiet"] = [], False, 0
                elif t == "conversation.item.create":
                    item = ev.get("item") or {}
                    if item.get("type") == "message":
                        sess["items"].append(item["content"][0]["text"])
                    else:
                        sess["items"].append(item.get("type"))
                elif t == "response.create":
                    sess["creates"].append(ev.get("response") or {})
                    asyncio.create_task(respond("", ev.get("response") or {}))
                elif t == "response.cancel":
                    sess["cancels"] += 1
                    st["active"] = None
        finally:
            sess["closed"] = True
