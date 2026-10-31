---
name: molty
description: Connect to your human's Molty (the round desk device that talks) and use it — pair with the link code and a tap, speak and listen in one call per turn, ask tap-button questions, show moods, and answer when they shake Molty and speak. Use when the human says "connect to molty", "connect yourself to my Molty", "talk to me through molty", "ping me on molty if you need anything", pastes a Molty setup message (Device + Code), or asks to test, update or debug Molty.
---

# Molty

Molty is a small desk creature next to your human: round touch screen, speaker, mic, motion
sensor. It is a thin client: you drive it over HTTPS (or MCP) via a relay, and you are its
brain while your human keeps you paired. The relay does the speech ([voice.md](references/voice.md)).

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

The default relay is `https://molty-relay.alicenet.workers.dev`; `MOLTY_RELAY` overrides it.

## Helpers (`<skill-dir>/scripts/`)

| Script | Does | Prints |
|---|---|---|
| `pair.sh <DEVICE> <CODE> [name]` | pair, store env + token | Molty's code, then `paired` |
| `talk.sh "text"` | speak, then listen until the human finishes | `HEARD: '…' \| reason …` |
| `talk.sh "text" --no-listen` | speak only | — |
| `talk.sh "text" --reply <turn_id>` | answer an `utterance` (claims the turn) | `HEARD: …` |
| `quick.sh <id> [--reply <turn_id>]` | play a cached quick phrase now | the job |
| `ask.sh "question" yes no later` | speak + 1–5 tap buttons (labels ≤ 8 chars) | `ANSWER: yes \| index 0` |
| `mood.sh happy [hold_s]` | set a mood | the result |
| `act.sh nod` | a one-shot gesture (nod, wiggle, bounce, …) | the result |
| `watch.py` / `watch.py --once [wait_s]` | one line per event, forever / unread events, then exit | `MOLTY utterance: '…'` |

Each helper follows a long turn (`202 pending`) to its end.

## Key rules

- **Speak first, always.** Your first call on every wake comes before any reasoning or other
  tool. No planning, lookups or status checks.
  - Can answer from what you know → one `talk`, the answer. Nothing else in the turn.
  - Needs any work (search, tools, a long think) → say what you'll do and start working:
    `talk {"say":"Searching for that. [working]"}`. Molty animates by itself until you speak
    again. Never send gestures to fill the wait. Over ~20 s of work: another one-liner saying
    where you are, `[working]` again. Then answer.
- **Fastest model for that first line**, always; a slower one only for the work behind it.
- **Follow a `202 pending` talk to its end.** Never finish a run with a talk unread: their
  words are in it.
- **Wake only on `utterance`** unless your human gave an input a job: other runs collide.
- **A shake means "I want to talk".** You get one `utterance` with their words: answer with talk
  and `in_reply_to: <turn_id>`. `turn_claimed`: stay quiet.
- **Never speak while they talk**: `utterance.partial` is context. (`human_speaking`). Only `draft`
  or `react`.
- **Slow wake (cold start, or ack not ready)?** `quick {id}` is cached, instant audio. Skipped
  by itself if Molty already filled; talk follows seamlessly. Vary them; register your own
  (`molty_phrases`).
- **Short: ≤ 2 sentences, ~25 words a turn.** Split longer answers; speech time is wait time.
- **Emote inline, sparingly:** `"[happy] Build passed!"`, `"Done. [nod]"` — never spoken or
  shown; one per sentence at most ([etiquette.md](references/etiquette.md#emotes)).
- **Learn them; offer what they can change** ([personalize.md](references/personalize.md)): keep
  notes on how they like Molty, and tell them about gestures and phrases when it would help.
- **A decision a tap can settle → ask**, not a spoken yes/no. Buttons (accept/reject) mean
  acknowledge / dismiss, never an answer.
- **Read `relay.caps`, not versions** (`status` command, or `device.caps` on an `utterance`).
- **Don't talk unprompted or repeatedly.** `no_speech` after a wake → one short line or nothing.
- **Errors:** `audio_busy` (wait for the current audio, retry once), `turn_claimed` (stay
  quiet), `device_offline` (tell them another way),
  `voice_box_offline`, `listen_failed` (try once more). A failed turn's `job` id is in the
  [activity log](references/activity.md).

## References

Read the one you need, when you need it.

- [api.md](references/api.md) — pairing in full, REST commands, talk, ask, errors, MCP, revoke.
- [events.md](references/events.md) — event types, the `utterance`, inbox, long-poll, webhooks and templates, the CLI stream.
- [talking-fast.md](references/talking-fast.md) — drafts, end of turn, the ack chirp, `no_speech`, latency rules.
- [personalize.md](references/personalize.md) — learning your human; gestures, phrases, wake: what they can customise.
- [etiquette.md](references/etiquette.md) — when to speak, emotes, keeping your own behaviour ledger (relay notes if you have no memory).
- [voice.md](references/voice.md) — the ways Molty gets a voice, and what each means for you.
- [voice-box.md](references/voice-box.md) — the owner's own GPU voice box: what agents see.
- [updates.md](references/updates.md) — firmware updates, `api_version`, updating this skill.
- [activity.md](references/activity.md) — `molty_activity`: debug your own turns.
- [self-hosting.md](references/self-hosting.md) — run your own private relay.
- Harnesses: [claude-code.md](references/harnesses/claude-code.md), [codex.md](references/harnesses/codex.md), [grok.md](references/harnesses/grok.md), [openclaw.md](references/harnesses/openclaw.md).
