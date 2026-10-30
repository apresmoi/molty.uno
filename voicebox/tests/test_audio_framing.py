import json

import numpy as np
import pytest

from molty_voice import framing
from molty_voice.audio import Resampler, float_to_pcm16, pcm16_to_float


def tone(freq: float, rate: int, seconds: float = 0.5, amp: float = 0.5) -> np.ndarray:
    t = np.arange(int(rate * seconds)) / rate
    return (amp * np.sin(2 * np.pi * freq * t)).astype(np.float32)


def peak_freq(x: np.ndarray, rate: int) -> float:
    spec = np.abs(np.fft.rfft(x * np.hanning(len(x))))
    return float(np.fft.rfftfreq(len(x), 1 / rate)[np.argmax(spec)])


def run(rs: Resampler, x: np.ndarray, sizes) -> np.ndarray:
    out, i, k = [], 0, 0
    while i < len(x):
        n = sizes[k % len(sizes)]
        out.append(rs.process(x[i : i + n]))
        i += n
        k += 1
    out.append(rs.flush())
    return np.concatenate(out)


@pytest.mark.parametrize("src,dst", [(16000, 24000), (24000, 16000)])
def test_resampler_length_and_tone(src, dst):
    x = tone(440, src)
    y = run(Resampler(src, dst), x, [len(x)])
    assert len(y) == len(x) * dst // src
    assert abs(peak_freq(y, dst) - 440) < 5
    # amplitude kept (away from the edges)
    mid = y[len(y) // 4 : 3 * len(y) // 4]
    assert 0.45 < np.max(np.abs(mid)) < 0.55


@pytest.mark.parametrize("src,dst", [(16000, 24000), (24000, 16000)])
def test_resampler_streaming_equals_one_shot(src, dst):
    x = tone(300, src, 0.3) + tone(1700, src, 0.3, 0.2)
    whole = run(Resampler(src, dst), x, [len(x)])
    chunked = run(Resampler(src, dst), x, [1, 7, 1920, 333, 4096])
    assert len(whole) == len(chunked)
    np.testing.assert_allclose(whole, chunked, atol=1e-5)


def test_downsampling_removes_what_16k_cannot_carry():
    # 10 kHz is above 16 kHz's Nyquist: it must be filtered out, not alias to 6 kHz
    y = run(Resampler(24000, 16000), tone(10000, 24000), [1920])
    assert np.max(np.abs(y[200:-200])) < 0.01


def test_pcm16_roundtrip_and_clip():
    x = np.array([0.0, 0.5, -0.5, 1.5, -1.5], dtype=np.float32)
    b = float_to_pcm16(x)
    assert len(b) == 10
    y = pcm16_to_float(b)
    np.testing.assert_allclose(y[:3], x[:3], atol=1e-4)
    assert y[3] == pytest.approx(32767 / 32768) and y[4] == -1.0
    assert len(pcm16_to_float(b + b"\x01")) == 5  # a dangling byte is dropped


def test_frame_roundtrip_matches_relay_format():
    f = framing.encode("s1abc", 3, b"\x01\x02\x03\x04")
    head, _, payload = f.partition(b"\n")
    assert json.loads(head) == {"s": "s1abc", "seq": 3, "kind": "pcm16", "rate": 16000, "end": False}
    assert payload == b"\x01\x02\x03\x04"
    d = framing.decode(f)
    assert (d.s, d.seq, d.end, d.payload) == ("s1abc", 3, False, b"\x01\x02\x03\x04")
    # header-only end frame (no newline), as the device sends it
    e = framing.decode(b'{"s":"x","seq":9,"kind":"pcm16","rate":16000,"end":true}')
    assert e.end and e.payload == b""


@pytest.mark.parametrize(
    "bad",
    [
        b'{"s":"x","seq":0,"kind":"pcm16","rate":24000}\n\x00\x00',
        b'{"s":"","seq":0,"kind":"pcm16","rate":16000}\n',
        b'{"s":"x","seq":-1,"kind":"pcm16","rate":16000}\n',
        b'{"s":"x","seq":0,"kind":"pcm16","rate":16000,"end":"yes"}\n',
        b"not json\n",
        b"x" * 600,
    ],
)
def test_frame_rejects(bad):
    with pytest.raises(framing.FrameError):
        framing.decode(bad)


def test_frame_limits_and_chunking():
    with pytest.raises(framing.FrameError):
        framing.encode("x", 0, b"\x00" * (16 * 1024 + 2))
    pcm = b"\x01\x00" * 20000  # 40000 B -> 16384 + 16384 + 7232
    frames = [framing.decode(f) for f in framing.chunks("id", pcm, 5)]
    assert [len(f.payload) for f in frames] == [16384, 16384, 7232]
    assert [f.seq for f in frames] == [5, 6, 7]
    assert b"".join(f.payload for f in frames) == pcm
