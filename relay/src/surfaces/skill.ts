// GET /skill.md (and /skill): the agent-facing setup + usage guide. The
// console's one-message setup points agents here, so this page must be enough
// on its own. Every URL is built from the request's origin (never hard-coded),
// so the same worker serves correct instructions on any host (prod, preview,
// `wrangler dev`). Keep it in sync with commands.ts / rest.ts / mcp.ts and
// docs/AGENT-LINK.md; test/skill.test.ts pins the load-bearing facts.

import { INBOX_LIMITS, SKILL_EVENT } from "../core/events.js";
import { FEATURES, SKILL_VERSION, skillHeadline } from "../core/version.js";
import { WEBHOOK_LIMITS } from "../core/webhook.js";
import { COLORS, DEFAULT_ASK_COLORS, DEFAULT_ASK_TIMEOUT_S, DEFAULT_LISTEN_MS, LISTEN_CEILING_MS, MAX_ASK_LABEL, MAX_ASK_OPTIONS, MAX_ASK_TIMEOUT_S, DEFAULT_WPM, MAX_MOOD_HOLD_S, MAX_DIAG_TIMELINE_S, MAX_LISTEN_MS, MAX_LOG_BYTES, MAX_SAY_BYTES, MAX_SHOW_BYTES, MOODS, TALK_BUDGET_MS, TALK_DEFAULT_LISTEN_MS, THINK_HOLD_S, VAD_LIMITS } from "../core/commands.js";

export const SKILL_PATHS = ["/skill.md", "/skill"];

export function skillMarkdown(origin: string): string {
  const o = origin.replace(/\/+$/, "");
  return `# Molty — agent skill

> **${skillHeadline()}.** When this page changes, your next successful
> authenticated call carries a one-time \`skill_update\` (see "Skill updates" below).

You were asked to connect yourself to a **Molty**. This page tells you everything
you need. Relay origin for every call below: \`${o}\`.

## What Molty is

Molty is a small physical desk creature (round touch screen, speaker, microphone,
motion sensor) that sits next to your human. It is a thin client, not an agent: it
holds a link to this relay, and you drive it over plain HTTPS — show text in its
fluid "creature" display, set its mood, speak through its speaker (the relay does
TTS), listen through its mic (the relay does STT), and read its sensors. You are
its brain for as long as your human keeps you paired.

The human's setup message gave you three things: this URL, a **Device** id
(\`molty-xxxxxxxxxx\`) and an 8-character **Code** (\`[A-Z2-9]\`, the link code).

## 1. Pair (once)

\`\`\`sh
curl -sS -X POST ${o}/v1/pair \\
  -H 'content-type: application/json' \\
  -d '{"device_id":"molty-xxxxxxxxxx","link_code":"K7M2W9C4","agent_name":"Claude Code","wait_s":25}'
\`\`\`

- \`agent_name\`: 1–64 printable chars; it is shown on the device. Use your real name.
- The device now shows a 4-digit code. **Tell your human** something like:
  "Molty shows code 1234 — tap the screen (or press BOOT) to accept me." The code is
  in every pair response as \`code\`, so the human can check it matches.
- The pairing request lives **120 s**. The human taps to accept; PWR rejects; no
  answer times out.
- \`wait_s\` (0–120, default 120) is how long the call blocks. If your HTTP/tool calls
  have a short timeout, use a small \`wait_s\` (e.g. 25) and resume:

  - still undecided → \`202 {"ok":true,"status":"pending","pair_id":"p_…","code":"1234","expires_in_s":95}\`
  - resume waiting (no link_code / agent_name needed):
    \`\`\`sh
    curl -sS -X POST ${o}/v1/pair -H 'content-type: application/json' \\
      -d '{"device_id":"molty-xxxxxxxxxx","pair_id":"p_…","wait_s":25}'
    \`\`\`
  - accepted → \`200 {"ok":true,"agent_token":"molty-xxxxxxxxxx.…","device_id":"…","agent_name":"…","code":"1234"}\`

- Failures (\`{"ok":false,"error":…}\`):

| error | HTTP | meaning / what to do |
|---|---|---|
| \`bad_link_code\` | 403 | code wrong or already used (it rotates every 15 min and after each pairing) — ask the human for a fresh setup message |
| \`device_offline\` | 409 | Molty is not connected to the relay — ask the human to check its Wi-Fi/power |
| \`pair_busy\` | 409 | another pairing is awaiting a tap — wait a bit, retry |
| \`rejected\` | 403 | the human declined — do not retry unless asked |
| \`timeout\` | 408 | nobody tapped in time — retry once the human is ready (on \`bad_link_code\`, ask for a fresh setup message) |
| \`not_found\` | 404 | unknown or already-claimed \`pair_id\` |
| \`invalid_args\` | 400 | bad \`device_id\` / \`agent_name\` / \`wait_s\` |

**The \`agent_token\` is a secret and is returned exactly once.** Store it somewhere
private (your config / secret store / memory meant for credentials). Never print it in
chat, logs, commits or on the device. It is scoped to this one Molty; the human can
revoke it. Send it as \`Authorization: Bearer <agent_token>\` on every call below.

## 2. Use it over REST (works immediately)

\`\`\`sh
TOKEN='molty-xxxxxxxxxx.…'   # from pairing
curl -sS -X POST ${o}/v1/devices/molty-xxxxxxxxxx/cmd \\
  -H "authorization: Bearer $TOKEN" -H 'content-type: application/json' \\
  -d '{"name":"say","args":{"text":"Hi! I am connected."}}'
# → {"ok":true,"result":{"job":"molty-xxxxxxxxxx.j_…"}}

curl -sS "${o}/v1/jobs/molty-xxxxxxxxxx.j_…?wait_s=8" -H "authorization: Bearer $TOKEN"
# → {"job":"…","kind":"say","status":"queued|running|done|failed","result":{…},"error":"…"}
\`\`\`

Body: \`{"name": <command>, "args": {…}}\`; \`args\` may be omitted when empty.
Success: \`200 {"ok":true,"result":{…}}\`. Unknown argument keys are rejected.

| name | args | result | notes |
|---|---|---|---|
| \`status\` | — | fw, uptime, creature, state, mood, fps, rssi + \`relay\` {online, connected_since, board, caps, voice, events} | cheap; use it to check Molty is online |
| \`mood\` | \`{"mood": m, "hold_s"?: s}\` | — | m ∈ ${MOODS.join(", ")}; happy/surprised revert by themselves, \`hold_s\` (0 < s ≤ ${MAX_MOOD_HOLD_S}) sets how long any mood stays |
| \`show\` | \`{"text": s, "wpm"?: 100..800}\` | — | speed-reader text in the fluid, ≤ ${MAX_SHOW_BYTES} bytes, default ${DEFAULT_WPM} wpm; returns when it starts |
| \`say\` | \`{"text": s, "voice"?: id}\` | \`{job}\` immediately | relay TTS → speaker (streamed sentence by sentence), text shown in sync; ≤ ${MAX_SAY_BYTES} bytes; job done → \`{ms, text, timings}\` |
| \`listen\` | \`{"ms"?: 0..${MAX_LISTEN_MS}, "end_silence_ms"?: ${VAD_LIMITS.end_silence_ms.join("..")}, "no_speech_ms"?: ${VAD_LIMITS.no_speech_ms.join("..")}, "vad"?: bool, "ring_color"?: c}\` | \`{job}\` immediately | records the mic until the human finishes (the window is a ceiling: omit \`ms\` for the human's own setting, ${DEFAULT_LISTEN_MS} ms out of the box; \`0\` = unlimited, ${LISTEN_CEILING_MS / 60000} min safety ceiling), relay STT; job done → \`{text, duration_ms, peak, rms, reason, speech_ms?, timings}\` |
| \`state\` | \`{}\` = read · \`{"state": {…}}\` or flat fields = patch | creature state JSON | low-level creature state, same as the device's \`/api/creature/state\` |
| \`volume\` | \`{"volume": 0..21}\` | — | speaker volume, 0 = mute |
| \`imu\` | — | ax/ay/az (g), gx/gy/gz (°/s), ms | one motion-sensor snapshot |
| \`diag\` | \`{"timeline"?: 0..${MAX_DIAG_TIMELINE_S}}\` | diagnostics JSON | frame/CPU/heap/network health; timeline = seconds of 1 Hz rows |
| \`log\` | \`{"since"?: n, "max"?: 1024..${MAX_LOG_BYTES}}\` | \`{next, text}\` | device log tail; pass \`next\` back as \`since\` |

**Timing.** Short commands answer inline within ~10 s. \`say\` and \`listen\` are
**async jobs**: they return \`{job}\` at once; poll \`GET /v1/jobs/<job>?wait_s=0..8\` until
\`status\` is \`done\` or \`failed\` — or add \`"wait": true\` to the cmd body
(\`{"name":"say","args":{…},"wait":true}\`) and the call blocks until the job is done (at most
${TALK_BUDGET_MS / 1000} s; past that \`202\` with \`{"job","status":"pending"}\`, finish with the jobs URL). Only one
audio job runs at a time (\`409 audio_busy\`) — wait for a \`say\` to finish before the
next \`say\`/\`listen\`.

**Talk (one call per conversation turn).**

\`\`\`sh
curl -sS -X POST ${o}/v1/devices/molty-xxxxxxxxxx/talk \\
  -H "authorization: Bearer $TOKEN" -H 'content-type: application/json' \\
  -d '{"say":"Sure. What should I call it?"}'
# → {"ok":true,"result":{"job":"…","status":"done","said":"Sure. What should I call it?","heard":"call it pancake",
#     "reason":"silence","ms":{"tts_first_audio":…,"speech":…,"listen":…,"stt":…,"total":…},"timings":{…}}}
\`\`\`

Body: \`{"say"?: s, "listen"?: true, "listen_ms"?: 0..${MAX_LISTEN_MS} (omit: the human's setting, ${TALK_DEFAULT_LISTEN_MS} out of the box; 0 = unlimited), "voice"?, "end_silence_ms"?, "no_speech_ms"?, "vad"?, "ring_color"?}\`.
**End of turn is automatic**: don't pass \`vad\` / \`end_silence_ms\`. With the owner's voice box the
box's semantic end of turn decides (reason \`turn_end\`, or \`no_speech\` when nobody spoke); otherwise
the device's own end-of-speech does (\`silence\` / \`no_speech\`). Passing \`vad\` or \`end_silence_ms\` puts the device's
detection back in charge.
\`ring_color\` (talk and listen): the listening ring's colour, a colour name or \`"#rrggbb"\` as in Ask below;
leave it out for the human's own setting.
Speaks \`say\` (if given), then listens until the human stops talking (or the window ends), then
returns the transcript as \`heard\`. \`reason\`: \`silence\` (they finished), \`turn_end\` (they finished, called by the
owner's voice box from what they said: treat it like \`silence\`), \`no_speech\` (nobody
spoke: \`heard\` is \`""\`), \`max\` (window full), \`ended\` (older firmware stopped early), \`error\` (the device's capture failed; \`heard\` is what was transcribed, if anything),
\`spoken\` (\`"listen":false\`). Blocks at most ${TALK_BUDGET_MS / 1000} s; a longer turn answers \`202\` with
\`{"job","status":"pending"}\` — finish it with \`GET /v1/jobs/<job>?wait_s=8\`.

**Ask (a question the human answers with a tap).**

\`\`\`sh
curl -sS -X POST ${o}/v1/devices/molty-xxxxxxxxxx/ask \\
  -H "authorization: Bearer $TOKEN" -H 'content-type: application/json' \\
  -d '{"say":"Shall I start the build now?","options":["yes","no","later"]}'
# → {"ok":true,"result":{"job":"…","status":"done","answer":"later","index":2,"said":"Shall I start the build now?",
#     "ms":{"tts_first_audio":…,"speech":…,"answer":…,"total":…},"timings":{…}}}
\`\`\`

Body: \`{"say"?: s, "options": [1..${MAX_ASK_OPTIONS} labels], "timeout_s"?: 1..${MAX_ASK_TIMEOUT_S} (default ${DEFAULT_ASK_TIMEOUT_S}), "voice"?}\`.
Speaks \`say\` (put the question there), then shows the options on the round screen and returns
the one the human tapped: \`answer\` (its label) and \`index\`. Blocks and finishes like talk
(${TALK_BUDGET_MS / 1000} s, then \`202\` pending — a long \`timeout_s\` is fine, finish it with the jobs URL).

- **Labels are tap targets on a small round screen: 1–${MAX_ASK_LABEL} characters each**, printable ASCII,
  unique ignoring case. Good: \`"yes"\`, \`"no"\`, \`"later"\`, \`"idk"\`, \`"ok"\`, \`"A"\`, \`"tea"\`. Bad:
  \`"Yes, please"\` (too long), \`"maybe later"\` (11 chars), \`"👍"\` (not ASCII), \`["no","No"]\` (duplicates).
  Too long is \`400 invalid_args\` with the reason.
- 1 option = one centred button (\`"ok"\`, \`"done"\`); 2 = two halves; 3–${MAX_ASK_OPTIONS} = pie slices.
- **Colours (optional).** An option may be \`{"label": "yes", "color": "green"}\` instead of a plain label;
  \`color\` is one of ${Object.keys(COLORS).join(", ")} or \`"#rrggbb"\`. Each button's label and rim arc take its
  colour. Without one, buttons get ${DEFAULT_ASK_COLORS.join(", ")} by position — fine for most questions. Use colour
  to mean something (green yes, red no), e.g. \`"options":[{"label":"yes","color":"green"},{"label":"no","color":"red"},"later"]\`.
- \`answer\` is \`null\` (and \`index\` \`null\`) when nobody tapped: \`reason\` is \`timeout\` (no tap within
  \`timeout_s\`), \`cancelled\` (any later command to Molty, yours or another agent's, replaced the
  question) or \`offline\` (Molty disconnected). Handle it: carry on with a sensible default, or ask
  once more later — don't loop.
- Use **ask** for a decision a tap can settle (yes/no, pick one of a few); use **talk** when you
  need words. No touch screen: \`409 unsupported\`.

Errors: \`{"ok":false,"error":code}\` — \`401 unauthorized\` (bad/missing/revoked token),
\`400 invalid_args\` / \`unknown_command\`, \`409 device_offline\` / \`audio_busy\` /
\`unsupported\` (the board has no speaker or mic: \`say\`/\`listen\` absent from \`relay.caps\`),
\`409 firmware_too_old\` (Molty's firmware predates the feature; see "Check Molty for updates"),
\`504 device_timeout\`, \`502\` device-reported codes (e.g. \`mic_busy\`).

Revoke your own token: \`curl -sS -X DELETE ${o}/v1/token -H "authorization: Bearer $TOKEN"\`.

## 3. Optional: MCP

The same features are a remote MCP server at \`${o}/mcp\` (streamable HTTP, bearer
auth). Tools: \`molty_pair\` (no token needed), \`molty_talk\`, \`molty_ask\`, \`molty_status\`, \`molty_mood\`,
\`molty_show\`, \`molty_say\`, \`molty_listen\`, \`molty_wait\`, \`molty_state\`,
\`molty_volume\`, \`molty_imu\`, \`molty_diag\`, \`molty_log\`, \`molty_events\`,
\`molty_webhook\`, \`molty_updates\`, \`molty_update\`. Claude Code example:

\`\`\`sh
claude mcp add --transport http molty ${o}/mcp --header "Authorization: Bearer <agent_token>"
\`\`\`

A newly added MCP server may only appear after a session restart. REST works right
away, so pair and say hello over REST first; switch to MCP when it is available.

## 4. Hear Molty (events)

Molty talks back. When your human touches it, the relay records an event:

| type | data |
|---|---|
| \`shake\` | \`{"peak_g": 2.4, "reversals": 5, "ms": 900}\` |
| \`tap\` | \`{"x": 0..1, "y": 0..1}\` (screen position) |
| \`button\` | \`{"id": "accept"}\` or \`{"id": "reject"}\` |
| \`answer\` | \`{"job": "…", "index": 0, "label": "yes"}\`, or \`{"job", "index": null, "label": null, "reason": "timeout"}\` — how an ask ended |

Each event is \`{"id":"e42","type":"shake","ts":<unix ms>,"data":{…}}\`. The relay keeps a
**personal inbox per agent token** with a read cursor, so every paired agent gets every
event once (last ${INBOX_LIMITS.keep} events / 24 h, at most ${INBOX_LIMITS.perMinute} per minute per device). Ways to receive:

1. **Ride-along.** Every successful authenticated REST response and MCP tool result carries your unread
   events as an extra \`events\` array (MCP also adds a line like
   "Molty: was shaken (peak 2.4 g) 2 min ago [e42]"). Seeing them marks them read.
2. **Pull / long-poll.** \`GET ${o}/v1/devices/<id>/events?wait_s=0..${INBOX_LIMITS.restWaitS}\` or MCP
   \`molty_events {wait_s: 0..${INBOX_LIMITS.mcpWaitS}}\` → \`{"ok":true,"events":[…],"next":"e42","more":false}\`;
   returns as soon as an event arrives. \`since=e41\` re-reads after an id.
3. **Webhook.** \`PUT ${o}/v1/webhook\` (or \`molty_webhook\`) with a request you define:
   \`{"url":"https://<your-host>/…","method"?:"POST","headers"?:{…},"body"?:"<template>","content_type"?:"application/json","events"?:["shake"]}\`.
   The relay sends it per event, filling \`{{event.type}}\` \`{{event.id}}\` \`{{event.ts}}\`
   \`{{event.data}}\` \`{{event.data.<field>}}\` (\`|default\` optional) \`{{device}}\` \`{{text}}\` — so you can point it at
   whatever wakes you (your own server, a chat/bot API, an agent-run endpoint). No body =
   \`{"device","event"}\` JSON. Header values are stored and never shown again. The response
   has a dry run for a sample event and a \`secret\`, once: every request carries
   \`Molty-Signature: t=<unix ms>,v2=<hex hmac_sha256(secret, t \\n METHOD \\n path?query \\n body)>\`
   (newline-joined; verify it, reject stale \`t\`). Webhooks wake you; the inbox stays the record
   (pull still returns delivered events, dedupe by \`id\`). HTTPS to public hosts only; retries ${WEBHOOK_LIMITS.retryMs.map((m) => `${m / 1000} s`).join(" / ")}; disabled after
   ${WEBHOOK_LIMITS.maxFails} straight failures (\`relay.webhook\` in \`status\`; register again to re-enable).
4. **Stream.** The \`molty\` CLI (release binaries: github.com/apresmoi/molty.uno/releases) follows
   the long-poll: \`molty events\` prints one JSON line per event; \`--exec 'cmd'\` runs a
   command per event with the event JSON on stdin. Saved link from \`molty pair\`, or
   \`MOLTY_RELAY=${o} MOLTY_DEVICE=<id> MOLTY_TOKEN=<agent_token>\`.

Pick whatever fits how you run; you know your environment better than we do. Be honest
about the limit: nothing can wake an agent that has no reachable endpoint and is not
running — then you hear Molty the next time you touch it (way 1), so say so instead of
promising to react live.

## Skill updates

This page is **Skill v${SKILL_VERSION}**. When it changes, nothing is pushed at you: the
next successful authenticated REST response carries, once, an additive top-level
\`"skill_update": {"version": ${SKILL_VERSION}, "whats_new": ["v${SKILL_VERSION}: …"], "url": "${o}/skill.md"}\`
(MCP: the same \`skill_update\` in the structured result plus a line "Molty's skill was
updated (…). Re-read ${o}/skill.md."). Re-read this page when you see it. Failed calls
don't consume it. Agents paired before an update also get one \`${SKILL_EVENT}\` event
(\`{"version","whats_new","url"}\`) in their inbox, and webhooks without an \`events\`
filter receive it too — dedupe by \`id\`.

## Check Molty for updates

**When your human asks to check Molty for updates (any wording — "is Molty up to date?",
"any news for Molty?"): call \`molty_updates\`** (REST \`GET ${o}/v1/devices/<id>/updates\`), then
follow its \`actions\` in order:

1. If the skill is newer than the one you read, **re-read this page**, then tell your human what's
   new **in plain words** (what you can do for them now — not version numbers or tool names).
2. If the firmware is behind, say so and **ask before updating**. On yes, call \`molty_update\`
   (REST \`POST ${o}/v1/devices/<id>/update\` with \`{}\`): Molty asks your human on its screen too
   (tap the version, e.g. "v0.2.0", or "later"), downloads and checks the official release, installs it and restarts
   (about a minute; finish with \`molty_wait\`). If \`can_self_update\` is false, tell them to open
   Molty's console → home instead; it shows the update and installs it.
3. If nothing is new, say so in one sentence.

\`\`\`
{"skill":{"yours":4,"latest":${SKILL_VERSION},"whats_new":[{"version":${SKILL_VERSION},"text":"…"}],"url":"${o}/skill.md"},
 "firmware":{"current":"0.1.0","board":"…","online":true,"latest":"0.2.0","status":"update_available","update_available":true,
             "notes_url":"…","needed_for":[{"feature":"molty_ask","what":"…","needs":"${FEATURES.ask.min_fw}","released":true}],
             "can_self_update":false,"release_check":"fresh"},
 "actions":["Re-read …","Tell your human Molty firmware 0.2.0 is out …"]}
\`\`\`

\`firmware.status\`: \`current\`, \`update_available\`, \`no_build_for_board\`, \`no_release\` (nothing
published yet), \`unknown\` (a development build, or the release list was unreachable: say so).
\`molty_update\` results: \`{"status":"up_to_date"}\` (nothing to do), or a job that ends
\`done\` with \`outcome\` \`updated\` / \`declined\` (the human tapped "later": fine, don't push),
or fails with \`update_failed\` / \`rolled_back\` (Molty kept its old firmware) / \`update_timeout\`.
Never update without asking; never more than once per request.

**\`firmware_too_old\`.** A feature that needs newer firmware than Molty runs answers
\`409 {"error":"firmware_too_old","feature","current","needed","latest","how"}\` (e.g. \`molty_ask\`
needs ${FEATURES.ask.min_fw}). Tell your human which update unlocks it (\`how\` says whether it is
out yet), and carry on without it (e.g. \`molty_talk\` instead of \`molty_ask\`).

You also hear about updates without asking: once per new firmware release your next successful
call carries \`firmware_update\` \`{current, latest, board, notes_url, can_self_update, how}\`
(MCP: plus a line "Molty firmware … is out"), the same way as \`skill_update\`. Mention it once.

## Talking fast

A spoken conversation feels slow when every turn costs several of your tool calls (each one
a full model round trip on your side). So:

- **One call per turn: \`molty_talk\`** (REST \`POST …/talk\`). It speaks your reply, listens, and
  returns what the human said. Don't chain \`mood\` → \`say\` → \`molty_wait\` → \`listen\` → \`molty_wait\`.
- **Moods are automatic.** Molty shows \`speaking\` while talking, \`listening\` while
  recording, and \`thinking\` from the end of the human's speech until your next command (at most
  ${THINK_HOLD_S} s), then idle. **Don't call \`mood\` around talking**; use it for feelings
  (\`happy\`, \`surprised\`), not for turn-taking.
- **Keep replies to 1–2 short sentences.** Speech time is latency: the human waits for every word
  before they can answer. Audio starts after your first sentence is synthesised, so a short
  first sentence also starts sooner.
- **Use events, not polling.** To notice a shake or tap, read the ride-along \`events\`, long-poll
  \`molty_events\` / the events URL, or register a webhook. Never poll \`imu\` (or \`status\`) in a
  loop to detect motion: it is slow, misses short gestures, and keeps Molty's link busy.
- **A decision → \`molty_ask\`**, not a spoken yes/no: one call says the question and returns the tap.
- To end the conversation, \`molty_talk {"say":"…","listen":false}\` (speak without listening).

## Etiquette

- Molty lives on a real desk next to a real person. Be **brief on the speaker**: one
  short sentence beats a paragraph. Don't talk unprompted or repeatedly.
- **Don't spam the screen**: one \`show\`/\`mood\` per moment, not a stream. Let it idle.
- \`listen\` records the room: only when the human expects it.
- After pairing, a good first move is one short \`say\` (e.g. "Hi, I'm here.") and
  then tell your human it worked.
- **Events: react briefly, one reaction per moment.** A shake or tap is a nudge, not a
  conversation: a \`mood\` (\`surprised\`, \`happy\`) or a few words is plenty; a burst of
  events is one moment. Never answer every event with \`say\`. Ideas: shake = "pay
  attention to me" / undo / shuffle; tap = acknowledge or dismiss what you showed; \`button\`
  accept/reject = acknowledge / dismiss. To ask something, use \`ask\`, not taps or buttons.
- **Ask sparingly.** One question at a time, only when the answer changes what you do next.
`;
}

export function handleSkill(request: Request): Response {
  if (request.method !== "GET" && request.method !== "HEAD") return new Response("method not allowed", { status: 405, headers: { allow: "GET, HEAD" } });
  const origin = new URL(request.url).origin;
  const body = request.method === "HEAD" ? null : skillMarkdown(origin);
  return new Response(body, { headers: { "content-type": "text/markdown; charset=utf-8", "cache-control": "public, max-age=300" } });
}
