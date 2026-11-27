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
import time

import numpy as np
from websockets.asyncio.server import serve

RATE = 24000
_log = logging.getLogger("molty_voice.fake_realtime")
_log.setLevel(logging.WARNING)  # its handshake log would show the Authorization header it was sent


class FakeRealtime:
    def __init__(self, *, first_audio_ms: int = 300, speed: float = 3.0, silence_ms: int = 500, transcript: str | None = None,
                 reply=None, threshold: float = 0.02, final=None, script=None, auto_create_ms: int = 0, recite: bool = True,
                 commit_event_ms: int = 0, ends_together: bool = False, misread=None, hold=None):
        self.first_audio_ms = first_audio_ms
        self.speed = speed  # audio is generated this many times faster than real time
        self.silence_ms = silence_ms
        self.transcript = transcript  # what the "person" said (None: "hello molty")
        # (transcript) -> (words to say, call hand_to_agent?)
        # a question it cannot answer: a holding line echoing the person's words, handed to the agent (brief.BRIEF)
        self.reply = reply or (lambda heard: (f"Ok, let me see... {heard.rstrip('?')}, one sec.", True) if "?" in heard or "what" in heard.lower() else ("Hi there!", False))
        # a scripted conversation: each committed turn takes the next {heard, said, handoff} (then transcript / reply)
        self.script = list(script or [])
        self.turn = 0
        # as a model seen live: an auto response (create_response) comes this late, and turn detection
        # switched off before it (session.update turn_detection null) loses it
        self.auto_create_ms = auto_create_ms
        # as a model seen live: background sent as a system item ("Inner voice context: ...") is said with the next answer
        self.recite = recite
        # the turn detection commits at once, but its `committed` event reaches the client this late (2 ms live:
        # enough for the device's end of the same turn to arrive first). A commit of an empty buffer is an error.
        self.commit_event_ms = commit_event_ms
        # the device's end and the model's own end of one turn together: when turn detection is switched off
        # mid speech, the model has just ended the turn itself (speech_stopped, its commit) before that
        self.ends_together = ends_together
        # a read-aloud request (out of band, its own input): what the model says for the text (live: it changed facts)
        self.misread = misread or (lambda text: text)
        # a holding line asked for on the person's words (out of band): what the model says
        self.hold = hold or (lambda heard: f"Okay, {' '.join(heard.split()[:3])}, let me check.")
        self.final = final or (lambda words: words)  # the output transcript's done event (a model may restate it)
        self.threshold = threshold
        self.sessions: list[dict] = []
        # what happened when, for a rig's checks: open / close {active} / create {kind, speaking}
        self.log: list[dict] = []
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
        self.log.append({"t": time.monotonic(), "ev": "open"})
        # vad: the turn detection's settings (None: off, push to talk). Its speech state lives across
        # commits and clears, as a real detector's does: only a new turn_detection resets it.
        st = {"buf": [], "speech": False, "quiet": 0, "audio_ms": 0, "start_ms": 0, "active": None, "n": 0, "create": True, "vad": None,
              "heard": "", "auto": None, "recited": 0, "pending_ms": 0}
        lock = asyncio.Lock()

        async def send(ev):
            async with lock:
                await ws.send(json.dumps(ev))

        def nid(p):
            st["n"] += 1
            return f"{p}_{st['n']}"

        async def commit(by_vad: bool, event_id: str | None = None):
            if not by_vad and st["pending_ms"] < 100:
                sess["empty_commits"] = sess.get("empty_commits", 0) + 1
                await send({"type": "error", "error": {"type": "invalid_request_error", "code": "input_audio_buffer_commit_empty", "event_id": event_id,
                                                       "message": f"buffer too small. Expected at least 100ms of audio, but buffer only has {st['pending_ms']}ms of audio."}})
                return
            item = nid("item")
            if by_vad:
                st["speech"], st["quiet"] = False, 0
            sess["commits"] += 1
            st["buf"], st["pending_ms"] = [], 0
            if by_vad and self.commit_event_ms:
                await asyncio.sleep(self.commit_event_ms / 1000)
            await send({"type": "input_audio_buffer.committed", "item_id": item, "previous_item_id": None})
            turn = self.script[self.turn] if self.turn < len(self.script) else None
            self.turn += 1
            heard = turn["heard"] if turn else self.transcript if self.transcript is not None else "hello molty"
            delay = (turn or {}).get("auto_create_ms", self.auto_create_ms)
            st["heard"] = heard
            if by_vad and st["create"]:  # a client's own commit never makes a response by itself

                async def auto():
                    await asyncio.sleep(delay / 1000)
                    st["auto"] = None
                    await respond(heard, None)
                st["auto"] = asyncio.create_task(auto())

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
            given = [c.get("text", "") for i in (req or {}).get("input") or [] for c in i.get("content") or [] if c.get("type") == "input_text"]
            kind = ("auto" if req is None else "inner" if given else "reply" if not instr
                    else "wait" if "still working" in instr else "plain")
            self.log.append({"t": time.monotonic(), "ev": "create", "kind": kind, "speaking": st["speech"]})
            m = re.search(r"word for word, and nothing else: (.*)$", instr, re.S)
            tool = False
            if given and "holding line" in instr:
                words = self.hold(" ".join(given).strip())
            elif given:
                words = self.misread(" ".join(given).strip())
            elif m:
                words = m.group(1).strip()
            elif "still working on the person's request" in instr:
                words = "Still on it, one moment."
            elif req is not None and sess["items"] and sess["items"][-1].startswith("Inner voice: "):
                words = "Okay, that's a neat vibe, let me translate that briefly."  # what a model did with a bare system message
            else:
                heard = heard or st["heard"]
                turn = next((t for t in self.script if t["heard"] == heard), None)
                words, tool = (turn["said"], bool(turn.get("handoff"))) if turn else self.reply(heard)
            if self.recite and instr and not given:
                pending = [i[len("Inner voice context: "):] for i in sess["items"][st["recited"]:] if isinstance(i, str) and i.startswith("Inner voice context: ")]
                st["recited"] = len(sess["items"])
                words = " ".join(pending + [words]).strip()
            await asyncio.sleep(self.first_audio_ms / 1000)
            n = max(1, len(words.split()))
            total = int(RATE * 0.3 * n) if words else 0  # 300 ms a word
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
            await send({"type": "response.output_audio_transcript.done", "response_id": rid, "transcript": self.final(words)})
            out = [{"type": "function_call", "name": tool if isinstance(tool, str) else "hand_to_agent", "call_id": "call_1", "arguments": "{}"}] if tool else []
            await send({"type": "response.done", "response": {"id": rid, "status": "cancelled" if cancelled else "completed", "metadata": meta, "output": out,
                                                              "usage": {"input_token_details": {"audio_tokens": st["audio_ms"] // 100, "text_tokens": 300, "cached_tokens_details": {"audio_tokens": 0}},
                                                                        "output_token_details": {"audio_tokens": sent * 20 // RATE, "text_tokens": n}}}})

        try:
            async for raw in ws:
                ev = json.loads(raw)
                t = ev.get("type")
                if t == "session.update":
                    sess["updates"].append(ev["session"])
                    inp = (ev["session"].get("audio") or {}).get("input") or {}
                    if "turn_detection" in inp:
                        td = inp["turn_detection"]
                        was_speaking = st["vad"] is not None and st["speech"]
                        st["vad"], st["speech"], st["quiet"] = td, False, 0
                        st["create"] = bool(td.get("create_response", True)) if isinstance(td, dict) else False
                        if td is None and self.ends_together and was_speaking:
                            await send({"type": "input_audio_buffer.speech_stopped", "audio_end_ms": st["audio_ms"], "item_id": "pending"})
                            st["pending_ms"] = 0
                            asyncio.create_task(commit(True))
                        if td is None and st["auto"] is not None:  # turned off before its auto response: lost
                            st["auto"].cancel()
                            st["auto"] = None
                    await send({"type": "session.updated", "session": ev["session"]})
                elif t == "input_audio_buffer.append":
                    pcm = np.frombuffer(base64.b64decode(ev["audio"]), dtype="<i2").astype(np.float32) / 32768
                    sess["appended"] += len(pcm)
                    for i in range(0, len(pcm), RATE // 50):  # 20 ms steps
                        w = pcm[i : i + RATE // 50]
                        st["audio_ms"] += 20
                        st["pending_ms"] += 20
                        if st["vad"] is None:
                            continue
                        loud = float(np.sqrt(np.mean(w * w))) > self.threshold if w.size else False
                        if loud and not st["speech"]:
                            st["speech"], st["quiet"] = True, 0
                            await send({"type": "input_audio_buffer.speech_started", "audio_start_ms": st["audio_ms"], "item_id": "pending"})
                        elif st["speech"]:
                            st["quiet"] = 0 if loud else st["quiet"] + 20
                            if st["quiet"] >= self.silence_ms:
                                await send({"type": "input_audio_buffer.speech_stopped", "audio_end_ms": st["audio_ms"], "item_id": "pending"})
                                st["pending_ms"], st["speech"], st["quiet"] = 0, False, 0  # committed now, server side; its event may come later
                                asyncio.create_task(commit(True))
                elif t == "input_audio_buffer.commit":
                    await commit(False, ev.get("event_id"))
                elif t == "input_audio_buffer.clear":
                    sess["clears"] += 1
                    st["buf"], st["pending_ms"] = [], 0
                elif t == "conversation.item.create":
                    item = ev.get("item") or {}
                    if item.get("type") == "message":
                        sess["items"].append(item["content"][0]["text"])
                    else:
                        sess["items"].append(item.get("type"))
                elif t == "response.create":
                    sess["creates"].append(ev.get("response") or {})
                    asyncio.create_task(respond("", ev.get("response") or {}))
                elif t == "conversation.item.delete":
                    sess.setdefault("deleted", []).append(ev.get("item_id"))
                elif t == "response.cancel":
                    sess["cancels"] += 1
                    st["active"] = None
        finally:
            sess["closed"] = True
            self.log.append({"t": time.monotonic(), "ev": "close", "active": st["active"] is not None})
