# Debug yourself: the activity log

When a turn failed or felt slow, don't guess: read **your own activity** on this Molty (kept 24 h).

```sh
curl -sS "$MOLTY_RELAY/v1/activity?job=<job id>" -H "authorization: Bearer $TOKEN"     # one turn
curl -sS "$MOLTY_RELAY/v1/activity?errors=1&limit=50" -H "authorization: Bearer $TOKEN" # recent failures
# MCP: molty_activity {job?, errors?, since?, limit?, kind?}
```

→ `{"ok":true,"result":{"entries":[…],"next":"a42","more":false}}`, oldest first; pass `next` back as
`since` for newer ones. Each entry: `{id, ts, kind, ev?, op?, job?, args?, ok?, error?, ms?, data?}`:

- `call` — a call you made: `op` (`talk`, `say`, `mood`, …), an args summary (secrets stripped), outcome, `ms`.
- `job` — a job you started, at its end: `data.timings` (every stage, as in the job result),
  `data.voice` (`{tts, stt}` = `box` or the cloud provider, `fallback` when the box failed and the
  cloud took over), `reason`, `heard` / `said` (empty with `redacted: true` when the owner turned
  transcripts off).
- `device_log` — Molty's own log lines tagged with your job (`data.level` `E` / `W` / `I` / `EVT`,
  `data.text`), already on the relay's clock.
- `device` — Molty's `connect` / `disconnect` (`data.code`: the close code) around your jobs.
- `pair` — your own pairing.
- `trace` — later facts about a human turn you were part of (`job` = the turn's id): `ev:"webhook"`
  (its `utterance` delivered to your webhook: `data.status` the HTTP status, `data.ms` the latency,
  `data.attempt`, `data.error` when it failed) and `ev:"reply"` (your answer: `data.reply_ms` /
  `data.first_audio_ms` from the utterance, and the `client_timing` you sent). Listen `job` entries
  also carry `data.trace`: what Molty, the voice box and the relay saw of the turn (lost audio,
  clipping, the end-of-turn decision) — see [talking-fast.md](talking-fast.md#measure-yourself).

You only ever see your own entries; other agents' calls and transcripts are not readable.

A failed `talk` / `ask` / `update` carries its `job` id next to `error` (the helpers print it as
`| job …`): start there. Typical reads:

- `data.voice.fallback` set → the owner's voice box failed and the cloud answered ([voice-box.md](voice-box.md)).
- a `device` `disconnect` inside the job → Molty's Wi-Fi dropped: tell your human, retry once.
- slow end of turn → `timings.listen.end_of_turn`: `silence_ms` (last word to the box's decision), `behind_ms` (the box's lag behind the audio), `rule`, `thr`, `hold_s`. Only with a voice box.
- slow `timings.turn.reply` → your own latency ([talking-fast.md](talking-fast.md)); send
  `client_timing` so the split between your receive, your model and the relay is visible.
- a `trace` `webhook` entry with an `error` or a slow `ms` → your endpoint, not Molty.
