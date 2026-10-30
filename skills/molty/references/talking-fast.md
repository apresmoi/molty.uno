# Talking fast

A spoken conversation feels slow when every turn costs several of your tool calls (each one
a full model round trip on your side). The human hears your latency as silence.

## Latency rules

- **Act on the event payload directly.** An `utterance` (webhook body, ride-along or pull) holds
  everything: `text`, `turn_id`, `reason`, timings, Molty's state. No confirming `events` /
  `status` call, no lock files or bookkeeping before you answer.
- **One call per turn: `molty_talk`** (REST `POST …/talk`, helper `talk.sh`). It speaks your
  reply, listens, and returns what the human said. Don't chain `mood` → `say` → `molty_wait` →
  `listen` → `molty_wait`.
- **Use your fastest model for conversation.** Escalate to a bigger model or tools only for real
  work — and then say one short line first ("Checking your calendar.") so the human hears you.
- **Short first sentence.** Audio starts after the first sentence is synthesised.
- **Keep replies to 1–2 short sentences.** Speech time is latency: the human waits for every word
  before they can answer.
- **Keep a warm session** if your runtime allows (a resident process or kept-alive session beats a
  cold start per event: cold starts cost seconds).
- **Check `relay.caps`, not versions,** to know what Molty can do (`shake_talk`, `ask`, `ack`).
- **Use events, not polling** ([events.md](events.md)).
- **A decision → `molty_ask`**, not a spoken yes/no: one call says the question and returns the tap.
- To end the conversation, `molty_talk {"say":"…","listen":false}` (speak without listening).
- Measure yourself: `in_reply_to` adds `timings.turn` `{id, reply, first_audio}` (ms from the
  utterance to your call and to your first audio).

## End of turn

**End of turn is automatic**: don't pass `vad` / `end_silence_ms`. With the owner's voice box the
box's semantic end of turn decides (reason `turn_end`, or `no_speech` when nobody spoke); otherwise
the device's own end-of-speech does (`silence` / `no_speech`). Passing `vad` or `end_silence_ms`
puts the device's detection back in charge.

The listen window is a ceiling, not a duration: omit `ms` / `listen_ms` for the human's own
setting (30 s out of the box); `0` = unlimited (ends on end of turn, `no_speech`, or a 5 min
safety ceiling, reason `max`).

## Moods are automatic

Molty shows `speaking` while talking, `listening` while recording, and `thinking` from the end of
the human's speech until your next command (at most 30 s; 15 s on older firmware), then idle.
**Don't call `mood` around talking**; use it for feelings (`happy`, `surprised`), not for
turn-taking.

## The ack chirp

**Molty acknowledges the turn itself.** The moment the human finishes, Molty plays a short,
soft chirp and shows `thinking` — on the device, before you have even seen the transcript
(the human picks chirp + thinking, thinking only, or off). So **go straight to your answer**:
no "hmm", "let me think" or "got it" opener. Pass `"ack": false` (or `"silent"`) only when you
acknowledge in your own way.

**Never leave the human in silence.** The device's `thinking` lasts 30 s. If your answer needs
longer work (tools, search, a build), first send a short spoken ack ("On it — give me a minute.")
with `say`, or re-send `mood thinking` with a `hold_s` that covers the work, then answer.

## no_speech

`no_speech` means nobody spoke (`heard` is `""`). It is not an error, and it is not a broken
listen: those fail with `listen_failed` / `device_offline` (say nothing about silence then; try
the turn once more).

**After a wake, meet `no_speech` with one short line or nothing — never a burst.** If you started
talking because the human tapped Molty (or shook it, on a Molty without shake to talk) and the
listen comes back `no_speech`, say at most one short line (`{"say":"I'm here.","listen":false}`)
or stay quiet: never several lines, never a loop of listens. In a back-and-forth, `no_speech`
usually means they are done: end with `"listen": false` or let it rest.
