"""~/.config/molty-voice/config.json (mode 0600): relay, device, the voice token, endpoints."""

from __future__ import annotations

import json
import os
from dataclasses import asdict, dataclass, field
from pathlib import Path

DEFAULT_VOICE = "expresso/ex03-ex01_happy_001_channel1_334s.wav"
# Voices in Kyutai's kyutai/tts-voices repo that the box has cached (the console offers these).
DEFAULT_VOICES = [
    DEFAULT_VOICE,
    "expresso/ex04-ex02_happy_001_channel1_118s.wav",
    "expresso/ex03-ex01_calm_001_channel2_1081s.wav",
    "unmute-prod-website/default_voice.wav",
]


def default_path() -> Path:
    base = os.environ.get("XDG_CONFIG_HOME") or str(Path.home() / ".config")
    return Path(base) / "molty-voice" / "config.json"


@dataclass
class Config:
    relay: str = ""
    device_id: str = ""
    token: str = ""
    name: str = "voice box"
    stt_url: str = "ws://127.0.0.1:8090/api/asr-streaming"
    tts_url: str = "ws://127.0.0.1:8089/api/tts_streaming"
    api_key: str = "public_token"
    default_voice: str = DEFAULT_VOICE
    voices: list[str] = field(default_factory=lambda: list(DEFAULT_VOICES))
    stt: bool = True
    tts: bool = True
    # Seconds of silence at most pushed after the marker to flush the STT model's delay.
    stt_flush_s: float = 6.0
    # Semantic end of turn (Unmute's pause rule): smoothed prs[2] above this, after >= 1
    # word, sends stt.turn_end so Molty stops listening. <= 0 turns it off.
    turn_end: float = 0.6
    # ...ignoring the first N model steps (80 ms each) of a listen, where prs[2] is unreliable (Unmute: 12).
    turn_end_skip_steps: int = 12
    # TTS: drop the near-silent lead of each stream's first frame (<= 80 ms) so the voice starts sooner.
    trim_lead: bool = True

    def __repr__(self) -> str:  # never show the token
        d = asdict(self)
        d["token"] = "<set>" if self.token else ""
        return f"Config({d})"


def load(path: Path | None = None) -> Config:
    p = path or default_path()
    if not p.exists():
        return Config()
    raw = json.loads(p.read_text())
    known = {k: v for k, v in raw.items() if k in Config.__dataclass_fields__}
    return Config(**known)


def save(cfg: Config, path: Path | None = None) -> Path:
    """Writes the config readable by its owner only (0600, directory 0700)."""
    p = path or default_path()
    p.parent.mkdir(parents=True, exist_ok=True)
    os.chmod(p.parent, 0o700)
    tmp = p.with_suffix(".tmp")
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(asdict(cfg), f, indent=2)
        f.write("\n")
    os.chmod(tmp, 0o600)
    os.replace(tmp, p)
    return p
