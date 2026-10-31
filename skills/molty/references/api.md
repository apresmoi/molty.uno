# Molty API: pairing, REST, talk, ask, MCP

Every call goes to the relay Molty uses, written `$MOLTY_RELAY` here (the official relay
unless your human runs their own, see [self-hosting.md](self-hosting.md)). `$MOLTY_DEVICE`
is the device id (`molty-xxxxxxxxxx`). With the helpers' config:

```sh
. ~/.config/molty/env; : "${MOLTY_RELAY:=https://molty-relay.alicenet.workers.dev}"
TOKEN=$(cat ~/.config/molty/token)   # never echo it
```

## Pair

Pairing happens once. The human gives you the device id and the 8-character link code
(`[A-Z2-9]`).

```sh
curl -sS -X POST "$MOLTY_RELAY/v1/pair" \
  -H 'content-type: application/json' \
  -d '{"device_id":"molty-xxxxxxxxxx","link_code":"K7M2W9C4","agent_name":"Claude Code","wait_s":25}'
```

- `agent_name`: 1–64 printable chars; it is shown on the device. Use your real name.
- The device now shows a 4-digit code. **Tell your human** something like:
  "Molty shows code 1234 — tap the screen (or press BOOT) to accept me." The code is
  in every pair response as `code`, so the human can check it matches.
- The pairing request lives **120 s**. The human taps to accept; PWR rejects; no
  answer times out.
- `wait_s` (0–120, default 120) is how long the call blocks. If your HTTP/tool calls
  have a short timeout, use a small `wait_s` (e.g. 25) and resume:

  - still undecided → `202 {"ok":true,"status":"pending","pair_id":"p_…","code":"1234","expires_in_s":95}`
  - resume waiting (no link_code / agent_name needed):
    ```sh
    curl -sS -X POST "$MOLTY_RELAY/v1/pair" -H 'content-type: application/json' \
      -d '{"device_id":"molty-xxxxxxxxxx","pair_id":"p_…","wait_s":25}'
    ```
  - accepted → `200 {"ok":true,"agent_token":"molty-xxxxxxxxxx.…","device_id":"…","agent_name":"…","code":"1234","api_version":1}`

- Failures (`{"ok":false,"error":…}`):

| error | HTTP | meaning / what to do |
|---|---|---|
| `bad_link_code` | 403 | code wrong or already used (it rotates every 15 min and after each pairing) — ask the human for a fresh setup message |
| `device_offline` | 409 | Molty is not connected to the relay — ask the human to check its Wi-Fi/power |
| `pair_busy` | 409 | another pairing is awaiting a tap — wait a bit, retry |
| `rejected` | 403 | the human declined — do not retry unless asked |
| `timeout` | 408 | nobody tapped in time — retry once the human is ready (on `bad_link_code`, ask for a fresh setup message) |
| `not_found` | 404 | unknown or already-claimed `pair_id` |
| `invalid_args` | 400 | bad `device_id` / `agent_name` / `wait_s` |

**The `agent_token` is a secret and is returned exactly once.** Store it somewhere
private (your config / secret store / memory meant for credentials). Never print it in
chat, logs, commits or on the device. It is scoped to this one Molty; the human can
revoke it. Send it as `Authorization: Bearer <agent_token>` on every call below.

`api_version` is the agent API contract the relay speaks (see [updates.md](updates.md)).

## Commands (REST, works immediately)

```sh
curl -sS -X POST "$MOLTY_RELAY/v1/devices/$MOLTY_DEVICE/cmd" \
  -H "authorization: Bearer $TOKEN" -H 'content-type: application/json' \
  -d '{"name":"say","args":{"text":"Hi! I am connected."}}'
# → {"ok":true,"result":{"job":"molty-xxxxxxxxxx.j_…"}}

curl -sS "$MOLTY_RELAY/v1/jobs/molty-xxxxxxxxxx.j_…?wait_s=8" -H "authorization: Bearer $TOKEN"
# → {"job":"…","kind":"say","status":"queued|running|done|failed","result":{…},"error":"…"}
```

Body: `{"name": <command>, "args": {…}}`; `args` may be omitted when empty.
Success: `200 {"ok":true,"result":{…}}`. Unknown argument keys are rejected.

| name | args | result | notes |
|---|---|---|---|
| `status` | — | fw, uptime, creature, state, mood, fps, rssi + `relay` {api_version, online, connected_since, board, caps, voice, events, webhook, firmware} | cheap; use it to check Molty is online |
| `mood` | `{"mood": m, "hold_s"?: s}` | — | m ∈ idle, listening, thinking, speaking, happy, sleepy, surprised; happy/surprised revert by themselves, `hold_s` (0 < s ≤ 3600) sets how long any mood stays |
| `act` | `{"action": a}` | `{action}` | a one-shot gesture over the current mood (see [Gestures](#gestures-act-and-inline-cues)); needs `act` in `relay.caps` (`409 unsupported` / `firmware_too_old` otherwise) |
| `show` | `{"text": s, "wpm"?: 100..800}` | — | speed-reader text in the fluid, ≤ 16384 bytes, default 300 wpm; returns when it starts |
| `say` | `{"text": s, "voice"?: id, "in_reply_to"?: turn_id}` | `{job}` immediately | relay TTS → speaker (streamed sentence by sentence), text shown in sync; ≤ 4096 bytes; job done → `{ms, text, timings}` (`text` without its [inline cues](#gestures-act-and-inline-cues) like `[happy]`); `in_reply_to`: see [events.md](events.md#shake-to-talk) |
| `quick` | `{"id": phrase, "in_reply_to"?: turn_id, "force"?: bool}` | `{job, phrase, text, cached}` immediately, or `{skipped:"filled", filler}` | plays a cached [quick phrase](#quick-phrases) at once (no speech synthesis); your next say / talk queues right behind it |
| `listen` | `{"ms"?: 0..30000, "end_silence_ms"?: 300..3000, "no_speech_ms"?: 1000..10000, "vad"?: bool, "ring_color"?: c, "ack"?: true\|false\|"silent"}` | `{job}` immediately | records the mic until the human finishes (the window is a ceiling: omit `ms` for the human's own setting, 30000 ms out of the box; `0` = unlimited, 5 min safety ceiling), relay STT; job done → `{text, duration_ms, peak, rms, reason, speech_ms?, timings}` |
| `state` | `{}` = read · `{"state": {…}}` or flat fields = patch | creature state JSON | low-level creature state, same as the device's `/api/creature/state` |
| `volume` | `{"volume": 0..21}` | — | speaker volume, 0 = mute |
| `imu` | — | ax/ay/az (g), gx/gy/gz (°/s), ms | one motion-sensor snapshot |
| `diag` | `{"timeline"?: 0..180}` | diagnostics JSON | frame/CPU/heap/network health; timeline = seconds of 1 Hz rows |
| `log` | `{"since"?: n, "max"?: 1024..32768}` | `{next, text}` | device log tail; pass `next` back as `since` |

**Timing.** Short commands answer inline within ~10 s. `say` and `listen` are
**async jobs**: they return `{job}` at once; poll `GET /v1/jobs/<job>?wait_s=0..8` until
`status` is `done` or `failed` — or add `"wait": true` to the cmd body
(`{"name":"say","args":{…},"wait":true}`) and the call blocks until the job is done (at most
24 s; past that `202` with `{"job","status":"pending"}`, finish with the jobs URL). Only one
audio job runs at a time (`409 audio_busy`) — wait for a `say` to finish before the
next `say`/`listen`.

## Gestures: act and inline cues

Molty idles by itself (glances, blinks, a stretch now and then: the human's "idle life"
setting). You add expression in two ways:

**Inline cues — the cheap way.** Put `[mood]` or `[action]` in the text of `say`, `talk`'s
`say` or `ask`'s `say`:

```json
{"say": "[happy] Build passed! [thinking] Three tests were flaky… [surprised] wait, one is new."}
```

- A cue is never spoken and never shown: the relay strips it from the speech and the on-screen
  words. It fires when the word after it is spoken (the same timing that shows each word with
  its voice); a cue after the last word fires when the speech ends.
- Names: any mood (idle, listening, thinking, speaking, happy, sleepy, surprised) or action
  (below), any case; `[shake head]` = `[shake-head]` = `[shake_head]`. Unknown or malformed
  cues are dropped silently, and a lone `[` or `]` is dropped too. **A literal bracket: escape
  it**, `\[` and `\]` (in JSON: `"\\[1\\]"`).
- While Molty speaks, its words cover the creature: a mood cue tints the words from there on
  (happy amber, surprised cyan, thinking violet, sleepy blue) and the creature shows the last
  mood cue when the words end (happy / surprised then revert as usual). An action cue plays
  on the creature once its face is back (within 3 s, else it is dropped): put actions at the
  **end** (`"Done. [nod]"`).
- At most 16 cues per say; a say of only cues is `400 invalid_args` (use `mood` / `act`).
  Molty firmware without `say_marks` in `relay.caps` still gets the cues stripped, they just
  don't fire.
- Cues never take the screen from a question or an update, and never cancel one.

**`act {action}`** — a one-shot gesture over the current mood, when you are not speaking:
`wiggle`, `nod`, `shake_head`, `bounce`, `peek`, `glance_left`, `glance_right` (as you look at
Molty), `blink`, `wobble`, `stretch`, `look_up`, `yawn`. It never changes what is shown and
never cancels a question; while words or a question cover the creature it waits up to 3 s,
then is dropped. Silent, ~0.3–2.6 s. A `nod` while they talk (listening) is fine.

## Quick phrases

A quick phrase is a short line ("Hmm…", "Let me think.") whose audio the relay has already
synthesised in Molty's current voice and cached: `quick {id}` plays it **at once** and the call
returns at once. Use it the moment you know you will think for more than a moment, **before**
you start reasoning; then answer with talk / say as usual — it **queues right behind the
phrase** (no `audio_busy`; its speech is synthesised while the phrase plays, so it follows with
no synthesis gap, only a device round trip).

```sh
curl -sS -X POST "$MOLTY_RELAY/v1/devices/$MOLTY_DEVICE/cmd" \
  -H "authorization: Bearer $TOKEN" -H 'content-type: application/json' \
  -d '{"name":"quick","args":{"id":"let_me_think","in_reply_to":"<turn_id>"}}'
# → {"ok":true,"result":{"job":"…","phrase":"let_me_think","text":"Let me think.","cached":true}}
```

- **Built-ins** (every agent, no registration): `hmm` "Hmm…", `let_me_think` "Let me think.",
  `one_sec` "One sec.", `let_me_see` "Let me see.", `good_question` "Good question.", `on_it`
  "On it.", `checking` "Checking.".
- **Your own**, in your voice and personality — set them once:
  `POST $MOLTY_RELAY/v1/devices/<id>/phrases {"phrases":[{"id":"think","text":"Ooh, let me think about that."}]}`
  (upsert; MCP `molty_phrases {action:"set", phrases}`); `GET …/phrases` lists yours and the
  built-ins with `ready` (cached in the current voice); `DELETE …/phrases?id=think` (`id=*` = all
  yours). At most 20 phrases, each ≤ 60 characters (inline cues allowed); ids 1–24 of `a-z 0-9 _ -`;
  yours shadow a built-in of the same id. `"builtins": false` (in a set) turns the built-ins off for
  you (not listed, not playable).
- **Your own fillers.** Mark phrases `"filler": true` (optionally `"for": "question" | "request" |
  "hard" | "any"`): when you last answered a turn and are slow on the next, Molty's own filler is
  yours, picked to fit what the human said (a question → your `question` one, else an `any` one). Their audio is synthesised when you set them and again
  whenever Molty's voice changes; only you see and play yours.
- A quick claims the human's turn like a say (`in_reply_to`, `turn_claimed`).
- After it, with nothing queued, Molty goes back to `thinking`.
- **Molty's own filler.** When the human finishes a turn with words (an `utterance`, or a talk
  that heard them) and no agent does anything (talk, say, quick, mood, …) within the owner's
  delay (default 2.5 s, console setting: off or 0.4–5 s — owners who set 0.5 s hear it almost at once), Molty says a filler itself: yours if you
  answered the last turn and have fillers, else a built-in that fits the turn (`good_question`
  after a question, `on_it` / `checking` after a request, `let_me_think` after something hard,
  else `hmm` / `one_sec` / `let_me_see`).
  The `utterance` and talk's result carry `filler_in_ms` (ms until that happens; absent = off).
  A `quick` right after Molty's filler is skipped — `{"skipped":"filled","filler":{phrase, text,
  ms_ago}}` — so the human never hears two fillers in a row; just answer (`"force":true` plays it
  anyway). Your answer arriving while the filler plays queues right after it.

## Talk (one call per conversation turn)

```sh
curl -sS -X POST "$MOLTY_RELAY/v1/devices/$MOLTY_DEVICE/talk" \
  -H "authorization: Bearer $TOKEN" -H 'content-type: application/json' \
  -d '{"say":"Sure. What should I call it?"}'
# → {"ok":true,"result":{"job":"…","status":"done","said":"Sure. What should I call it?","heard":"call it pancake",
#     "reason":"silence","ms":{"tts_first_audio":…,"speech":…,"listen":…,"stt":…,"total":…},"timings":{…}}}
```

Body: `{"say"?: s, "listen"?: true, "listen_ms"?: 0..30000 (omit: the human's setting, 30000 out of the box; 0 = unlimited), "voice"?, "end_silence_ms"?, "no_speech_ms"?, "vad"?, "ring_color"?, "ack"?, "in_reply_to"?, "client_timing"?}`.

- `in_reply_to` (talk and say): the `turn_id` of the `utterance` you answer (see
  [events.md](events.md#shake-to-talk)).
- `client_timing` (talk, say and ask): **include client_timing to help debug latency** — what your
  own side spent on this turn, in ms: an object of at most 8 keys (lower case, `a-z0-9_`), numbers
  only, each 0..3600000, e.g. `{"received_ms":120,"model_ms":900}`. Suggested keys: `received_ms`
  (the event reached you → this call), `model_ms` (your model's run), `tool_ms`, `queue_ms`,
  `total_ms`. Anything else is `400 invalid_args`. Stored in the job's `timings.client` and, when the
  call answers a human turn, in that turn's trace: the owner sees it on the console's Activity
  timeline next to the relay's own timings ([talking-fast.md](talking-fast.md#measure-yourself)).
- **End of turn is automatic**: don't pass `vad` / `end_silence_ms`. Details in
  [talking-fast.md](talking-fast.md#end-of-turn).
- `ring_color` (talk and listen): the listening ring's colour, a colour name or `"#rrggbb"` as
  in Ask below; leave it out for the human's own setting.
- `ack` (talk and listen): Molty's own acknowledgement when the human finishes (see
  [talking-fast.md](talking-fast.md#the-ack-chirp)): `true` (default) = the human's setting,
  `"silent"` = thinking without the chirp, `false` = none.

Speaks `say` (if given), then listens until the human stops talking (or the window ends), then
returns the transcript as `heard` (with `filler_in_ms`: see [Quick phrases](#quick-phrases)). `reason`:

| reason | meaning |
|---|---|
| `silence` | they finished |
| `turn_end` | they finished, called by the owner's voice box from what they said: treat it like `silence` |
| `no_speech` | nobody spoke: `heard` is `""` |
| `max` | window full |
| `ended` | older firmware stopped early |
| `error` | the voice box failed mid-listen; `heard` is what was transcribed, if anything |
| `spoken` | `"listen":false` |

Blocks at most 24 s; a longer turn answers `202` with `{"job","status":"pending"}` — finish it
with `GET /v1/jobs/<job>?wait_s=8`.

A listen that broke is an error, never `no_speech`: `listen_failed` (Molty's recording failed
mid-listen, e.g. weak Wi-Fi) or `device_offline` (Molty dropped off mid-listen). Say nothing about
silence then; try the turn again once (or tell your human Molty's connection is shaky).
For what to do on `no_speech`, see [talking-fast.md](talking-fast.md#no_speech).

To end the conversation: `{"say":"…","listen":false}` (speak without listening).

## Ask (a question the human answers with a tap)

```sh
curl -sS -X POST "$MOLTY_RELAY/v1/devices/$MOLTY_DEVICE/ask" \
  -H "authorization: Bearer $TOKEN" -H 'content-type: application/json' \
  -d '{"say":"Shall I start the build now?","options":["yes","no","later"]}'
# → {"ok":true,"result":{"job":"…","status":"done","answer":"later","index":2,"said":"Shall I start the build now?",
#     "ms":{"tts_first_audio":…,"speech":…,"answer":…,"total":…},"timings":{…}}}
```

Body: `{"say"?: s, "options": [1..5 labels], "timeout_s"?: 1..120 (default 30), "voice"?, "client_timing"?}` (`client_timing` as in Talk).
Speaks `say` (put the question there), then shows the options on the round screen and returns
the one the human tapped: `answer` (its label) and `index`. Blocks and finishes like talk
(24 s, then `202` pending — a long `timeout_s` is fine, finish it with the jobs URL).

- **Labels are tap targets on a small round screen: 1–8 characters each**, printable ASCII,
  unique ignoring case. Good: `"yes"`, `"no"`, `"later"`, `"idk"`, `"ok"`, `"A"`, `"tea"`. Bad:
  `"Yes, please"` (too long), `"maybe later"` (11 chars), `"👍"` (not ASCII), `["no","No"]` (duplicates).
  Too long is `400 invalid_args` with the reason.
- 1 option = one centred button (`"ok"`, `"done"`); 2 = two halves; 3–5 = pie slices.
- **Colours (optional).** An option may be `{"label": "yes", "color": "green"}` instead of a plain label;
  `color` is one of green, red, amber, blue, violet, pink, cyan, white or `"#rrggbb"`. Each button's label and rim arc take its
  colour. Without one, buttons get green, red, amber, blue, violet by position — fine for most questions. Use colour
  to mean something (green yes, red no), e.g. `"options":[{"label":"yes","color":"green"},{"label":"no","color":"red"},"later"]`.
- `answer` is `null` (and `index` `null`) when nobody tapped: `reason` is `timeout` (no tap within
  `timeout_s`), `cancelled` (any later command to Molty, yours or another agent's, replaced the
  question) or `offline` (Molty disconnected). Handle it: carry on with a sensible default, or ask
  once more later — don't loop.
- Use **ask** for a decision a tap can settle (yes/no, pick one of a few); use **talk** when you
  need words. No touch screen: `409 unsupported`. Firmware too old for it: `409 firmware_too_old`
  (see [updates.md](updates.md#firmware_too_old)).

## While the human speaks: draft and react

During an open human turn (they shook Molty and are still talking; you get `utterance.partial`
events — [events.md](events.md#while-the-human-speaks)) **only these two calls are accepted**;
`say` / `talk` / `ask` / `mood` / `show` / `listen` get `409 human_speaking`.

```sh
# hold a short opener for your answer, based on partial seq 2 (it is synthesised now)
curl -sS -X POST "$MOLTY_RELAY/v1/devices/$DEVICE/draft" -H "authorization: Bearer $TOKEN" \
  -H 'content-type: application/json' \
  -d '{"turn_id":"molty-xxxxxxxxxx.j_…","based_on":2,"opener":"Let me look.","max_new_words":4}'
# → {"ok":true,"result":{"turn_id":"…","based_on":2,"status":"held","max_new_words":4,"threshold":4}}

# a reaction on the screen, no sound (the mic is open)
curl -sS -X POST "$MOLTY_RELAY/v1/devices/$DEVICE/react" -H "authorization: Bearer $TOKEN" \
  -H 'content-type: application/json' -d '{"turn_id":"molty-xxxxxxxxxx.j_…","mood":"happy"}'
# → {"ok":true,"result":{"turn_id":"…","shown":"happy","for_ms":1500,"next_after_ms":3000}}
```

- **draft** `{turn_id, based_on, opener, max_new_words?, voice?}`: `opener` ≤ 24 words, spoken as
  is the instant the human finishes (right after Molty's chirp) when their final words add at most
  `max_new_words` (0..20; the relay caps it with its own calibrated threshold, returned as
  `max_new_words`) beyond partial `based_on` and none of those words turns the meaning ("but",
  "not", "actually", …). Then you own the turn: `draft.used`; else `draft.dropped {reason}`
  ([talking-fast.md](talking-fast.md#drafts-a-head-start-not-an-answer)). A new draft replaces yours
  (≤ 3 per turn: `429 rate_limited`). `409 turn_closed`: they already finished.
- **react** `{turn_id, mood}` (`happy`, `surprised`, `thinking`): shown over the listening
  animation for ~1.5 s, then Molty listens on. Or `{turn_id, action}`: one of the gestures of
  `act` (`nod`, `wiggle`, …), played over the listening. At most one per 3 s per Molty (`429 rate_limited`
  with `retry_after_ms`). Firmware without `react` (a mood) / `act` (an action) in `relay.caps`: `409 unsupported`.
  Molty's own reflex (an owner setting) reacts by itself while they talk; it never limits your
  react, and after yours it stays quiet for the rest of the turn.

## Notes (your private ledger store)

The relay keeps a small key-value store per agent token, readable only with that token (the
owner's activity log records the call, never the value). For your
[behaviour ledger](etiquette.md#keep-a-behaviour-ledger) when you have no memory of your own:

```sh
curl -sS -X PUT "$MOLTY_RELAY/v1/notes" -H "authorization: Bearer $TOKEN" -H 'content-type: application/json' \
  -d '{"key":"ledger","value":[{"at":"2026-10-30 14:02","did":"react happy","response":"they laughed"}]}'
curl -sS "$MOLTY_RELAY/v1/notes?key=ledger" -H "authorization: Bearer $TOKEN"   # → {"key","value","updated"}
curl -sS "$MOLTY_RELAY/v1/notes" -H "authorization: Bearer $TOKEN"              # → {"keys":[{key, bytes, updated}], "bytes", "limits"}
curl -sS -X DELETE "$MOLTY_RELAY/v1/notes?key=ledger" -H "authorization: Bearer $TOKEN"
```

Keys: 1–64 of letters, digits, `. _ : -`. Values: any JSON, ≤ 8 KB each, ≤ 64 keys and 32 KB in
all (`413 too_large`). MCP: `molty_notes {action: list|get|put|delete, key?, value?}`. Revoking
your token deletes them.

## Errors

`{"ok":false,"error":code}`:

| error | meaning |
|---|---|
| `401 unauthorized` | bad/missing/revoked token |
| `400 invalid_args` / `unknown_command` | the call itself is wrong; `message` says why |
| `409 device_offline` | Molty is not connected (Wi-Fi/power), or dropped off mid-listen |
| `409 audio_busy` | another say/listen is running: wait for it, retry once |
| `409 unsupported` | the board lacks it (no speaker or mic: `say`/`listen` absent from `relay.caps`; no touch: no ask) |
| `409 turn_claimed` | another agent is already answering the human's turn: stay quiet ([events.md](events.md#shake-to-talk)) |
| `409 human_speaking` | the human is still talking (an open turn): wait for their `utterance`; meanwhile only draft / react ([events.md](events.md#while-the-human-speaks)) |
| `409 turn_closed` | a draft / react for a turn that is no longer open: answer its `utterance` as usual |
| `429 rate_limited` | a second react within 3 s, or a fourth draft in one turn (`retry_after_ms` when it applies) |
| `413 too_large` | notes over their limits |
| `409 firmware_too_old` | Molty's firmware predates the feature ([updates.md](updates.md#firmware_too_old)) |
| `503 voice_box_offline` | the owner's voice box is required (mode `box`) and down ([voice-box.md](voice-box.md)) |
| `listen_failed` | the recording broke mid-listen |
| `504 device_timeout` | Molty did not answer in time |
| `502` device-reported codes | e.g. `mic_busy` |

A failed `talk` / `ask` / `update` also carries its `job` id next to `error`: look it up with the
[activity log](activity.md) to see where it broke.

Revoke your own token: `curl -sS -X DELETE "$MOLTY_RELAY/v1/token" -H "authorization: Bearer $TOKEN"`.

## Every response may carry more

Any successful authenticated response may also carry, additively at the top level:

- `events` (+ `more_events`): your unread events, now marked read ([events.md](events.md#ride-along)).
- `firmware_update`: once per new firmware release ([updates.md](updates.md#firmware-updates)).

Ignore fields you don't know.

## MCP (optional)

The same features are a remote MCP server at `$MOLTY_RELAY/mcp` (streamable HTTP, bearer
auth). Tools: `molty_pair` (no token needed), `molty_talk`, `molty_quick`, `molty_phrases`, `molty_ask`, `molty_status`, `molty_mood`, `molty_act`,
`molty_show`, `molty_say`, `molty_listen`, `molty_wait`, `molty_state`,
`molty_volume`, `molty_imu`, `molty_diag`, `molty_log`, `molty_events`,
`molty_webhook`, `molty_updates`, `molty_update`, `molty_activity`, `molty_draft`, `molty_react`,
`molty_notes`. Claude Code example:

```sh
claude mcp add --transport http molty "$MOLTY_RELAY/mcp" --header "Authorization: Bearer <agent_token>"
```

A newly added MCP server may only appear after a session restart. REST works right
away, so pair and say hello over REST first; switch to MCP when it is available.
MCP tool results carry the same ride-alongs as REST, plus one line each
("Molty: was shaken (peak 2.4 g) 2 min ago [e42]", "Molty firmware … is out").
