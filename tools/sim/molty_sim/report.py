"""report.md + report.json in the run directory, and the summary table printed at the end."""

from __future__ import annotations

import json
from pathlib import Path

from .metrics import SUMMARY_KEYS

COLS = [
    ("id", "turn"),
    ("reason", "reason"),
    ("expect", "exp"),
    ("eos_to_turn_end_ms", "eos→end"),
    ("false_cutoff", "cut"),
    ("speech_lost_ms", "lost"),
    ("wer", "WER"),
    ("stt_final_ms", "stt"),
    ("gap_ms", "gap"),
    ("tts_first_audio_ms", "say 1st"),
    ("cue_err_ms", "cue"),
    ("say_underruns", "dry"),
    ("ack", "ack"),
]


def _cell(v) -> str:
    if v is None:
        return "–"
    if isinstance(v, bool):
        return "CUT" if v else ""
    if isinstance(v, float):
        return f"{v:.2f}"
    return str(v)


def table(rows: list[dict], summary: dict, md: bool = False) -> str:
    head = [h for _, h in COLS]
    body = [[_cell(r.get(k)) for k, _ in COLS] for r in rows]
    if md:
        lines = ["| " + " | ".join(head) + " |", "|" + "---|" * len(head)]
        lines += ["| " + " | ".join(b) + " |" for b in body]
    else:
        w = [max(len(h), *(len(b[i]) for b in body)) for i, h in enumerate(head)]
        fmt = lambda xs: "  ".join(x.ljust(w[i]) for i, x in enumerate(xs))  # noqa: E731
        lines = [fmt(head), fmt(["-" * n for n in w])] + [fmt(b) for b in body]
    lines.append("")
    lines += summary_lines(summary, md)
    return "\n".join(lines)


def summary_lines(s: dict, md: bool = False) -> list[str]:
    out = []
    if md:
        out += ["| metric | n | median | p90 | min | max |", "|---|---|---|---|---|---|"]
    for k, label in SUMMARY_KEYS:
        st = s.get(k)
        if not st:
            continue
        if md:
            out.append(f"| {label} | {st['n']} | {st['median']} | {st['p90']} | {st['min']} | {st['max']} |")
        else:
            out.append(f"{label:52s} n={st['n']:<3} median {st['median']:>7}  p90 {st['p90']:>7}  [{st['min']} .. {st['max']}]")
    out.append("")
    out.append(f"turns {s['turns']}: reason as expected {s['reason_ok']}, false cut-offs {s['false_cutoffs']}, "
               f"false no_speech {s['false_no_speech']}, missed no_speech {s['missed_no_speech']}, errors {s['errors']}, "
               f"say underruns {s['underruns']}, acks {s['acks']}")
    if s.get("alignment_selfcheck_ms"):
        out.append(f"alignment self-check (human clips, TTS onset - MMS onset): {s['alignment_selfcheck_ms']}")
    return out


def write(out: Path, meta: dict, rows: list[dict], summary: dict) -> None:
    (out / "report.json").write_text(json.dumps({"meta": meta, "summary": summary, "turns": rows}, indent=2, default=str))
    md = [f"# Sim run {meta['run']} — {meta['scenario']}", ""]
    md += [f"- {k}: `{v}`" for k, v in meta.items() if k not in ("run", "scenario")]
    md += ["", "## Turns", "", table(rows, summary, md=True), "", "## Per turn", ""]
    for r in rows:
        md.append(f"### {r['i'] + 1}. {r['id']} ({', '.join(r.get('tags') or []) or r['kind']})")
        md.append(f"- Molty said: {r.get('said')!r}")
        md.append(f"- human: {r.get('text')!r}")
        md.append(f"- heard: {r.get('heard')!r} — reason `{r.get('reason')}` (device `{r.get('device_reason')}`), expected `{r.get('expect')}`")
        if r.get("error"):
            md.append(f"- error: `{r['error']}` {r.get('message') or ''}")
        md.append(f"- speech end {r.get('speech_end_s')} s, listen_stop at {r.get('stop_mic_s')} s, end frame at {r.get('end_mic_s')} s (mic time); "
                  f"lost speech {r.get('speech_lost_ms')} ms")
        if r.get("gap_parts_ms"):
            md.append(f"- gap {r.get('gap_ms')} ms = {r['gap_parts_ms']}")
        md.append(f"- say: request→first audio {r.get('tts_first_audio_ms')} ms (relay first_audio {r.get('relay_first_audio_ms')}), "
                  f"chirp delay {r.get('chirp_delay_ms')} ms, underruns {r.get('say_underruns')} ({r.get('say_underrun_ms')} ms), "
                  f"cues {r.get('say_cues')} (late {r.get('cues_late')}), cue error median {r.get('cue_err_ms')} ms, |p90| {r.get('cue_abs_p90_ms')} ms")
        md.append("")
    (out / "report.md").write_text("\n".join(md))
