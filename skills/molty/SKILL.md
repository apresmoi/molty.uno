---
name: molty
description: Connect to your human's Molty (the round desk device that talks) and use it — pair with the link code and a tap, speak and listen in one call per turn, ask tap-button questions, show moods, and answer when they shake Molty and speak. Use when the human says "connect to molty", "connect yourself to my Molty", "talk to me through molty", "ping me on molty if you need anything", pastes a Molty setup message (Device + Code), or asks to test, update or debug Molty.
---

# Molty

Molty is a small physical desk creature next to your human: a round touch screen, a
speaker, a microphone and a motion sensor. It is a thin client, not an agent. It holds a
link to a relay, and you drive it over plain HTTPS (or MCP): show text in its fluid
"creature" display, set its mood, speak through its speaker and listen through its mic (the
relay does the speech: [voice.md](references/voice.md)), ask questions answered with a tap,
and hear what the human does to it. You are its brain for as
long as your human keeps you paired.

Resolve `<skill-dir>` to this file's directory.

## Connect

The human's setup message gives you a **Device** id (`molty-xxxxxxxxxx`) and an
8-character **Code** (`[A-Z2-9]`, the link code; it rotates every 15 min and after each
pairing). Molty uses the official relay unless the message also names a **Relay** (a
private one, see [self-hosting](references/self-hosting.md)).

1. **Pair once.** With a shell (macOS/Linux, `zsh` or `bash`, `curl`, `python3`):

   ```sh
   "<skill-dir>/scripts/pair.sh" <DEVICE> <CODE> "<your name>"
   ```

   It prints Molty's 4-digit code: tell your human "Molty shows 1234 — tap it to accept me."
   It stores the device in `~/.config/molty/env` and the agent token in
   `~/.config/molty/token` (0600). The message names a Relay? Run it with
   `MOLTY_RELAY=<that relay>` in front; it is stored too. Without a shell, pair over REST or
   MCP: [api.md](references/api.md#pair).
2. **The agent token is a secret.** Never print, echo or paste it — not in chat, logs,
   commits or on the device. It is scoped to this one Molty; the human can revoke it.
3. **Say hello once**, short: `"<skill-dir>/scripts/talk.sh" "Hi, I'm here." --no-listen`,
   then tell your human it worked.
4. **Hear Molty.** Pick how you receive events from your harness:
   [Claude Code](references/harnesses/claude-code.md),
   [Codex](references/harnesses/codex.md), [Grok](references/harnesses/grok.md),
   [OpenClaw and other long-lived agents](references/harnesses/openclaw.md).

The default relay is `https://molty-relay.alicenet.workers.dev`; `MOLTY_RELAY` overrides
it. Examples in the references write `$MOLTY_RELAY` for whichever relay Molty uses.

## Helpers (`<skill-dir>/scripts/`)

| Script | Does | Prints |
|---|---|---|
| `pair.sh <DEVICE> <CODE> [name]` | pair, store env + token | Molty's code, then `paired` |
| `talk.sh "text"` | speak, then listen until the human finishes | `HEARD: '…' \| reason …` |
| `talk.sh "text" --no-listen` | speak only | — |
| `talk.sh "text" --reply <turn_id>` | answer an `utterance` (claims the turn) | `HEARD: …` |
| `ask.sh "question" yes no later` | speak + 1–5 tap buttons (labels ≤ 8 chars) | `ANSWER: yes \| index 0` |
| `mood.sh happy [hold_s]` | set a mood | the result |
| `act.sh nod` | a one-shot gesture (nod, wiggle, bounce, …) | the result |
| `watch.py` / `watch.py --once [wait_s]` | one line per event, forever / unread events, then exit | `MOLTY utterance: '…'` |

Each helper follows a long turn (`202 pending`) to its end.

## Key rules

- **One call per turn: talk, nothing else.** It speaks, listens and returns their words. No
  `show`/`mood`/`act` around it (moods are automatic; feelings go in the text as cues).
- **Fastest model for chat turns**; a slower one only for real work, after a short line.
- **Wake only on `utterance`.** A run started by a `tap`/`button`/`shake` mid-conversation
  collides with the conversation's run.
- **A shake means "I want to talk".** Molty listens by itself; you get one `utterance`
  event with their words. Answer it at once with talk and `in_reply_to: <turn_id>`; ignore
  the `shake` with `"listening": true`. `turn_claimed`: stay quiet.
- **Never speak while they talk**: `utterance.partial` is context (`human_speaking`). Only
  `draft` an opener (`draft.used`: continue it) or `react` (Molty also reacts by itself; yours wins).
- **Molty acknowledges the end of their turn itself** (chirp + `thinking`): answer straight
  away, no "hmm". Long work → one short spoken line first; never leave them in silence.
- **Short.** 1–2 sentences; a short first sentence starts audio sooner.
- **Emote inline, sparingly:** `"[happy] Build passed!"`, `"Done. [nod]"` — cues are never
  spoken or shown and fire on their word; one per sentence at most; don't fidget
  ([etiquette.md](references/etiquette.md#emotes)).
- **A decision a tap can settle → ask**, not a spoken yes/no. Buttons (accept/reject) mean
  acknowledge / dismiss, never an answer.
- **Read `relay.caps`, not versions**, to know what this Molty can do (`say`, `listen`,
  `ask`, `shake_talk`, `update`, …). `relay.voice` says who speaks. Get them from the `status`
  command (`POST …/cmd {"name":"status"}`, MCP `molty_status`); every `utterance` also carries
  them as `device.caps`. No `GET …/status` route.
- **Don't talk unprompted or repeatedly**; the mic only when expected. `no_speech` after
  a wake → one short line or nothing.
- **Errors:** `audio_busy` (wait for the current audio, retry once), `turn_claimed` (stay
  quiet), `device_offline` (tell them another way),
  `voice_box_offline` (their voice box is down), `listen_failed` (the recording broke:
  try once more). A failed turn carries its `job` id: look it up in the
  [activity log](references/activity.md).

## References

Read the one you need, when you need it.

- [api.md](references/api.md) — pairing in full, REST commands, talk, ask, errors, MCP, revoke.
- [events.md](references/events.md) — event types, the `utterance`, inbox, long-poll, webhooks and templates, the CLI stream.
- [talking-fast.md](references/talking-fast.md) — drafts, end of turn, the ack chirp, `no_speech`, latency rules.
- [etiquette.md](references/etiquette.md) — when to speak, emotes, keeping your own behaviour ledger (relay notes if you have no memory).
- [voice.md](references/voice.md) — the ways Molty gets a voice, and what each means for you.
- [voice-box.md](references/voice-box.md) — the owner's own GPU voice box: what agents see.
- [updates.md](references/updates.md) — firmware updates, `api_version`, updating this skill.
- [activity.md](references/activity.md) — `molty_activity`: debug your own turns.
- [self-hosting.md](references/self-hosting.md) — run your own private relay.
- Harnesses: [claude-code.md](references/harnesses/claude-code.md), [codex.md](references/harnesses/codex.md), [grok.md](references/harnesses/grok.md), [openclaw.md](references/harnesses/openclaw.md).
