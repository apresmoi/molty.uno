# Hearing Molty: events, the utterance, inbox, long-poll, webhooks

Molty talks back. When your human touches it, the relay records an event:

| type | data |
|---|---|
| `shake` | `{"peak_g": 2.4, "reversals": 5, "ms": 900}`; with shake to talk also `"listening": true, "turn_id": "…"` — Molty is already listening: do nothing, its `utterance` follows |
| `utterance` | `{"turn_id": "molty-…j_…", "text": "what's on my calendar?", "reason": "turn_end", "ms": {"speech": 1800, "stt": 120}, "trigger": "shake", "device": {"caps": […], "mood": "thinking", "voice": "box"}}` — the human's words, once per turn (see Shake to talk) |
| `tap` | `{"x": 0..1, "y": 0..1}` (screen position) |
| `button` | `{"id": "accept"}` or `{"id": "reject"}` — the board's physical buttons. Not an answer to a question: they mean acknowledge / dismiss whatever you showed (see [etiquette.md](etiquette.md)). Answers to a question come only as `answer` |
| `answer` | `{"job": "…", "index": 0, "label": "yes"}`, or `{"job", "index": null, "label": null, "reason": "timeout"}` — how an ask ended |
| `utterance.partial` | `{"turn_id": "…", "seq": 1, "text_so_far": "what's on my calendar", "new_text": "what's on my calendar", "elapsed_ms": 3200}` — the human is **still talking**: context only (see While the human speaks) |
| `draft.used` | `{"turn_id", "based_on", "opener", "new_words", "job"}` — to you only: your draft's opener is playing, you own the turn |
| `draft.dropped` | `{"turn_id", "based_on", "reason", "new_words"?}` — to you only: your draft will not be spoken; answer the `utterance` as usual |

Ignore event types you don't know: more will come. Whatever comes, never speak while the human talks.

Each event is `{"id":"e42","type":"shake","ts":<unix ms>,"data":{…}}`.

## Shake to talk

When Molty lists `shake_talk` in `relay.caps` and its human leaves the setting on (out of the box),
**a shake means "I want to talk"**. Molty starts listening by itself, at once (listening ring), the
relay transcribes with the usual end of turn, and every paired agent gets **one `utterance` event**
with the human's words — nothing at all when nobody spoke. You also get the `shake` (with
`"listening": true`): **don't react to it**, the `utterance` follows within seconds.

1. On `utterance`: answer at once with `molty_talk {"say": "<your answer>", "in_reply_to": "<turn_id>"}`
   (REST `POST …/talk`; helper `talk.sh "…" --reply <turn_id>`). Molty already chirped and shows
   `thinking`: no "yeah?", no "let me check". Keep listening (`listen` default true) for a
   back-and-forth, or `"listen": false` to finish.
2. **The first agent to answer a turn claims it.** For 60 s other agents' `say` / `talk` get
   `409 turn_claimed` (not `audio_busy`): that's fine, stay quiet. All paired agents get the
   event for now (routing a gesture to one agent comes later).
3. `in_reply_to` adds `timings.turn` `{id, reply, first_audio}`: ms from the utterance to your
   call and to your first audio — your real response time.

The setting off (or older firmware): a plain `shake` (no `listening`) — a nudge, as before.

An `utterance` with `"claimed": true`: another agent's draft opener already answers it — stay quiet.

## While the human speaks

With the owner's voice box, a longer turn also brings **`utterance.partial`** events while the
human is still talking: chunks by meaning (a sentence end, or about a third of how long this human
usually talks), never word by word, none before the turn ran ~3 s. `seq` counts them per turn,
`text_so_far` is everything heard so far, `new_text` what this one adds.

- **They are context, not a question.** The relay enforces it: until the `utterance`, your `say` /
  `talk` / `ask` / `mood` / `show` get `409 human_speaking`. Think ahead; don't answer.
- Two things are allowed meanwhile: **`draft`** a short opener (played the instant they finish, if
  their last words don't change the meaning — then `draft.used` and the turn is yours; otherwise
  `draft.dropped`), and **`react`** with a mood on the screen (no sound, ≤ 1 per 3 s). See
  [api.md](api.md#while-the-human-speaks-draft-and-react) and
  [talking-fast.md](talking-fast.md#drafts-a-head-start-not-an-answer).
- **Molty reacts on its own too** (its *reflex*, when the owner turned it on): while they talk it
  shows a reaction to what they say (surprised, happy, a nod, a bounce, a peek) — screen only, at
  most one per 3 s, never over a question, a pairing or an update. You need do nothing for it, and
  it works with agents that never read partials. **Your `react` wins**: once you react in a turn,
  the reflex stays quiet for the rest of that turn. Don't echo it with your own reacts.
- Webhooks get partials **only when you ask** (`"events": ["utterance.partial", "utterance", …]`):
  they are chatty. Pull and ride-along carry them as usual. `draft.used` / `draft.dropped` reach
  only the agent that drafted.

## Inbox

The relay keeps a **personal inbox per agent token** with a read cursor, so every paired agent
gets every event once (last 200 events / 24 h, at most 30 per minute per device). A newly paired
agent starts at the head: it hears what happens from now on.

## Ways to receive

Pick whatever fits how you run; you know your environment better than we do. The
[harness references](harnesses/) show concrete setups.

### Ride-along

Every successful authenticated REST response and MCP tool result carries your unread
events as an extra `events` array (at most 10; `more_events` when more remain; MCP also adds a
line like "Molty: was shaken (peak 2.4 g) 2 min ago [e42]"). Seeing them marks them read.

### Pull / long-poll

`GET $MOLTY_RELAY/v1/devices/<id>/events?wait_s=0..25` or MCP `molty_events {wait_s: 0..8}` →
`{"ok":true,"events":[…],"next":"e42","more":false}`; returns as soon as an event arrives.
`since=e41` re-reads after an id. At most 2 concurrent waits per token (`429 too_many_polls`).
Helper: `watch.py` (forever) or `watch.py --once [wait_s]`.

### Webhook

`PUT $MOLTY_RELAY/v1/webhook` (or `molty_webhook`) with a request you define:

```json
{"url":"https://<your-host>/…","method"?:"POST","headers"?:{…},"body"?:"<template>","content_type"?:"application/json","events"?:["utterance"]}
```

`events` limits the types sent; without it you get every type **except `utterance.partial`**,
which a webhook only gets when it names it.

- The relay sends it per event, filling `{{event.type}}` `{{event.id}}` `{{event.ts}}`
  `{{event.data}}` `{{event.data.<field>}}` (nested up to 3 deep: `{{event.data.ms.speech}}`,
  `{{event.data.device.mood}}`; a default after a pipe, e.g. `{{event.data.text|(nothing said)}}`, is used when the field is missing — literal text, escaped like a string; without one a missing field is `null` in a JSON body and empty elsewhere) `{{device}}` `{{text}}` — so you can point it at
  whatever wakes you (your own server, a chat/bot API, an agent-run endpoint). No body =
  `{"device","event"}` JSON.
- String placeholders are escaped for the body's `content_type` (JSON-safe in a JSON body, so
  quotes in the human's words can't break it); `{{event.data}}` and non-string fields insert JSON
  (write them unquoted).
- For conversation, a body like
  `{"message":"Molty: the human said \"{{event.data.text}}\" (turn {{event.data.turn_id}})"}` with
  `"events":["utterance"]` hands your runtime everything it needs: no confirming call.
- Header values are stored and never shown again. The response has a dry run for a sample
  event and a `secret`, once: every request carries
  `Molty-Signature: t=<unix ms>,v2=<hex hmac_sha256(secret, t \n METHOD \n path?query \n body)>`
  (newline-joined; verify it, reject stale `t`).
- Webhooks wake you; the inbox stays the record (pull still returns delivered events, dedupe by `id`).
- HTTPS to public hosts only; retries 1 s / 10 s / 60 s; disabled after 20 straight failures
  (`relay.webhook` in `status`; register again to re-enable).
- `GET` reads your webhook (secrets redacted), `DELETE` removes it.

### Stream (CLI)

The `molty` CLI (release binaries: github.com/apresmoi/molty.uno/releases) follows
the long-poll: `molty events` prints one JSON line per event; `--exec 'cmd'` runs a
command per event with the event JSON on stdin. Saved link from `molty pair`, or
`MOLTY_RELAY=$MOLTY_RELAY MOLTY_DEVICE=<id> MOLTY_TOKEN=<agent_token>`.

## The honest limit

Nothing can wake an agent that has no reachable endpoint and is not running — then you hear
Molty the next time you touch it (ride-along), so say so instead of promising to react live.

Use events, not polling: never poll `imu` (or `status`) in a loop to detect motion — it is slow,
misses short gestures, and keeps Molty's link busy.
