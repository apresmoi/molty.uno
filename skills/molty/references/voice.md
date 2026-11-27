# Giving Molty a voice

Molty's speaker and mic are only half of speech: the words are turned into audio (TTS) and
audio into words (STT) by the relay, by one of these.

| Way | What does the speech | Needs |
|---|---|---|
| **Cloud** | the relay's provider: Cloudflare Workers AI (Deepgram Aura TTS, Whisper STT), or any OpenAI-compatible API | nothing from the owner on the official relay; on a private relay, the Workers AI binding on its account (`VOICE_PROVIDER=workers-ai`, what `wrangler.jsonc` ships) or a key (`VOICE_PROVIDER=openai`, `OPENAI_API_KEY`, optional `OPENAI_BASE_URL`, `TTS_MODEL` / `STT_MODEL` / `TTS_VOICE`) |
| **Own voice box** | the owner's GPU running Kyutai STT/TTS + `molty-voice`, paired with a tap like an agent | a GPU machine ([services/molty-voice/README](https://github.com/apresmoi/molty.uno/tree/main/services/molty-voice)); streaming STT and semantic end of turn: the fastest turns. See [voice-box.md](voice-box.md) |
| **Own voice box, realtime engine** | a realtime voice model reached with the owner's own key, through `molty-voice --engine realtime` on any computer: Molty's fast conversational voice, the *voice front* (below) | the owner's provider key and an always-on computer; no GPU |
| **None** | nothing: `VOICE_PROVIDER=none` on the relay (also what a relay without an AI binding and without the variable falls back to) | no account, key or GPU |

The official relay runs the cloud provider. Which one a private relay runs is its operator's
choice ([self-hosting.md](self-hosting.md)); the owner picks per Molty whether their voice box
or the cloud speaks (`voice_mode` on the console's agent page: `auto`, `box`, `cloud`).

## The voice front: you are the inner voice

When the owner turns the voice front on, a realtime voice model is Molty's outer voice. It hears
the person as they speak and answers within a fraction of a second; you are the inner voice. You
can tell: the `utterance` carries `front: {said, waiting}`.

- **The front already answered.** `said` is what Molty told the person. `waiting: true` means it
  said a holding line and handed the turn to you: the person waits for your answer. `waiting:
  false` means it handled small talk itself: answer only if you have something to add.
- **Answer with facts, not with spoken filler.** Your `talk` / `say` text is said by the front's
  voice as you wrote it (the owner may set it to rephrase, keeping your facts, numbers and names), so
  write it to be heard: short, spoken. No "let me check", no greeting, no quick phrase first: the
  front already did that. Inline `[cues]` are never spoken.
- **Use `exact: true` when the wording matters** (a quote, a code, a name spelled out, a sentence you
  chose with care): it is always word for word. Inline `[cues]` apply only with `exact`.
- **Your answer never cuts the front off.** If it is still speaking, yours plays right after.
- **Background, said only if asked:** `molty_front {context: "…"}`. **End the conversation:**
  `molty_front {end: true}`. Molty listens again by itself after each answer (hands-free) until the
  person stops, says bye, or taps.
- **You stay the brain.** The front knows nothing about the person's work and is told never to
  invent a result. If you never answer a waiting turn, it says once that it is still waiting.

The model runs on the owner's own computer with the owner's own key; the relay never holds a key.

## What works without a voice

Speech needs a voice; everything else does not:

| Needs a voice | Works without one |
|---|---|
| `say`, `listen`, `talk`, the spoken part of `ask`, shake to talk (`utterance`) | `ask`'s tap buttons, `mood`, `show` (text in the fluid), `state`, events (shake, tap, button, answer), Molty's own ack chirp, `status`, updates, the activity log |

Without a voice, talk with text and taps: `show` a short line, `ask` with options and no `say`,
emote with `mood`. Speak through your own channel (the terminal, chat) for anything longer.

## How you tell which mode you are in

- **`relay.caps`** (from `status`): `say` / `listen` = this board has a speaker / mic. That is
  hardware, not the voice: check the next point too.
- **`relay.voice`** (from `status`): who would speak right now — `box` (the owner's voice box),
  the cloud provider's name (`workers-ai`, `openai`), `none` (no voice), `voice_off` (the owner chose
  text only), or `voice_box_offline`
  (the owner chose `box` and it is down).
- **Every say / listen timing** carries `voice` (`timings.voice`; for talk / ask
  `timings.say.voice` and `timings.listen.voice`), and the activity log records it per job.
- **Errors:** `voice_box_offline` (503) when the box is required and down. With `none`, nothing
  fails: `say` plays silence sized to the text while the words show on screen, and a listen
  hears the fixed marker `[stt unavailable]` — treat that as "no voice", not as words.

Check `relay.voice` once after pairing and when speech misbehaves; don't call `status` before
every turn.

## Text only

The owner can turn voice off in setup. `relay.voice` is then `voice_off`, and `relay.setup` says what
they chose (`{voice, first_words, reflex}`). `say`, `talk` and `ask` show your words on the screen,
and a talk returns `reason: "voice_off"` without listening. `listen` fails with `voice_off`, `quick`
answers `{skipped: "voice_off"}`, and a shake is declined, so no `utterance` arrives. Cues still work.
Reach the human with `ask` buttons, emotes, and your normal chat.
