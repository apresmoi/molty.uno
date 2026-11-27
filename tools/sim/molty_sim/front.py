"""`molty-sim front`: the voice front end to end, without the device and (by default) without a key.

  WAV (16 kHz) -> sim device --WS /v1/device--> relay (wrangler dev --local) <--WS /v1/voice-- molty-voice
                                                     ^                          (engine realtime, this machine)
  scripted agent --REST say (inner voice)------------+                                  |
                                                                       fake realtime provider (default)
                                                                       or the real one (--live)

The person shakes Molty and says the WAV; the front ends the turn and answers; when it hands the turn
to the agent, the scripted agent answers after --agent-ms through REST say (the inner voice); Molty
listens again hands-free and the conversation ends when nobody speaks. Prints each turn's activity line
(the relay's own timings) and what the sim device measured on its side.

--live: the bridge talks to the real provider with the key from THIS process's environment
(MOLTY_REALTIME_KEY or OPENAI_API_KEY): it reaches only the bridge child process, never a file.
"""

from __future__ import annotations

import asyncio
import json
import os
import shutil
import time
from pathlib import Path

import numpy as np

from . import stack
from .agent import EventPump, Rest
from .device import SimDevice, new_device_id
from .voice import Clip, Mic, read_wav, resample

ANSWER = "Your next meeting is the team standup at ten, and you have a dentist appointment at three."


def speech_end_s(pcm: np.ndarray, rate: int = 16000) -> float:
    """Where the voice in the clip ends: the last 20 ms window louder than -40 dBFS."""
    w = rate // 50
    n = len(pcm) // w
    if n == 0:
        return 0.0
    rms = np.sqrt(np.mean(pcm[: n * w].reshape(n, w).astype(np.float64) ** 2, axis=1))
    loud = np.nonzero(rms > 0.01)[0]
    return float((loud[-1] + 1) * w / rate) if loud.size else 0.0


def synthetic_question(rate: int = 16000) -> np.ndarray:
    """A stand-in for a spoken question (the fake provider only measures energy): 1.6 s of a voiced buzz, syllable shaped."""
    t = np.arange(int(1.6 * rate)) / rate
    env = 0.5 + 0.5 * np.sin(2 * np.pi * 4 * t)
    x = 0.25 * env * np.sign(np.sin(2 * np.pi * 140 * t)) * (0.6 + 0.4 * np.sin(2 * np.pi * 3 * t))
    return np.concatenate([np.zeros(int(0.3 * rate)), x, np.zeros(int(0.2 * rate))]).astype(np.float32)


def load_clip(path: str | None) -> Clip:
    if not path:
        return Clip(pcm=synthetic_question(), text="(synthetic question)")
    x, rate = read_wav(Path(path))
    return Clip(pcm=resample(x, rate, 16000) if rate != 16000 else x, text=Path(path).name)


async def front_run(wavs: list[str], live: bool, out: Path, agent_ms: int, answer: str, fake_first_audio_ms: int) -> int:
    out.mkdir(parents=True, exist_ok=True)
    procs: list[stack.Proc] = []
    voice_cfg = out / "voice.json"
    fake = None
    dev_task = None
    try:
        p, relay_http = await stack.relay(out / "wrangler", out / "relay.log")
        procs.append(p)
        device_id = new_device_id()
        fw = (stack.ROOT / "VERSION").read_text().strip()
        dev = SimDevice(relay_http.replace("http", "ws", 1), device_id, fw=fw, filler_ms=2500)
        dev_task = asyncio.create_task(dev.run())
        await asyncio.wait_for(dev.welcomed.wait(), 20)
        rest = Rest(relay_http, device_id)
        rest.token = await rest.pair(dev.link_code, "front e2e agent")
        await asyncio.to_thread(stack.pair_bridge, voice_cfg, relay_http, device_id, dev.link_code, out / "bridge.log")

        env: dict[str, str] = {}
        args = ["run", "--engine", "realtime", "--name", "front e2e"]
        if live:
            if not (os.environ.get("MOLTY_REALTIME_KEY") or os.environ.get("OPENAI_API_KEY")):
                raise SystemExit("--live needs MOLTY_REALTIME_KEY (or OPENAI_API_KEY) in this command's environment")
        else:
            from molty_voice.fake_realtime import FakeRealtime

            fake = await FakeRealtime(first_audio_ms=fake_first_audio_ms, transcript="what is on my calendar today?").__aenter__()
            args += ["--realtime-url", fake.url]
            env = {"MOLTY_REALTIME_KEY": "fake-local-key", "OPENAI_API_KEY": ""}
        bridge = stack.Proc("molty-voice", stack.bridge_cmd(voice_cfg, *args), out / "bridge.log", env=env)
        procs.append(bridge)
        await stack.wait_until(lambda: "connected to the relay" in bridge.text(), 30, "the bridge to connect", bridge)
        vs = await rest.voice_set(dev.link_code, front="on")
        print(f"relay {relay_http}, sim device {device_id}, voice front: {json.dumps(vs.get('front'))}", flush=True)
        if not (vs.get("front") or {}).get("serving"):
            raise RuntimeError(f"the front is not serving: {vs.get('front')}")

        clips = [load_clip(w) for w in (wavs or [None])]
        heard_at: dict[str, float] = {}
        order = iter(clips)

        def mic_for(job: str) -> Mic:
            c = next(order, None)
            if c is not None:
                heard_at[job] = speech_end_s(c.pcm)
            return Mic(c)

        dev.mic_for = mic_for
        pump = EventPump(rest)
        pump.start()
        answers: list[dict] = []

        async def agent() -> None:
            while True:
                got = await pump.next(60)
                if got is None:
                    return
                at, e = got
                if e.get("type") != "utterance":
                    continue
                d = e["data"]
                f = d.get("front") or {}
                print(f"  utterance {d.get('text')!r} (front said {f.get('said')!r}, waiting={f.get('waiting')})", flush=True)
                if not f.get("waiting"):
                    continue  # the front handled it: the agent stays quiet
                await asyncio.sleep(agent_ms / 1000)
                status, res = await rest.call("POST", f"/v1/devices/{device_id}/cmd", {"name": "say", "args": {"text": answer, "in_reply_to": d["turn_id"]}})
                answers.append({"at": time.monotonic(), "status": status, "job": (res.get("result") or {}).get("job")})

        agent_task = asyncio.create_task(agent())
        t0 = time.monotonic()
        await dev.shake()
        # the conversation is over when the relay logs front.end
        ended = None
        for _ in range(240):
            await asyncio.sleep(0.5)
            entries = await activity(rest, dev.link_code)
            ended = next((e for e in entries if e.get("ev") == "front.end"), None)
            if ended:
                break
        agent_task.cancel()
        await pump.stop()
        entries = await activity(rest, dev.link_code)
        print(f"\nconversation ended: {json.dumps((ended or {}).get('data'))} after {time.monotonic() - t0:.1f} s", flush=True)
        print("\nrelay, per turn (activity log):")
        for e in entries:
            if e.get("kind") == "trace" and e.get("ev") == "front":
                print("  " + e["data"]["line"])
                print("    ms after the person stopped: " + json.dumps({k: v for k, v in e["data"].items() if k not in ("line", "waiting")}))
        for e in entries:
            if e.get("kind") == "voice_box" and e.get("ev") == "front.session" and e["data"].get("state") == "closed":
                print(f"  box session closed ({e['data'].get('reason')}): {e['data'].get('s')} s, usage {json.dumps(e['data'].get('usage'))}")
        print("\ndevice side (the sim's clock):")
        for rec in {id(r): r for r in dev.listens.values()}.values():
            job = rec.local or rec.job
            if job not in heard_at or not rec.mic_open_at:
                continue
            stopped = rec.mic_open_at + heard_at[job] - (rec.trim_ms or 0) / 1000
            says = sorted((s for s in dev.says.values() if s.play_start and s.play_start > stopped), key=lambda s: s.play_start)
            line = f"  listen {job[-8:]}: listen_stop {((rec.stop_at or 0) - stopped) * 1000:.0f} ms after the voice ended"
            if says:
                line += f"; Molty's first sound {(says[0].play_start - stopped) * 1000:.0f} ms after it ({says[0].text!r})"
            if len(says) > 1:
                line += f"; the agent's answer {(says[1].play_start - stopped) * 1000:.0f} ms ({says[1].text[:40]!r}…)"
            print(line)
        if fake is not None:
            s = fake.sessions
            print(f"\nfake provider: {len(s)} session(s); appended {sum(x['appended'] for x in s) / 24000:.1f} s of 24 kHz audio; "
                  f"{sum(len(x['creates']) for x in s)} explicit responses; all closed: {all(x['closed'] for x in s)}")
        print(f"\nlogs: {out}")
        return 0 if ended else 1
    finally:
        if dev_task:
            dev_task.cancel()
        for pr in reversed(procs):
            pr.stop()
        if fake is not None:
            await fake.__aexit__(None, None, None)
        voice_cfg.unlink(missing_ok=True)  # the voice token
        shutil.rmtree(out / "wrangler", ignore_errors=True)


async def activity(rest: Rest, link: str) -> list[dict]:
    from .agent import http

    _, outj = await http("GET", f"{rest.base}/v1/devices/{rest.device}/activity?limit=500", headers={"X-Molty-Link-Code": link})
    return ((outj.get("result") or {}).get("entries")) or []
