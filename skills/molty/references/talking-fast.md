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
- **Thinking for more than a moment? Play a quick phrase FIRST** — `quick {id}`, one call that
  plays cached audio at once — before you start reasoning, then answer with talk (it follows
  the phrase seamlessly). See [Quick phrases](#quick-phrases).
- **Short first sentence.** Audio starts after the first sentence is synthesised.
- **Keep replies to 1–2 short sentences.** Speech time is latency: the human waits for every word
  before they can answer.
- **Keep a warm session** if your runtime allows (a resident process or kept-alive session beats a
  cold start per event: cold starts cost seconds).
- **Check `relay.caps`, not versions,** to know what Molty can do (`shake_talk`, `ask`, `ack`).
- **Use events, not polling** ([events.md](events.md)).
- **A decision → `molty_ask`**, not a spoken yes/no: one call says the question and returns the tap.
- To end the conversation, `molty_talk {"say":"…","listen":false}` (speak without listening).
- Measure yourself (below).

## Weak Wi-Fi

Don't retry a turn because it got slow: with firmware cap `resume`, a job in `reconnecting` picks up
where it was when Molty returns, the human's words and yours. Retrying would talk over it.

## Measure yourself

- `in_reply_to` adds `timings.turn` `{id, reply, first_audio}` (ms from the utterance to your call
  and to your first audio).
- **Include `client_timing` to help debug latency**: on `talk` / `say` / `ask`, an object of what
  your side spent, in ms — numbers only, at most 8 keys, e.g.
  `"client_timing": {"received_ms": 120, "model_ms": 900}` (`received_ms` = the event reached you →
  this call, `model_ms` = your model's run; `tool_ms`, `queue_ms`, `total_ms` if you have them).
  It costs nothing, never reaches the device, and lands in the turn's trace: the owner's Activity
  timeline then shows shake → speech → end of turn → webhook → your work → your first audio in one
  line, so a slow turn is found in seconds instead of guessed at. Details: [api.md](api.md#talk-one-call-per-conversation-turn).

## Drafts: a head start, not an answer

Partials (`utterance.partial`, [events.md](events.md#while-the-human-speaks)) let you think while
the human talks. The fastest answer starts with a **draft opener** the relay has already
synthesised, so Molty speaks the instant they finish — right after its chirp, no gap.

- **Draft only what fits any ending.** "Sure —", "Good question.", "Let me look.", "Oh nice." —
  never the answer itself, never a commitment ("Done, alarm set for seven"): their last words may
  turn it around ("…but not today"), and an opener that has played cannot be taken back.
- **Base it on the latest partial** (`based_on: seq`): re-draft the same opener on each new
  partial (its audio is kept), and stop drafting once you hear a turn-around word — the relay only
  checks the words *after* `based_on`. The relay drops it (`draft.dropped`) when the
  final words add more than `max_new_words` or contain a turn-around word (but, not, actually,
  wait, …); leave `max_new_words` out unless you want it stricter — the relay tunes its own
  threshold from how drafts fare with this human.
- **On `draft.used`, continue — don't repeat.** Your opener is playing and the turn is yours: send
  the rest with `molty_talk` / `molty_say {in_reply_to: turn_id}` at once. It queues right behind
  the opener (synthesised while the opener plays) and starts the moment it ends.
- **On `draft.dropped`, answer the `utterance` as usual.** Nothing was said.
- **React sparingly** (`react {turn_id, mood|action}`): a `nod` while they explain, a `happy`
  when they say something good, a `surprised` at news — screen only, ≤ 1 per 3 s. Note what landed in your
  [ledger](etiquette.md#keep-a-behaviour-ledger).

## First words: Molty starts, you take over

The owner's setting `first_words` (voice box with `--opener`) makes a small model start the reply the
moment the human finishes: 12 to 20 words that react and name the topic, never a fact. The `utterance`
(and talk's result) carries **`opener`: the whole planned line, playing now**, and `opener_wpm`.

1. **Keep first, with your fastest model, before any thinking**: `keep {words: m, in_reply_to}` (REST
   `POST …/cmd {"name":"keep","args":{"words":m,"in_reply_to":"<turn_id>"}}`, MCP `molty_keep`).
   m = the last word of the opener your answer can follow; prefer a clause end (after a comma or stop);
   all of it is fine. Molty never plays past word m. Result `late: true`: too late for m, Molty
   finishes its clause; use `keep` (the words it plays) and `said` instead.
2. **Reply starting with those m words, word for word**, then your own: opener "Okay, the weather in
   Berlin, for tomorrow, let me walk you through it." + keep 5 → say "Okay, the weather in Berlin, rain
   until noon." Molty switches to your words at word 5 with no seam. Or pass `keep: 5` on talk / say
   and send only "rain until noon." Write the continuation in lower case unless a sentence starts.
3. Slow? Molty pauses at word m (a soft "hmm", then thinking) and goes on when your reply comes.

- No keep and a late reply: Molty finishes the clause playing, says "I mean," and your reply without the
  words already said. A reply that ignores the opener plays after the clause playing now. Both work; the
  keep sounds best.
- `keep` without `words`: where it is (`playing`, `sent`, `words`), nothing claimed.
- Often there is no line (the model was late, or the guard refused it): no `opener`, answer as usual.
- The activity log shows each line, keep and reply plan as `trace` `ev:"opener"`.

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
(the human picks chirp + thinking, thinking only, or off). So when your answer is ready at
once, **go straight to your answer**: no "got it" opener. Pass `"ack": false` (or `"silent"`)
only when you acknowledge in your own way.

**Never leave the human in silence.** The chirp is not words: past a second or two the silence
feels long. If you will think (a slow model, a cold start, tools, search), **play a quick phrase
first** (below), then answer. For long work, also re-send `mood thinking` with a `hold_s` that
covers it (the device's `thinking` lasts 30 s).

## Quick phrases

**Before you start reasoning, play a quick phrase**: `quick {id}` (REST
`POST …/cmd {"name":"quick","args":{"id":"hmm","in_reply_to":"<turn_id>"}}`, MCP `molty_quick`,
helper `quick.sh hmm --reply <turn_id>`). Its audio is cached in Molty's voice: it starts at
once and the call returns at once. Then think, and answer with talk as usual — your talk queues
behind the phrase and follows it seamlessly (its speech is synthesised while the phrase plays).

- **Make them yours, once**: register a few phrases in your own voice and personality
  (`molty_phrases {action:"set", phrases:[{"id":"think","text":"Ooh, let me think about that.","filler":true,"for":"hard"}]}`,
  REST `POST …/phrases`; at most 20, ≤ 60 characters each). `filler: true` ones become Molty's
  own filler when you are slow (by `for`: question / request / hard / any); `builtins: false`
  drops the built-ins for you. Built-ins for everyone: `hmm`,
  `let_me_think`, `one_sec`, `let_me_see`, `good_question`, `on_it`, `checking`.
- **A draft opener is a lead-in too**: your quick / talk after it queues behind it the same way,
  and an opener that is one of your phrases plays from the cache. A played opener already
  answered: Molty says no filler then.
- **Vary them**; match the moment (`checking` before a lookup, `good_question` only after one).
- **Molty fills by itself when you are slow.** No agent action within the owner's delay (2.5 s by
  default, as short as 0.4 s) after the human finishes: Molty says a built-in filler. `filler_in_ms` in the
  `utterance` / talk result says how long you have. A `quick` after Molty's filler is skipped
  (`{"skipped":"filled"}`): go straight to your answer — never two fillers in a row.

## no_speech

`no_speech` means nobody spoke (`heard` is `""`). It is not an error, and it is not a broken
listen: those fail with `listen_failed` / `device_offline` (say nothing about silence then; try
the turn once more).

**After a wake, meet `no_speech` with one short line or nothing — never a burst.** If you started
talking because the human tapped Molty (or shook it, on a Molty without shake to talk) and the
listen comes back `no_speech`, say at most one short line (`{"say":"I'm here.","listen":false}`)
or stay quiet: never several lines, never a loop of listens. In a back-and-forth, `no_speech`
usually means they are done: end with `"listen": false` or let it rest.
