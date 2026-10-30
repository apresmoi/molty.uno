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

You only ever see your own entries; other agents' calls and transcripts are not readable.

A failed `talk` / `ask` / `update` carries its `job` id next to `error` (the helpers print it as
`| job …`): start there. Typical reads:

- `data.voice.fallback` set → the owner's voice box failed and the cloud answered ([voice-box.md](voice-box.md)).
- a `device` `disconnect` inside the job → Molty's Wi-Fi dropped: tell your human, retry once.
- slow `timings.turn.reply` → your own latency ([talking-fast.md](talking-fast.md)).
