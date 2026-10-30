---
name: molty
description: Connect this session to your human's Molty (the round desk device that talks) and use it as a side channel — speak to them, ask quick tap-button questions, show moods, and hear them when they shake Molty and speak. Use when the human says "connect to molty", "talk to me through molty", "ping me on molty if you need anything", or asks to test/talk with Molty. Session-only: while this session lives you can reach them; when it ends Molty can't wake you.
---

# Molty (session attach)

Molty is a thin surface: screen + speaker + mic + touch, reached through a relay. This skill
attaches **this session** to it. Long-lived agents (Grok Bot, OpenClaw) wake themselves via
webhooks; a session agent like you does not — you can only use Molty while you are running.

Full API, wire details and etiquette: the relay's live skill at `$MOLTY_RELAY/skill.md`
(relay URL in `~/.config/molty-claude/env`). Read it once per session if you need more than
the helpers below.

## Setup (once per machine)

- `~/.config/molty-claude/env` — `MOLTY_RELAY=…`, `MOLTY_DEVICE=molty-…`
- `~/.config/molty-claude/token` — agent token (0600). **Never print, echo or paste it.**
- Not paired yet → ask the human for the 8-char link code (console's agent page), then
  `~/.claude/skills/molty/scripts/pair.sh <LINK_CODE> "Claude Code"`; the human taps Molty.

## Connect

1. Start the event watcher with **Monitor** (persistent, one notification per event):
   `python3 ~/.claude/skills/molty/scripts/watch.py` — description "Molty events".
   Lines: `MOLTY utterance: '…'` (the human shook Molty and spoke — Molty already listened),
   `MOLTY shake/tap/button: …`. Ignore a `shake` with `"listening": true` (its `utterance` follows).
2. Say hello once, short: `scripts/talk.sh "Claude here, connected." --no-listen`.

## Helpers (`~/.claude/skills/molty/scripts/`)

| Script | Does | Output |
|---|---|---|
| `talk.sh "text"` | speak, then listen until the human finishes (end of turn is automatic) | `HEARD: '…' \| reason …` |
| `talk.sh "text" --no-listen` | speak only | — |
| `ask.sh "question" yes no later` | speak + 1–5 tap buttons (labels ≤ 8 chars), waits up to 120 s | `ANSWER: yes \| index 0` or `None` (timeout) |
| `mood.sh happy [hold_s]` | idle listening thinking speaking happy sleepy surprised | — |

All helpers follow long turns (202 pending) to the end; one call = one full turn.

## How to behave

- **Answer an `utterance` with one `talk.sh`** — it speaks your reply and hears their next turn.
  Keep going turn by turn; when they're done (or `reason no_speech`), end with `--no-listen`.
- **Blocked or need a decision → `ask.sh`** with short options instead of waiting in the
  terminal. Finished something they're waiting for → one short `talk.sh … --no-listen`.
- **Short.** 1–2 sentences; the first sentence starts the audio, so make it short.
  Molty chirps + shows `thinking` by itself when they stop talking — don't fill with "ok!".
- **Emote with intent**, not for turn-taking: `happy`/`surprised` on real moments.
- **Don't talk unprompted or repeatedly**; never use the mic unless they expect it.
- `reason no_speech` after a wake → one short line ("I'm here.") or nothing, never a burst.
- Errors: `audio_busy` (something else is speaking — wait/retry once), `turn_claimed`
  (another agent took this turn — stay quiet), `device_offline` (Molty's Wi-Fi; tell them in
  the terminal), `voice_box_offline` (their GPU voice box is down; cloud fallback is automatic
  in auto mode).

## Disconnect

When the session's work is done or they say stop: `talk.sh "Signing off." --no-listen`,
then stop the Monitor (TaskStop). Leave the token in place for next time.
