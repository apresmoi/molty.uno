"""One end-to-end run: stack up, pair, play the scenario as a conversation, align, measure, report."""

from __future__ import annotations

import asyncio
import base64
import json
import logging
import os
import re
import time
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from . import align, report, stack
from .agent import EventPump, LlmAgent, RelayError, Rest, ScriptedAgent
from .device import CUE_LEAD_S, SimDevice, new_device_id
from .metrics import ShakeFacts, TurnFacts, drafts_summary, foreign_script, shake_metrics, summarize, turn_metrics, wer
from .scenario import Scenario
from .voice import Clip, Mic, Synth, write_wav

log = logging.getLogger("molty_sim")
SIM_DIR = Path(__file__).resolve().parents[1]


@dataclass
class Options:
    scenario: Scenario
    runs_dir: Path = SIM_DIR / "runs"
    cache_dir: Path = SIM_DIR / ".cache"
    state_dir: Path = SIM_DIR / "state"
    box: str = "voicebox"
    tunnel: bool = True
    tts_port: int = 18089
    stt_port: int = 18090
    relay: str | None = None  # None: a local wrangler dev
    allow_remote: bool = False
    bridge: bool = True  # run our own molty-voice for the sim device
    tuning: list[str] = field(default_factory=list)  # passed to `molty-voice run`
    agent: str = "scripted"
    agent_gap_ms: int = 0
    filler_ms: int | None = 2500  # the sim device's auto filler setting (0 = off; None = not reported, like older firmware)
    llm_url: str = "http://127.0.0.1:11434"
    llm_model: str = "llama3.2:3b"
    align: str = "mms"  # mms | none
    align_python: str = "python3"
    keep_audio: bool = True
    only: list[str] | None = None  # turn ids
    drafter: str = "careful"  # shake turns: careful (no draft once a turn-around word was heard) | naive

    @property
    def tts_url(self) -> str:
        return f"ws://127.0.0.1:{self.tts_port}/api/tts_streaming"

    @property
    def stt_url(self) -> str:
        return f"ws://127.0.0.1:{self.stt_port}/api/asr-streaming"


def _ws(http: str) -> str:
    return http.replace("https://", "wss://").replace("http://", "ws://")


def _remote_identity(state: Path, relay: str) -> tuple[str, bytes]:
    """The sim device for a deployed relay: one persisted id/key per relay host (never a real device's)."""
    state.mkdir(parents=True, exist_ok=True)
    os.chmod(state, 0o700)
    f = state / (relay.split("//")[-1].split("/")[0].replace(":", "_") + ".device.json")
    if f.exists():
        d = json.loads(f.read_text())
        return d["device_id"], base64.b64decode(d["key"])
    did, key = new_device_id(), os.urandom(32)
    fd = os.open(f, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as fh:
        json.dump({"device_id": did, "key": base64.b64encode(key).decode()}, fh)
    return did, key


async def run(o: Options) -> Path:
    ts = time.strftime("%Y%m%d-%H%M%S")
    out = o.runs_dir / ts
    (out / "audio").mkdir(parents=True, exist_ok=True)
    procs: list[stack.Proc] = []
    voice_cfg = out / "voice.json"
    dev_task = None
    try:
        # ---- the stack ----
        if o.tunnel:
            t = await stack.tunnel(o.box, o.tts_port, o.stt_port, out / "tunnel.log")
            if t:
                procs.append(t)
        sc = o.scenario
        turns = [t for t in sc.turns if not o.only or t.id in o.only]
        synth = Synth(o.tts_url, o.cache_dir / "user")
        print(f"synthesising {len(turns)} user turns (cached after the first run)…", flush=True)
        clips: dict[str, Clip] = {}
        for t in turns:
            clips[t.id] = await synth.clip(t)

        if o.relay:
            if not o.allow_remote:
                raise SystemExit("a deployed relay needs --allow-remote (the sim then uses its own persisted molty-sim… device)")
            relay_http = o.relay.rstrip("/")
            device_id, key = _remote_identity(o.state_dir, relay_http)
        else:
            p, relay_http = await stack.relay(out / "wrangler", out / "relay.log")
            procs.append(p)
            device_id, key = new_device_id(), None
        fw = (stack.ROOT / "VERSION").read_text().strip()
        dev = SimDevice(_ws(relay_http), device_id, key, fw=fw, filler_ms=o.filler_ms)
        dev_task = asyncio.create_task(dev.run())
        await asyncio.wait_for(dev.welcomed.wait(), 20)
        print(f"sim device {device_id} welcomed by {'the local relay' if not o.relay else relay_http}", flush=True)

        rest = Rest(relay_http, device_id)
        rest.token = await rest.pair(dev.link_code, "molty-sim agent")
        if o.bridge:
            await asyncio.to_thread(stack.pair_bridge, voice_cfg, relay_http, device_id, dev.link_code, out / "bridge.log")
            procs.append(await stack.bridge(voice_cfg, out / "bridge.log", o.stt_url, o.tts_url, o.tuning))
        for _ in range(50):
            vs = await rest.voice_status(dev.link_code)
            if (vs.get("box") or {}).get("connected"):
                break
            await asyncio.sleep(0.2)
        else:
            raise RuntimeError(f"the voice box never showed as connected: {vs}")
        print(f"voice box connected (mode {vs.get('mode')}); bridge tuning: {' '.join(o.tuning) or 'defaults'}", flush=True)

        if sc.shake:
            return await shake_run(o, out, ts, dev, rest, turns, clips, fw, device_id, relay_http)
        agent = (LlmAgent(o.llm_url, o.llm_model, o.agent_gap_ms) if o.agent == "llm" else ScriptedAgent(o.agent_gap_ms))
        rows = await conversation(dev, rest, agent, turns, clips, sc.closing)
        await asyncio.sleep(0.5)

        # ---- offline: alignment, metrics ----
        cue_errs, selfcheck = await alignment(o, dev, rows, turns, clips)
        for r in rows:
            r.pop("_say", None)
            r.pop("_listen", None)
            r.pop("_facts", None)
        summary = summarize(rows, cue_errs)
        summary["alignment_selfcheck_ms"] = selfcheck
        meta = {"run": ts, "scenario": sc.name, "relay": "local wrangler dev" if not o.relay else relay_http, "device": device_id,
                "fw": fw, "caps": dev.caps, "bridge_tuning": o.tuning, "agent": agent.name, "agent_gap_ms": o.agent_gap_ms, "filler_ms": o.filler_ms,
                "align": o.align, "box": o.box}
        if o.keep_audio:
            for t in turns:
                c = clips[t.id]
                m = Mic(c)
                write_wav(out / "audio" / f"{t.index:02d}-{t.id}.user.wav", m.read(len(c.pcm) + 16000))
            for n, s in enumerate(dev.says.values()):  # in order: turn n's say (the last one: the closing)
                write_wav(out / "audio" / f"{n:02d}-say.wav", s.audio())
        report.write(out, meta, rows, summary)
        print(report.table(rows, summary), flush=True)
        print(f"\nreport: {out / 'report.md'}", flush=True)
        return out
    finally:
        if dev_task:
            await dev.close()
            dev_task.cancel()
        for p in reversed(procs):
            p.stop()
        if voice_cfg.exists():  # the voice token never outlives the run
            voice_cfg.unlink()


async def shake_run(o: Options, out: Path, ts: str, dev: SimDevice, rest: Rest, turns, clips, fw: str, device_id: str, relay_http: str) -> Path:
    """A shake scenario: every turn once per arm, then the drafts report (no say-cue alignment: not measured here)."""
    pump = EventPump(rest)
    pump.start()
    try:
        rows = await shake_conversation(dev, rest, pump, turns, clips, o.scenario.arms, o.agent_gap_ms, o.drafter)
    finally:
        await pump.stop()
    speech = await rest.speech(dev.link_code)
    summary = drafts_summary(rows)
    summary["calibration"] = speech
    meta = {"run": ts, "scenario": o.scenario.name, "relay": "local wrangler dev" if not o.relay else relay_http, "device": device_id,
            "fw": fw, "caps": dev.caps, "bridge_tuning": o.tuning, "agent": f"scripted drafter ({o.drafter})", "agent_gap_ms": o.agent_gap_ms,
            "arms": o.scenario.arms, "box": o.box}
    if o.keep_audio:
        for t in turns:
            c = clips[t.id]
            write_wav(out / "audio" / f"{t.index:02d}-{t.id}.user.wav", Mic(c).read(len(c.pcm) + 16000))
    report.write_shake(out, meta, rows, summary)
    print(report.shake_table(rows, summary), flush=True)
    print(f"\nreport: {out / 'report.md'}", flush=True)
    return out


async def shake_conversation(dev: SimDevice, rest: Rest, pump: EventPump, turns, clips, arms: list[str], gap_ms: int = 0, drafter: str = "careful") -> list[dict]:
    rows: list[dict] = []
    n = len(turns) * len(arms)
    for t in turns:
        for arm in arms:
            print(f"[{len(rows) + 1}/{n}] {t.id} ({arm}): human: {t.text[:60]!r}", flush=True)
            row = await shake_turn(dev, rest, pump, t, clips[t.id], arm, gap_ms, drafter)
            rows.append(row)
            print(f"      heard {row.get('heard')!r} gap {row.get('gap_ms')} ms partials {row.get('partials')}"
                  + (f" draft {'used' if row.get('used') else row.get('dropped') or row.get('draft_error') or '—'}" if arm == "drafts" else ""), flush=True)
            await asyncio.sleep(1.5)  # Molty idle again, well past the misfire window
    return rows


MAX_DRAFTS = 3  # per turn (relay MAX_DRAFTS_PER_TURN)
# what a careful drafter listens for (services/relay/src/core/partials.ts FLIP_WORDS): once heard, no (re)draft
FLIP_WORDS = {"but", "not", "no", "nope", "never", "don't", "dont", "doesn't", "didn't", "isn't", "won't", "can't", "cannot",
              "shouldn't", "wouldn't", "except", "unless", "actually", "instead", "wait", "cancel", "although", "however", "rather"}


def heard_flip(text: str) -> bool:
    return any(re.sub(r"[^\w']+", "", w.lower().replace("’", "'")) in FLIP_WORDS for w in text.split())


async def shake_turn(dev: SimDevice, rest: Rest, pump: EventPump, t, clip: Clip, arm: str, gap_ms: int = 0, drafter: str = "careful") -> dict:
    """The human shakes Molty and speaks; the scripted agent drafts / reacts (arm drafts) and answers."""
    dev.mic_for = lambda job, c=clip: Mic(c)
    pump.drain()
    before = set(dev.says)
    moods0 = len(dev.moods)
    row: dict = {"id": t.id, "arm": arm, "text": t.text, "tags": t.tags, "answer": t.answer}
    local = await dev.shake()
    rec = dev.listens.get(local) if local else None
    if rec is None:
        row["error"] = "device_busy"
        return row
    turn_id = None
    partials: list[dict] = []
    drafted = react = used = dropped = utt = None
    end = time.monotonic() + 45
    while utt is None:
        got = await pump.next(max(0.05, end - time.monotonic()))
        if got is None:
            break
        at, e = got
        typ, d = e.get("type"), e.get("data") or {}
        if typ == "shake" and d.get("turn_id"):
            turn_id = d["turn_id"]
        elif typ == "utterance.partial" and d.get("turn_id") == turn_id:
            partials.append({"at": at, **d})
            if arm != "drafts":
                continue
            so_far = d.get("text_so_far", "").lower()
            # the skill's advice: base the opener on the LATEST partial (a new draft replaces the old; <= 3 a turn)
            careful_stop = drafter == "careful" and heard_flip(so_far)
            if t.draft and not careful_stop and t.draft["when"].lower() in so_far and (drafted is None or (drafted["ok"] and drafted["n"] < MAX_DRAFTS)):
                st, b = await rest.draft(turn_id, d["seq"], t.draft["opener"])
                n = (drafted or {}).get("n", 0) + 1
                if b.get("ok") or drafted is None:
                    drafted = {"at": time.monotonic(), "seq": d["seq"], "n": n, "ok": bool(b.get("ok")), "error": None if b.get("ok") else b.get("error")}
            if t.react and react is None and t.react["when"].lower() in so_far:
                sent = time.monotonic()
                st, b = await rest.react(turn_id, **{k: t.react[k] for k in ("mood", "action") if k in t.react})
                react = {"sent": sent, "error": None if b.get("ok") else b.get("error"), "words": len(d.get("text_so_far", "").split())}
        elif typ == "draft.used" and d.get("turn_id") == turn_id:
            used = d
        elif typ == "draft.dropped" and d.get("turn_id") == turn_id and d.get("reason") != "replaced":
            dropped = d
        elif typ == "utterance" and d.get("turn_id") == turn_id:
            utt = d
    await asyncio.wait_for(rec.done.wait(), 15)
    row.update({"turn_id": turn_id, "heard": (utt or {}).get("text"), "claimed": bool((utt or {}).get("claimed")),
                "partial_texts": [p.get("text_so_far") for p in partials], "first_partial_ms": partials[0].get("elapsed_ms") if partials else None,
                "draft_seq": (drafted or {}).get("seq"), "opener": t.draft["opener"] if drafted else None,
                "drop_new_words": (dropped or {}).get("new_words")})
    if utt is None:
        row["error"] = "no_utterance"
    else:
        if gap_ms:
            await asyncio.sleep(gap_ms / 1000)
        text = (t.follow or t.reply) if used else t.reply
        try:
            await rest.say(text, in_reply_to=turn_id)
        except RelayError as e:
            row["error"] = e.body.get("error")
    says = [dev.says[k] for k in dev.says if k not in before]
    for sy in says:
        try:
            await asyncio.wait_for(sy.done.wait(), 20)
        except asyncio.TimeoutError:
            pass
    opener = dev.says.get(used.get("job")) if used else None
    follow = next((sy for sy in says if sy is not opener), None) if opener else None
    starts = [sy.play_start for sy in says if sy.play_start is not None]
    shown = None
    if react and not react["error"]:
        want = t.react.get("mood") or f"act:{t.react.get('action')}"
        shown = next((at for at, mood, r in dev.moods[moods0:] if r and mood == want), None)
    word_at = None
    if react and clip.words:
        k = min(react["words"], len(clip.words)) - 1
        word_at = rec.mic_open_at + clip.words[k][1] if k >= 0 else None
    facts = ShakeFacts(
        arm=arm,
        speech_end_at=rec.mic_open_at + clip.speech_end_s if clip.speech_end_s is not None else None,
        first_voice_at=min(starts) if starts else None,
        partials=len(partials),
        drafted=bool(drafted and drafted["ok"]),
        draft_error=(drafted or {}).get("error"),
        used=used is not None,
        dropped=(dropped or {}).get("reason"),
        family=t.draft["family"] if drafted else None,
        answer=t.answer,
        opener_end_at=opener.cursor if opener else None,
        follow_start_at=follow.play_start if follow else None,
        react_sent_at=react["sent"] if react else None,
        react_shown_at=shown,
        react_word_at=word_at,
        react_error=(react or {}).get("error"),
        ack=rec.ack,
    )
    row.update(shake_metrics(facts))
    # where the gap goes: eos → listen end (end of turn) → the first say's cmd → its first frame → its first sample played
    first = min((sy for sy in says if sy.play_start is not None), key=lambda sy: sy.play_start, default=None)
    if first is not None and facts.speech_end_at is not None and rec.end_at is not None:
        ms = lambda a, b: round((b - a) * 1000) if a is not None and b is not None else None  # noqa: E731
        row["gap_parts_ms"] = {"eos_to_turn_end": ms(facts.speech_end_at, rec.end_at), "turn_end_to_say_cmd": ms(rec.end_at, first.cmd_at),
                               "say_cmd_to_first_frame": ms(first.cmd_at, first.first_at), "first_frame_to_play": ms(first.first_at, first.play_start)}
    return row


async def conversation(dev: SimDevice, rest: Rest, agent, turns, clips, closing) -> list[dict]:
    rows: list[dict] = []
    history: list[dict] = []
    t_ret_prev = None
    prev_heard = ""
    for i, t in enumerate(turns):
        clip = clips[t.id]
        dev.mic_for = lambda job, c=clip: Mic(c)
        dev.imu_for = lambda job, c=clip: c.imu
        dev.stall_for = lambda job, w=list(t.stalls): w
        t_think = time.monotonic()
        say_text = t.say if i == 0 else await agent.reply(t.say, prev_heard, history)
        before_s, before_l = set(dev.says), set(dev.listens)
        t_req = time.monotonic()
        row: dict = {"i": i, "id": t.id, "kind": t.kind, "tags": t.tags, "voice": t.voice if t.kind == "tts" else None,
                     "expect": t.expect, "text": t.text, "said": say_text, "listen_ms": t.listen_ms,
                     "think_ms": round((t_req - t_think) * 1000) if i else None}
        print(f"[{i + 1}/{len(turns)}] {t.id}: {'Molty: ' + repr(say_text[:50]) + ' / ' if say_text else ''}human: {t.text[:60]!r}", flush=True)
        try:
            if t.shake:
                res = await shake_turn(dev, rest, t, say_text)
            else:
                res = await rest.talk(say_text, listen_ms=t.listen_ms)
        except RelayError as e:
            res = {"error": e.body.get("error"), "message": e.body.get("message")}
        t_ret = time.monotonic()
        say = agent_say(dev, before_s, say_text)
        lis = next((dev.listens[k] for k in dev.listens if k not in before_l), None)
        if lis is not None:
            row["uplink"] = {"frames": len(lis.up), "resent": sum(1 for h in lis.up if h.get("rt")),
                             "tr": next((h.get("tr") for h in lis.up if h.get("end")), None), "trim_ms": lis.trim_ms,
                             "hints": list(lis.hints)}
        if lis is not None:
            await asyncio.wait_for(lis.done.wait(), 10)
        if rows and say is not None:
            finish_gap(rows[-1], say, t_req, t_ret_prev, dev)
        row.update(say_metrics(say, t_req, (res.get("timings") or {}).get("say") or {}))
        row.update(listen_row(t, clip, lis, res, t_ret))
        row["_say"], row["_listen"] = say, lis
        rows.append(row)
        history.append({"said": say_text, "heard": res.get("heard", "")})
        prev_heard = res.get("heard", "")
        t_ret_prev = t_ret
    if closing is not None or isinstance(agent, LlmAgent):
        text = await agent.reply(closing, prev_heard, history)
        if text:
            before_s = set(dev.says)
            t_req = time.monotonic()
            try:
                res = await rest.say(text)
            except RelayError as e:
                res = {"error": e.body.get("error")}
            say = agent_say(dev, before_s, text)
            if rows and say is not None:
                finish_gap(rows[-1], say, t_req, t_ret_prev, dev)
    for r in rows:  # turns whose next say never came: their gap stays unmeasured
        if "gap_ms" not in r:
            r.update(turn_metrics(_facts(r, None)))
    return rows


async def shake_turn(dev: SimDevice, rest: Rest, t, say_text: str | None) -> dict:
    """Molty says its line (no listen), then the human shakes it and speaks: the device's own
    listen, adopted by the relay; its job (the turn) read back like a talk's listen."""
    if say_text:
        await rest.talk(say_text, listen=False)
    local = await dev.shake(peak_g=t.shake["peak_g"])
    if local is None:
        return {"error": "shake_not_listening"}
    lis = dev.listens[local]
    await asyncio.wait_for(lis.done.wait(), 40)
    if lis.adopt is None:
        return {"error": "not_adopted", "message": lis.reason or ""}
    r = await rest.job(lis.job)
    return {"heard": r.get("text", ""), "reason": r.get("reason"), "timings": {"listen": r.get("timings") or {}}}


def _facts(row: dict, next_play_at: float | None, first_voice_at: float | None = None) -> TurnFacts:
    f = row["_facts"]
    return TurnFacts(**{**f, "next_play_at": next_play_at, "first_voice_at": first_voice_at})


def agent_say(dev, before: set, text: str | None):
    """The agent's own say among the new ones (a relay filler may have started meanwhile)."""
    new = [dev.says[k] for k in dev.says if k not in before]
    return next((s for s in new if s.text == text), new[-1] if new else None)


def first_voice(says, after: float, until: float | None):
    """Molty's first say that reached the speaker in [after, until]: a filler, a quick, or the answer."""
    heard = [s for s in says if s.play_start is not None and s.play_start >= after and (until is None or s.play_start <= until)]
    return min(heard, key=lambda s: s.play_start, default=None)


def finish_gap(row: dict, say, t_req: float, t_ret_prev: float | None, dev=None) -> None:
    """The previous listen's gap: its end of speech -> this say's first sample at the speaker; and
    its perceived silence: -> Molty's first voice of any kind (the relay's filler counts)."""
    lis = row.get("_listen")
    first = None
    if dev is not None and lis is not None and row.get("speech_end_s") is not None and lis.mic_open_at is not None:
        first = first_voice(dev.says.values(), lis.mic_open_at + row["speech_end_s"], say.play_start)
    if first is not None and first is not say:
        row["filler"] = first.text
    row.update(turn_metrics(_facts(row, say.play_start, first.play_start if first is not None else None)))
    if lis is None or row.get("speech_end_s") is None or say.play_start is None:
        return
    eos = lis.mic_open_at + row["speech_end_s"]
    stop = lis.stop_at if lis.stop_at is not None else lis.end_at
    row["gap_parts_ms"] = {
        "eos_to_turn_end": round((stop - eos) * 1000),
        "turn_end_to_agent": round((t_ret_prev - stop) * 1000) if t_ret_prev else None,
        "agent_think": round((t_req - t_ret_prev) * 1000) if t_ret_prev else None,
        "request_to_voice": round((say.play_start - t_req) * 1000),
    }


def say_metrics(say, t_req: float, relay_say: dict) -> dict:
    if say is None:
        return {}
    late = 0
    for word, sample, arrival in say.cues:
        pt = say.play_time(sample)
        if pt is not None and arrival > pt - CUE_LEAD_S:
            late += 1
    return {
        "say_job": say.job,
        "tts_first_audio_ms": round((say.first_at - t_req) * 1000) if say.first_at else None,
        "say_cmd_ms": round((say.cmd_at - t_req) * 1000),
        "relay_first_audio_ms": relay_say.get("first_audio"),
        "relay_tts_first_byte_ms": relay_say.get("tts_first_byte"),
        "chirp_delay_ms": round(say.chirp_delay_s * 1000),
        "say_underruns": say.underruns,
        "say_underrun_ms": round(say.underrun_s * 1000),
        "say_audio_ms": round(say.samples / 16),
        "say_cues": len(say.cues),
        "cues_late": late,
        "say_bad_frames": len(say.bad),
    }


def listen_row(t, clip: Clip, lis, res: dict, t_ret: float) -> dict:
    tim = (res.get("timings") or {}).get("listen") or {}
    r: dict = {
        "reason": res.get("reason"),
        "heard": res.get("heard"),
        "error": res.get("error"),
        "wer": round(wer(t.text, res.get("heard") or ""), 3) if t.text and not res.get("error") else None,
        "foreign_script": foreign_script(res.get("heard") or ""),
        "speech_end_s": clip.speech_end_s,
        "stt_final_ms": tim.get("stt"),
        "relay_turn_end_ms": tim.get("turn_end"),
        "relay_voice": tim.get("voice"),
        "device_reason": lis.reason if lis else None,
        "stop_mic_s": round(lis.stop_mic_s, 3) if lis and lis.stop_mic_s is not None else None,
        "end_mic_s": round(lis.end_mic_s, 3) if lis and lis.end_mic_s is not None else None,
        "window_ms": lis.window_ms if lis else None,
        "vad": lis.vad if lis else None,
    }
    end = lis.end_mic_s if lis and lis.end_mic_s is not None else None
    r["_facts"] = {
        "expect": t.expect, "reason": res.get("reason") or (lis.reason if lis else None), "speech_end_s": clip.speech_end_s,
        "speech_after_end_s": clip.speech_after(end) if end is not None else 0.0,
        "stop_mic_s": lis.stop_mic_s if lis else None, "end_mic_s": end,
        "mic_open_at": lis.mic_open_at if lis else None, "end_at": lis.end_at if lis else None,
        "ack": lis.ack if lis else "off",
    }
    return r


async def alignment(o: Options, dev: SimDevice, rows: list[dict], turns, clips) -> tuple[list[int], dict | None]:
    if o.align == "none":
        return [], None
    items: dict[str, tuple[np.ndarray, list[str]]] = {}
    for r in rows:
        s = r.get("_say")
        if s is not None and s.cues and s.samples:
            items[f"say:{s.job}"] = (s.audio(), s.text.split())
    for t in turns:
        c = clips[t.id]
        if c.words:
            items[f"user:{t.id}"] = ((np.clip(c.pcm, -1, 1) * 32767).astype(np.int16), [w for w, _ in c.words])
    print(f"aligning {len(items)} clips on {o.box} (MMS_FA, CPU)…", flush=True)
    try:
        res = await asyncio.to_thread(align.mms_align, items, o.box, o.align_python)
    except Exception as e:  # noqa: BLE001 - alignment is a measurement, never the run's failure
        print(f"alignment failed: {e}", flush=True)
        return [], {"error": str(e)[:200]}
    errs: list[int] = []
    for r in rows:
        s = r.get("_say")
        if s is None or f"say:{s.job}" not in res:
            continue
        e = align.cue_errors(s.text, [(w, smp) for w, smp, _ in s.cues], res[f"say:{s.job}"])
        r["cue_err_ms"] = round(float(np.median([x["err_ms"] for x in e]))) if e else None
        r["cue_abs_p90_ms"] = round(float(np.percentile([abs(x["err_ms"]) for x in e], 90))) if e else None
        r["cue_words"] = e
        errs += [x["err_ms"] for x in e]
    diffs = []
    for t in turns:
        al = res.get(f"user:{t.id}")
        if not al:
            continue
        diffs += [(onset - a[0]) * 1000 for (_, onset), a in zip(clips[t.id].words, al) if a]
    selfcheck = {"n": len(diffs), "median_tts_minus_mms_ms": round(float(np.median(diffs)))} if diffs else None
    return errs, selfcheck
