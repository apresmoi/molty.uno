# The owner's voice box: what agents see

Some owners run Molty's speech on their own GPU: Kyutai STT/TTS behind `molty-voice`, a
"voice box" that pairs with Molty like an agent (same tap) and dials out to the relay. You never
talk to it: you call `talk` / `say` / `listen` / `ask` exactly as always, and the relay routes the
speech through the box. Setup is the owner's job
([services/molty-voice/README](https://github.com/apresmoi/molty.uno/tree/main/services/molty-voice)); the ways Molty can
speak at all are in [voice.md](voice.md).

## What changes for you

- **Faster turns.** STT streams while the human talks, so the transcript arrives about
  0.1–0.5 s after they stop instead of after a batch upload.
- **Semantic end of turn.** The box decides when the human finished from what they said:
  `reason` is `turn_end` (treat it like `silence`), or `no_speech` when nobody spoke. A thinking
  pause mid-sentence does not cut them off. Don't pass `vad` / `end_silence_ms`
  ([talking-fast.md](talking-fast.md#end-of-turn)).
- **Voices.** Your `voice` argument passes through to the box. Without one, the owner's default
  box voice is used; with neither, the box picks its own.
- **Who spoke.** `status` → `relay.voice` is `box` while the box serves; timings carry
  `voice: "box"`; the `utterance` event carries `device.voice`.

## The owner's mode (`voice_mode`, set on the console)

| Mode | You see |
|---|---|
| `auto` (default) | the box when it is connected, else the cloud. If the box drops mid-listen the relay finishes with the cloud on the audio it already has: your turn still succeeds, `timings.voice_fallback` (and the activity log's `voice.fallback`) says why (`voice_box_offline`, `stt_timeout`, `voice_box_error`) |
| `box` | the box only: without it, speech calls fail at once with `voice_box_offline` (REST 503) |
| `cloud` | the cloud only, box or not |

## When it fails

- `voice_box_offline`: tell your human their voice box is down (it is their machine); carry on
  with `ask` taps, `show` and `mood` ([voice.md](voice.md#what-works-without-a-voice)), and
  don't retry speech in a loop.
- `reason: error` on a listen: the box failed mid-listen; `heard` is what was transcribed, if
  anything. Try the turn once more.
- Slow or garbled turns: read the job in the [activity log](activity.md) (`data.voice`,
  `data.timings`) before blaming the network.

## Live conversation (experiment)

A box started with `--live` and an owner who set `conversation: "live"` answer a shake or double tap on OpenAI's `gpt-live-1` directly. You are not in that conversation: its `shake` still says `listening: true`, no `utterance` follows, and your `say` or `talk` meanwhile gets `audio_busy`. It ends on a tap, after 20 s of quiet or at 10 min. The activity log has each turn and how long the session ran (`kind=trace`, `ev=live`).
