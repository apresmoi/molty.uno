"""molty-sim run | check | measure | show — see tools/sim/README.md."""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import os
import sys
from pathlib import Path

from . import report, scenario
from .run import SIM_DIR, Options, run

DEFAULT_SCENARIO = SIM_DIR / "scenarios" / "default.yaml"


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="molty-sim", description="Simulated Molty: measure the conversation loop end to end.")
    sub = p.add_subparsers(dest="cmd", required=True)

    r = sub.add_parser("run", help="run a scenario end to end (default: the default suite against a local wrangler dev)")
    r.add_argument("scenario", nargs="?", default=str(DEFAULT_SCENARIO))
    r.add_argument("--only", help="comma-separated turn ids")
    r.add_argument("--box", default=os.environ.get("MOLTY_SIM_BOX", "voicebox"), help="ssh host of the voice box (tunnel + alignment; env MOLTY_SIM_BOX)")
    r.add_argument("--no-tunnel", action="store_true", help="the Kyutai ports are already reachable on --tts-port/--stt-port")
    r.add_argument("--tts-port", type=int, default=18089)
    r.add_argument("--stt-port", type=int, default=18090)
    r.add_argument("--relay", help="a DEPLOYED relay origin instead of a local wrangler dev (needs --allow-remote)")
    r.add_argument("--allow-remote", action="store_true", help="yes, use the deployed relay (with the sim's own molty-sim… device)")
    r.add_argument("--no-bridge", action="store_true", help="don't start a molty-voice for the sim device (one is already paired + running)")
    r.add_argument("--turn-end", type=float, help="bridge: end-of-turn threshold (molty-voice --turn-end)")
    r.add_argument("--turn-end-hold", type=float, help="bridge: mid-sentence hold seconds (molty-voice --turn-end-hold)")
    r.add_argument("--turn-end-skip", type=int, help="bridge: steps ignored at a listen's start (molty-voice --turn-end-skip)")
    r.add_argument("--agent", choices=["scripted", "llm"], default="scripted")
    r.add_argument("--agent-gap-ms", type=int, default=0, help="fixed agent think time added before each reply (default 0)")
    r.add_argument("--llm-url", default=os.environ.get("MOLTY_SIM_LLM_URL", "http://127.0.0.1:11434"), help="OpenAI-compatible base URL for --agent llm (env MOLTY_SIM_LLM_URL)")
    r.add_argument("--llm-model", default=os.environ.get("MOLTY_SIM_LLM_MODEL", "llama3.2:3b"))
    r.add_argument("--align", choices=["mms", "none"], default="mms", help="say-cue ground truth: forced alignment on the box, or skip")
    r.add_argument("--align-python", default=os.environ.get("MOLTY_SIM_ALIGN_PYTHON", "python3"), help="python with torchaudio on the box (env MOLTY_SIM_ALIGN_PYTHON)")
    r.add_argument("--no-audio", action="store_true", help="don't keep WAVs in the run directory")
    r.add_argument("--drafter", choices=["careful", "naive"], default="careful", help="shake scenarios: careful = no (re)draft once a turn-around word (but, not, actually, …) was heard; naive = always re-base on the latest partial")
    r.add_argument("-v", "--verbose", action="store_true")

    s = sub.add_parser("check", help="parse a scenario and print its turns (no network)")
    s.add_argument("scenario", nargs="?", default=str(DEFAULT_SCENARIO))

    sub.add_parser("measure", help="offline numbers for the turn-trace fixes: shake noise trim, Wi-Fi stall resend, mic gain (no box, no relay)")

    sh = sub.add_parser("show", help="print the summary table of a finished run")
    sh.add_argument("run_dir")

    a = p.parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if getattr(a, "verbose", False) else logging.WARNING, format="%(asctime)s %(name)s %(message)s")

    if a.cmd == "check":
        sc = scenario.load(a.scenario)
        for t in sc.turns:
            src = t.markup and t.markup.pauses or t.wav or (f"silence {t.silence_s}s" if t.kind == "silence" else "")
            print(f"{t.index + 1:2d}. {t.id:22s} {t.kind:7s} expect {t.expect:9s} listen_ms {t.listen_ms!s:5s} {t.text[:50]!r} {src}")
            if sc.shake and (t.draft or t.react):
                print(f"    draft {t.draft} react {t.react} fits {t.answer}")
        if sc.shake:
            print(f"shake turns, arms {sc.arms}")
        return 0
    if a.cmd == "measure":
        from .measure import measure

        print(json.dumps(measure(), indent=2))
        return 0
    if a.cmd == "show":
        d = json.loads((Path(a.run_dir) / "report.json").read_text())
        print(report.shake_table(d["turns"], d["summary"]) if d["turns"] and "arm" in d["turns"][0] else report.table(d["turns"], d["summary"]))
        return 0

    tuning = []
    for flag, v in (("--turn-end", a.turn_end), ("--turn-end-hold", a.turn_end_hold), ("--turn-end-skip", a.turn_end_skip)):
        if v is not None:
            tuning += [flag, str(v)]
    o = Options(scenario=scenario.load(a.scenario), box=a.box, tunnel=not a.no_tunnel, tts_port=a.tts_port, stt_port=a.stt_port,
                relay=a.relay, allow_remote=a.allow_remote, bridge=not a.no_bridge, tuning=tuning, agent=a.agent,
                agent_gap_ms=a.agent_gap_ms, llm_url=a.llm_url, llm_model=a.llm_model, align=a.align, align_python=a.align_python,
                keep_audio=not a.no_audio, only=a.only.split(",") if a.only else None, drafter=a.drafter)
    try:
        asyncio.run(run(o))
    except KeyboardInterrupt:
        return 130
    except SystemExit:
        raise
    except Exception as e:  # noqa: BLE001
        print(f"sim run failed: {type(e).__name__}: {e}", file=sys.stderr)
        return 1
    return 0
