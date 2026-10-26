// GET /skill.md (and /skill): the agent-facing setup + usage guide. The
// console's one-message setup points agents here, so this page must be enough
// on its own. Every URL is built from the request's origin (never hard-coded),
// so the same worker serves correct instructions on any host (prod, preview,
// `wrangler dev`). Keep it in sync with commands.ts / rest.ts / mcp.ts and
// docs/AGENT-LINK.md; test/skill.test.ts pins the load-bearing facts.

import { DEFAULT_LISTEN_MS, DEFAULT_WPM, MAX_DIAG_TIMELINE_S, MAX_LISTEN_MS, MAX_LOG_BYTES, MAX_SAY_BYTES, MAX_SHOW_BYTES, MOODS } from "../core/commands.js";

export const SKILL_PATHS = ["/skill.md", "/skill"];

export function skillMarkdown(origin: string): string {
  const o = origin.replace(/\/+$/, "");
  return `# Molty — agent skill

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
| \`mood\` | \`{"mood": m}\` | — | m ∈ ${MOODS.join(", ")} |
| \`show\` | \`{"text": s, "wpm"?: 100..800}\` | — | speed-reader text in the fluid, ≤ ${MAX_SHOW_BYTES} bytes, default ${DEFAULT_WPM} wpm; returns when it starts |
| \`say\` | \`{"text": s, "voice"?: id}\` | \`{job}\` immediately | relay TTS → speaker, text shown in sync; ≤ ${MAX_SAY_BYTES} bytes; job done → \`{ms, text}\` |
| \`listen\` | \`{"ms"?: 1..${MAX_LISTEN_MS}}\` | \`{job}\` immediately | records the mic (default ${DEFAULT_LISTEN_MS} ms), relay STT; job done → \`{text, duration_ms, peak, rms}\` |
| \`state\` | \`{}\` = read · \`{"state": {…}}\` or flat fields = patch | creature state JSON | low-level creature state, same as the device's \`/api/creature/state\` |
| \`volume\` | \`{"volume": 0..21}\` | — | speaker volume, 0 = mute |
| \`imu\` | — | ax/ay/az (g), gx/gy/gz (°/s), ms | one motion-sensor snapshot |
| \`diag\` | \`{"timeline"?: 0..${MAX_DIAG_TIMELINE_S}}\` | diagnostics JSON | frame/CPU/heap/network health; timeline = seconds of 1 Hz rows |
| \`log\` | \`{"since"?: n, "max"?: 1024..${MAX_LOG_BYTES}}\` | \`{next, text}\` | device log tail; pass \`next\` back as \`since\` |

**Timing.** Every call is designed to answer within ~10 s. Short commands answer
inline. \`say\` and \`listen\` are **async jobs**: they return \`{job}\` at once; poll
\`GET /v1/jobs/<job>?wait_s=0..8\` until \`status\` is \`done\` or \`failed\`. Only one
audio job runs at a time (\`409 audio_busy\`) — wait for a \`say\` to finish before the
next \`say\`/\`listen\`.

Errors: \`{"ok":false,"error":code}\` — \`401 unauthorized\` (bad/missing/revoked token),
\`400 invalid_args\` / \`unknown_command\`, \`409 device_offline\` / \`audio_busy\` /
\`unsupported\` (the board has no speaker or mic: \`say\`/\`listen\` absent from \`relay.caps\`),
\`504 device_timeout\`, \`502\` device-reported codes (e.g. \`mic_busy\`).

Revoke your own token: \`curl -sS -X DELETE ${o}/v1/token -H "authorization: Bearer $TOKEN"\`.

## 3. Optional: MCP

The same features are a remote MCP server at \`${o}/mcp\` (streamable HTTP, bearer
auth). Tools: \`molty_pair\` (no token needed), \`molty_status\`, \`molty_mood\`,
\`molty_show\`, \`molty_say\`, \`molty_listen\`, \`molty_wait\`, \`molty_state\`,
\`molty_volume\`, \`molty_imu\`, \`molty_diag\`, \`molty_log\`. Claude Code example:

\`\`\`sh
claude mcp add --transport http molty ${o}/mcp --header "Authorization: Bearer <agent_token>"
\`\`\`

A newly added MCP server may only appear after a session restart. REST works right
away, so pair and say hello over REST first; switch to MCP when it is available.

## Etiquette

- Molty lives on a real desk next to a real person. Be **brief on the speaker**: one
  short sentence beats a paragraph. Don't talk unprompted or repeatedly.
- **Don't spam the screen**: one \`show\`/\`mood\` per moment, not a stream. Let it idle.
- \`listen\` records the room: only when the human expects it.
- After pairing, a good first move is one short \`say\` (e.g. "Hi, I'm here.") and
  then tell your human it worked.
`;
}

export function handleSkill(request: Request): Response {
  if (request.method !== "GET" && request.method !== "HEAD") return new Response("method not allowed", { status: 405, headers: { allow: "GET, HEAD" } });
  const origin = new URL(request.url).origin;
  const body = request.method === "HEAD" ? null : skillMarkdown(origin);
  return new Response(body, { headers: { "content-type": "text/markdown; charset=utf-8", "cache-control": "public, max-age=300" } });
}
