"""Word onsets in audio, and matching words to a text.

Ground truth for say-cue sync is forced alignment (torchaudio MMS_FA), run on the box's CPU over
ssh (`mms_remote.py`, the venv that already has torchaudio + the model) — offline after a run,
never during a live listen. The box STT's `Word.start_time` is NOT usable for this: measured
here it lands ~ at the NEXT word's onset (one word late, so its error depends on word length).
`stt_words` stays for transcripts only.

Self-check: the human's clips have known onsets (Kyutai TTS timing less its one step,
validated at +5 ms mean against the same aligner), so aligning them too gives the aligner's
offset on this run's audio (`selfcheck` in the report).
"""

from __future__ import annotations

import asyncio
import io
import json
import re
import subprocess
import tempfile
import difflib
from pathlib import Path

import msgpack
import numpy as np
from websockets.asyncio.client import connect as ws_connect

from molty_voice.audio import DEVICE_RATE, MODEL_RATE, Resampler
from molty_voice.kyutai import FRAME, _headers, _pack

NUMBERS = {"0": "zero", "1": "one", "2": "two", "3": "three", "4": "four", "5": "five", "6": "six", "7": "seven",
           "8": "eight", "9": "nine", "10": "ten", "11": "eleven", "12": "twelve", "20": "twenty", "30": "thirty"}
FILLERS = frozenset({"um", "uh", "uhm", "umm", "erm", "er", "hmm", "mm", "ah", "eh", "mhm"})


def norm(w: str) -> str:
    w = w.lower().replace("’", "'")
    w = re.sub(r"[^a-z0-9']", "", w).strip("'")
    return NUMBERS.get(w, w)


def mms_word(w: str) -> str:
    """The aligner's alphabet: a-z and ' only (digits spelled out, the rest dropped)."""
    return re.sub(r"[^a-z']", "", norm(w))


def match(ref: list[str], hyp: list[str]) -> list[tuple[int, int]]:
    """Index pairs (ref_i, hyp_j) of words equal after normalisation, in order."""
    a, b = [norm(w) for w in ref], [norm(w) for w in hyp]
    sm = difflib.SequenceMatcher(a=a, b=b, autojunk=False)
    return [(blk.a + k, blk.b + k) for blk in sm.get_matching_blocks() for k in range(blk.size) if a[blk.a + k]]


def mms_align(items: dict[str, tuple[np.ndarray, list[str]]], host: str, python: str, timeout: float = 900) -> dict[str, list]:
    """{id: (int16 pcm @16 kHz, words)} -> {id: [[start_s, end_s] | None per word]}, aligned on `host` over ssh."""
    if not items:
        return {}
    ids = list(items)
    arrays = {"ids": np.array(json.dumps(ids))}
    for i, k in enumerate(ids):
        pcm, words = items[k]
        arrays[f"pcm_{i}"] = np.asarray(pcm, dtype=np.int16)
        arrays[f"words_{i}"] = np.array(json.dumps([mms_word(w) for w in words]))
    buf = io.BytesIO()
    np.savez(buf, **arrays)
    remote = "/tmp/molty-sim-align-" + next(tempfile._get_candidate_names())  # noqa: SLF001 - just a unique name
    script = (Path(__file__).parent / "mms_remote.py").read_bytes()
    try:
        subprocess.run(["ssh", host, f"mkdir -p {remote} && cat > {remote}/in.npz"], input=buf.getvalue(), check=True, timeout=120)
        subprocess.run(["ssh", host, f"cat > {remote}/align.py"], input=script, check=True, timeout=60)
        r = subprocess.run(["ssh", host, f"{python} {remote}/align.py {remote}/in.npz"], capture_output=True, check=True, timeout=timeout)
    finally:
        subprocess.run(["ssh", host, f"rm -rf {remote}"], timeout=60, check=False)
    return json.loads(r.stdout)


async def stt_words(url: str, pcm16: np.ndarray, api_key: str = "public_token", timeout: float = 30.0) -> list[tuple[str, float]]:
    """[(word, start_time_s)] for int16 @16 kHz audio via moshi-server's streaming STT (faster than real time)."""
    x = pcm16.astype(np.float32) / 32768.0
    rs = Resampler(DEVICE_RATE, MODEL_RATE)
    y = np.concatenate([rs.process(x), rs.flush(), np.zeros(int(MODEL_RATE * 0.5), np.float32)])
    words: list[tuple[str, float]] = []
    async with ws_connect(url, additional_headers=_headers(api_key), max_size=None, ping_interval=None) as ws:
        done = asyncio.Event()

        async def recv():
            async for m in ws:
                d = msgpack.unpackb(m, raw=False)
                t = d.get("type") if isinstance(d, dict) else None
                if t == "Word":
                    words.append((str(d.get("text", "")), float(d.get("start_time", 0.0))))
                elif t == "Marker":
                    done.set()
                    return

        rt = asyncio.create_task(recv())
        n = (len(y) // FRAME) * FRAME
        for i in range(0, n, FRAME):
            await ws.send(_pack({"type": "Audio", "pcm": y[i : i + FRAME].tolist()}))
        await ws.send(_pack({"type": "Marker", "id": 1}))
        silence = np.zeros(FRAME, np.float32).tolist()
        for _ in range(int(6.0 / (FRAME / MODEL_RATE))):
            if done.is_set():
                break
            await ws.send(_pack({"type": "Audio", "pcm": silence}))
            await asyncio.sleep(0)
        await asyncio.wait_for(done.wait(), timeout)
        rt.cancel()
    return words


def cue_errors(text: str, cues: list[tuple[int, int]], onsets: list) -> list[dict]:
    """Per cued word: cue time vs the word's aligned onset in the received audio. + = the cue is late."""
    ref = text.split()
    out = []
    for word, sample in cues:
        if word >= len(ref) or word >= len(onsets) or onsets[word] is None:
            continue
        onset = onsets[word][0]
        cue = sample / DEVICE_RATE
        out.append({"i": word, "word": ref[word], "cue_s": round(cue, 3), "onset_s": round(onset, 3), "err_ms": round((cue - onset) * 1000)})
    return out
