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


# ---------------------------------------------------------------- shake scenarios (partials, drafts, reacts)

SHAKE_COLS = [
    ("id", "turn"),
    ("arm", "arm"),
    ("partials", "parts"),
    ("first_partial_ms", "1st part"),
    ("gap_ms", "gap"),
    ("used", "draft"),
    ("dropped", "dropped"),
    ("wrong_opener", "wrong"),
    ("seam_ms", "seam"),
    ("react_lag_ms", "react lag"),
    ("ack", "ack"),
]


def _shake_cell(k: str, v) -> str:
    if k == "used":
        return "used" if v is True else ""
    if k == "wrong_opener":
        return "WRONG" if v is True else ""
    return _cell(v)


def _st(st: dict | None) -> str:
    return "–" if not st else f"median {st['median']} (p90 {st['p90']}, n={st['n']}, [{st['min']} .. {st['max']}])"


def shake_summary_lines(s: dict) -> list[str]:
    c = s.get("calibration") or {}
    d = c.get("drafts") or {}
    return [
        f"perceived gap, baseline (answer the utterance):    {_st(s['gap_baseline_ms'])} ms",
        f"perceived gap, with drafts (every turn):          {_st(s['gap_drafts_ms'])} ms",
        f"perceived gap, drafts that played:                {_st(s['gap_drafts_used_ms'])} ms",
        f"gap saved per turn (baseline - drafts, paired):   {_st(s['gap_saved_ms'])} ms",
        f"drafts: {s['drafted']} made, {s['used']} played (use rate {s['use_rate']}), dropped {s['dropped']}, errors {s['draft_errors']}",
        f"wrong openers played: {s['wrong_openers']} (rate {s['wrong_opener_rate']}); relay dropped {s['saved_by_relay']} that would have been wrong, {s['lost_by_relay']} that would have fit",
        f"opener → follow-up seam:                          {_st(s['seam_ms'])} ms",
        f"reacts: {s['reacts']} (errors {s['react_errors']}); request → device {_st(s['react_rtt_ms'])} ms; word spoken → shown {_st(s['react_lag_ms'])} ms",
        f"turns with partials (drafts arm): {s['turns_with_partials']}",
        f"relay calibration after the run: turns {c.get('turns')}, policy {c.get('policy')}, threshold {d.get('threshold')}",
    ]


def shake_table(rows: list[dict], summary: dict, md: bool = False) -> str:
    head = [h for _, h in SHAKE_COLS]
    body = [[_shake_cell(k, r.get(k)) for k, _ in SHAKE_COLS] for r in rows]
    if md:
        lines = ["| " + " | ".join(head) + " |", "|" + "---|" * len(head)] + ["| " + " | ".join(b) + " |" for b in body]
    else:
        w = [max(len(h), *(len(b[i]) for b in body)) for i, h in enumerate(head)]
        fmt = lambda xs: "  ".join(x.ljust(w[i]) for i, x in enumerate(xs))  # noqa: E731
        lines = [fmt(head), fmt(["-" * n for n in w])] + [fmt(b) for b in body]
    lines.append("")
    lines += [f"- {x}" if md else x for x in shake_summary_lines(summary)]
    return "\n".join(lines)


def write_shake(out: Path, meta: dict, rows: list[dict], summary: dict) -> None:
    (out / "report.json").write_text(json.dumps({"meta": meta, "summary": summary, "turns": rows}, indent=2, default=str))
    md = [f"# Sim run {meta['run']} — {meta['scenario']} (shake turns: partials, drafts, reacts)", ""]
    md += [f"- {k}: `{v}`" for k, v in meta.items() if k not in ("run", "scenario")]
    md += ["", "## Turns", "", shake_table(rows, summary, md=True), "", "## Per turn", ""]
    for r in rows:
        md.append(f"### {r['id']} ({r['arm']})")
        md.append(f"- human: {r.get('text')!r} — heard {r.get('heard')!r}")
        md.append(f"- partials: {r.get('partial_texts')}")
        if r["arm"] == "drafts":
            md.append(f"- opener {r.get('opener')!r} on partial {r.get('draft_seq')}: {'played' if r.get('used') else r.get('dropped') or r.get('draft_error') or 'not drafted'}"
                      f" (fits: {r.get('answer')}; wrong: {r.get('wrong_opener')}; late words {r.get('drop_new_words')})")
        if r.get("error"):
            md.append(f"- error: `{r['error']}`")
        md.append(f"- gap parts: {r.get('gap_parts_ms')}")
        md.append(f"- gap {r.get('gap_ms')} ms, seam {r.get('seam_ms')} ms, react lag {r.get('react_lag_ms')} ms (rtt {r.get('react_rtt_ms')} ms), ack {r.get('ack')}")
        md.append("")
    (out / "report.md").write_text("\n".join(md))
