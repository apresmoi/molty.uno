"""Relay audio frames: `<JSON header>\\n<payload>` (docs/AGENT-LINK.md, "Device <-> relay wire").

Header {"s": id, "seq": n, "kind": "pcm16", "rate": 16000, "end": bool}, header <= 512 B,
payload <= 16 KB of PCM16 mono @16 kHz. Same rules as services/relay/src/core/protocol.ts.
"""

from __future__ import annotations

import json
from dataclasses import dataclass

MAX_HEADER = 512
MAX_PAYLOAD = 16 * 1024
RATE = 16000


class FrameError(ValueError):
    pass


@dataclass
class Frame:
    s: str
    seq: int
    end: bool
    payload: bytes


def encode(s: str, seq: int, payload: bytes, end: bool = False) -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise FrameError("audio payload exceeds 16 KB")
    head = json.dumps({"s": s, "seq": seq, "kind": "pcm16", "rate": RATE, "end": end}, separators=(",", ":")).encode()
    if len(head) > MAX_HEADER:
        raise FrameError("audio header exceeds 512 B")
    return head + b"\n" + payload


def decode(data: bytes) -> Frame:
    nl = data.find(b"\n", 0, MAX_HEADER + 1)
    if nl < 0 and len(data) > MAX_HEADER:
        raise FrameError("no header end")
    head, payload = (data, b"") if nl < 0 else (data[:nl], data[nl + 1 :])
    try:
        h = json.loads(head)
    except ValueError as e:
        raise FrameError("bad header") from e
    if not isinstance(h, dict):
        raise FrameError("bad header")
    s, seq = h.get("s"), h.get("seq")
    if not isinstance(s, str) or not 0 < len(s) <= 128:
        raise FrameError("bad stream id")
    if not isinstance(seq, int) or isinstance(seq, bool) or seq < 0:
        raise FrameError("bad seq")
    if h.get("kind") != "pcm16" or h.get("rate") != RATE:
        raise FrameError("not pcm16 @16 kHz")
    end = h.get("end", False)
    if not isinstance(end, bool):
        raise FrameError("bad end")
    if len(payload) > MAX_PAYLOAD:
        raise FrameError("payload exceeds 16 KB")
    return Frame(s=s, seq=seq, end=end, payload=payload)


def chunks(s: str, pcm: bytes, start_seq: int = 0, end: bool = False):
    """Splits PCM16 bytes into <= 16 KB frames (whole samples); yields encoded frames."""
    seq = start_seq
    step = MAX_PAYLOAD
    for i in range(0, len(pcm), step):
        last = i + step >= len(pcm)
        yield encode(s, seq, pcm[i : i + step], end and last)
        seq += 1
    if not pcm and end:
        yield encode(s, seq, b"", True)
