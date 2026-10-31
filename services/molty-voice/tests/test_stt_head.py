"""Every STT stream opens with its head (molty_voice/kyutai.py stream_head): Kyutai's 1 s silence
prefix, then — when the box made one — the English language anchor (an audio prompt) and 0.8 s of
silence. Live turns came back spelled in Persian / Chinese script although Juan spoke English:
stt-1b-en_fr's text stream locked into another script on the first words. The head is the stream's,
never the human's: its Steps reach neither the pause predictor nor the trace, its Words are not the
transcript nor partials, and word times stay capture-relative.
"""

import asyncio
import json

import msgpack
import numpy as np
from websockets.asyncio.server import serve

from molty_voice import framing
from molty_voice.bridge import Bridge
from molty_voice.config import Config
from molty_voice.kyutai import FRAME, FRAME_S, HEAD_MARKER, MODEL_RATE, PREFIX_STEPS, PROMPT_GAP_S, SILENCE_PREFIX_S, SttSession, stream_head
from molty_voice.trace import SttTrace

DEVICE = "molty-k7m2q9xa4c"
TOKEN = DEVICE + "." + "A" * 43
PROMPT = np.full(int(1.5 * MODEL_RATE), 0.2, dtype=np.float32)  # 1.5 s of "speech" (a constant: easy to spot)


class RecordingMoshi:
    """Records every Audio frame and answers each with a 'speaking' Step (prs[2] 0.0); the Marker 6 frames after it.

    Like moshi-server, Word times count from the stream's start: with a prompt, the prompt's own Word
    ("Okay,", inside the prompt) comes first, then the human's ("hi", 80 ms into the capture)."""

    def __init__(self, head_frames: int, prompt_end_s: float = 0.0, gap_word: bool = False):
        self.head_frames, self.prompt_end_s, self.gap_word = head_frames, prompt_end_s, gap_word
        self.frames: list[list[float]] = []

    async def handler(self, ws):
        marker, after = None, 0
        async for m in ws:
            d = msgpack.unpackb(m)
            if d["type"] == "Audio":
                if marker is None:
                    self.frames.append(d["pcm"])
                    n = len(self.frames)
                    if self.prompt_end_s and n == self.head_frames - 2:
                        await ws.send(msgpack.packb({"type": "Word", "text": "Okay,", "start_time": self.prompt_end_s - 0.5}))
                        await ws.send(msgpack.packb({"type": "EndWord", "stop_time": self.prompt_end_s - 0.2}))
                    if self.gap_word and n == self.head_frames - 1:
                        # the model completing the prompt in the gap: a word that starts after the prompt's end
                        await ws.send(msgpack.packb({"type": "Word", "text": "think.", "start_time": self.prompt_end_s + 0.1}))
                    if n == self.head_frames + 2:
                        t0 = self.head_frames * FRAME_S
                        await ws.send(msgpack.packb({"type": "Word", "text": "hi", "start_time": t0 + 0.08}))
                        await ws.send(msgpack.packb({"type": "EndWord", "stop_time": t0 + 0.16}))
                    await ws.send(msgpack.packb({"type": "Step", "step_idx": 0, "prs": [0.0, 0.0, 0.0, 0.0]}, use_single_float=True))
                else:
                    after += 1
                    if after == 6:
                        await ws.send(msgpack.packb({"type": "Marker", "id": marker}))
            elif d["type"] == "Marker" and d["id"] == HEAD_MARKER:
                await ws.send(msgpack.packb({"type": "Marker", "id": HEAD_MARKER}))  # the head is all in
            elif d["type"] == "Marker":
                marker = d["id"]


def run_session(capture_frames: int, prompt=None, gap_word=False):
    head, prompt_end = stream_head(prompt)
    moshi = RecordingMoshi(len(head) // FRAME, prompt_end, gap_word)
    out = {"partials": []}

    async def main():
        async with serve(moshi.handler, "127.0.0.1", 0) as ms:
            port = ms.sockets[0].getsockname()[1]
            trace = SttTrace()

            async def partial(text):
                out["partials"].append(text)

            s = SttSession(f"ws://127.0.0.1:{port}/api/asr-streaming", "public_token", observer=trace, on_word=partial, prompt=prompt)
            await s.open()
            # the capture goes in right behind open(), as queued relay frames do: 1280 samples @16 kHz
            # = one 80 ms model frame @24 kHz per feed
            loud = np.full(FRAME * 2 // 3, 6000, dtype="<i2").tobytes()
            for _ in range(capture_frames):
                await s.feed(loud)
            for _ in range(300):
                if len(moshi.frames) >= len(head) // FRAME + capture_frames and s.pause.steps >= capture_frames - 1:
                    break
                await asyncio.sleep(0.01)
            out["pause_steps"], out["pause_words"] = s.pause.steps, s.pause.words
            out["trace_steps"] = trace.steps
            out["text"] = await s.finish()
            out["trace"] = trace.to_json()
            await s.close()

    asyncio.run(main())
    out["frames"] = moshi.frames
    out["head_frames"] = len(head) // FRAME
    return out


def test_the_stream_opens_with_a_second_of_digital_silence_before_any_capture():
    r = run_session(capture_frames=5)
    frames = r["frames"]
    assert r["head_frames"] == PREFIX_STEPS and PREFIX_STEPS * FRAME_S >= SILENCE_PREFIX_S == 1.0
    assert all(len(f) == FRAME and not any(f) for f in frames[:PREFIX_STEPS])  # pure zeros, whole model frames
    assert all(max(abs(x) for x in f) > 0.1 for f in frames[PREFIX_STEPS + 1 : PREFIX_STEPS + 5])  # then the capture, at once
    assert r["text"] == "hi"


def test_the_prompt_goes_between_the_silence_prefix_and_the_capture():
    r = run_session(capture_frames=5, prompt=PROMPT)
    flat = np.concatenate([np.asarray(f, dtype=np.float32) for f in r["frames"]])
    pre, p = PREFIX_STEPS * FRAME, len(PROMPT)
    assert not flat[:pre].any()  # Kyutai's prefix
    assert np.allclose(flat[pre : pre + p], 0.2)  # the prompt, whole
    gap_end = r["head_frames"] * FRAME
    assert gap_end - (pre + p) >= PROMPT_GAP_S * MODEL_RATE and not flat[pre + p : gap_end].any()  # >= 0.8 s of silence
    assert np.abs(flat[gap_end + FRAME : gap_end + 4 * FRAME]).min() > 0.1  # then the capture


def test_the_head_is_not_the_humans_audio():
    # no prompt: the prefix's steps never reach the pause predictor nor the trace; word times stay capture-relative
    r = run_session(capture_frames=5)
    assert 4 <= r["pause_steps"] <= 6  # capture frames (+ the resampler tail), never + the head's
    assert r["trace_steps"] == r["pause_steps"]
    assert r["trace"]["words"][0][:3] == ["hi", 0.08, 0.16]


def test_the_prompts_own_words_are_never_the_humans():
    r = run_session(capture_frames=5, prompt=PROMPT)
    assert r["text"] == "hi"  # not "Okay, hi"
    assert r["partials"] == ["hi"]
    assert r["pause_words"] == 1
    assert 4 <= r["pause_steps"] <= 6 and r["trace_steps"] == r["pause_steps"]
    assert [w[:3] for w in r["trace"]["words"]] == [["hi", 0.08, 0.16]]


def test_a_word_the_model_adds_in_the_gap_is_the_heads_too():
    # e.g. a clipped anchor "...what I" completed with "think." during the 0.8 s gap: never the human's
    r = run_session(capture_frames=5, prompt=PROMPT, gap_word=True)
    assert r["text"] == "hi" and r["partials"] == ["hi"] and r["pause_words"] == 1


class PromptMoshi:
    """TTS: 1 s of 0.3 per request (the anchor line); STT: records the audio of each session and
    hears the anchor check (prefix + 0.3s, no head marker) as `anchor_heard`, a listen as 'yes.'"""

    def __init__(self, tts_ok=True, anchor_heard=Config().stt_prompt):
        self.tts_requests, self.stt_audio, self.tts_ok, self.anchor_heard = [], [], tts_ok, anchor_heard

    async def handler(self, ws):
        if ws.request.path.startswith("/api/tts_streaming"):
            words = []
            async for m in ws:
                d = msgpack.unpackb(m)
                if d["type"] == "Text":
                    words.append(d["text"])
                elif d["type"] == "Eos":
                    self.tts_requests.append(" ".join(words))
                    if not self.tts_ok:
                        await ws.close(1011, "tts down")
                        return
                    for _ in range(10):
                        await ws.send(msgpack.packb({"type": "Audio", "pcm": [0.3] * 2400}, use_single_float=True))
                    await ws.close()
                    return
            return
        audio, marker, after, prompted = [], None, 0, False
        self.stt_audio.append(audio)
        async for m in ws:
            d = msgpack.unpackb(m)
            if d["type"] == "Audio" and marker is None:
                audio.extend(d["pcm"])
            elif d["type"] == "Audio":
                after += 1
                if after == 6:
                    a = np.asarray(audio, dtype=np.float32)
                    anchor_check = not prompted and np.allclose(a[PREFIX_STEPS * FRAME :][:MODEL_RATE], 0.3)
                    for w in (self.anchor_heard.split() if anchor_check else ["yes."]):
                        await ws.send(msgpack.packb({"type": "Word", "text": w, "start_time": len(audio) / MODEL_RATE - 0.2}))
                    await ws.send(msgpack.packb({"type": "Marker", "id": marker}))
            elif d["type"] == "Marker" and d["id"] == HEAD_MARKER:
                prompted = True  # a listen with the anchor in its head (the check itself has none)
                await ws.send(msgpack.packb({"type": "Marker", "id": HEAD_MARKER}))
            elif d["type"] == "Marker":
                marker = d["id"]


def run_bridge(moshi: PromptMoshi, listens: int = 2):
    finals = []

    async def relay(ws):
        await ws.recv()
        await ws.send(json.dumps({"t": "voice.welcome", "device_id": DEVICE, "mode": "auto", "voice": None}))
        await asyncio.sleep(0.3)  # the anchor is made in the background meanwhile
        for k in range(listens):
            sid = f"s{k}"
            await ws.send(json.dumps({"t": "stt.start", "id": sid, "rate": 16000}))
            await ws.send(framing.encode(sid, 0, np.full(3200, 6000, dtype="<i2").tobytes()))
            await ws.send(json.dumps({"t": "stt.end", "id": sid}))
            while True:
                d = json.loads(await ws.recv())
                if d["t"] == "stt.final":
                    finals.append(d)
                    break
        await ws.close()

    async def main():
        async with serve(moshi.handler, "127.0.0.1", 0) as ms, serve(relay, "127.0.0.1", 0) as rs:
            mp, rp = ms.sockets[0].getsockname()[1], rs.sockets[0].getsockname()[1]
            cfg = Config(relay=f"http://127.0.0.1:{rp}", device_id=DEVICE, token=TOKEN,
                         stt_url=f"ws://127.0.0.1:{mp}/api/asr-streaming", tts_url=f"ws://127.0.0.1:{mp}/api/tts_streaming")
            await asyncio.wait_for(Bridge(cfg).serve_once(), 10)

    asyncio.run(main())
    return finals


def test_the_bridge_makes_the_anchor_once_with_its_tts_and_every_listen_carries_it():
    moshi = PromptMoshi()
    finals = run_bridge(moshi)
    assert moshi.tts_requests == [Config().stt_prompt]  # made once, for every listen
    assert [f.get("text") for f in finals] == ["yes.", "yes."]
    assert len(moshi.stt_audio) == 3  # the anchor's own check, then the two listens
    for audio in moshi.stt_audio[1:]:
        a = np.asarray(audio, dtype=np.float32)
        pre = PREFIX_STEPS * FRAME
        assert not a[:pre].any() and np.allclose(a[pre : pre + MODEL_RATE], 0.3)  # prefix, then the anchor


def test_without_its_tts_the_bridge_still_listens_with_the_silence_prefix():
    moshi = PromptMoshi(tts_ok=False)
    finals = run_bridge(moshi)
    assert len(moshi.tts_requests) == 1  # tried; again only after PROMPT_RETRY_S
    assert [f.get("text") for f in finals] == ["yes.", "yes."]
    for audio in moshi.stt_audio:
        a = np.asarray(audio, dtype=np.float32)
        assert not a[: PREFIX_STEPS * FRAME].any() and np.abs(a[PREFIX_STEPS * FRAME : PREFIX_STEPS * FRAME + FRAME]).max() > 0.1


def test_an_anchor_the_stt_does_not_hear_as_the_line_is_never_used():
    # a clipped / slurred TTS take ("Okay, so here is what I") would steer nothing or leak a word
    moshi = PromptMoshi(anchor_heard="Okay, so here is what I")
    finals = run_bridge(moshi)
    assert len(moshi.tts_requests) == 1 and len(moshi.stt_audio) == 3  # made, checked, dropped
    assert [f.get("text") for f in finals] == ["yes.", "yes."]
    for audio in moshi.stt_audio[1:]:
        a = np.asarray(audio, dtype=np.float32)
        cap = a[PREFIX_STEPS * FRAME : PREFIX_STEPS * FRAME + FRAME]
        assert np.abs(cap).max() > 0.1 and not np.isclose(cap, 0.3).any()  # the capture right after the prefix, no anchor
