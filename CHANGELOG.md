# Changelog

All notable changes to Molty are recorded here. One version covers the whole product
(firmware, console, relay, CLI); it lives in [`VERSION`](VERSION). How to cut a release:
[docs/RELEASING.md](docs/RELEASING.md).

## Unreleased

First public release.

- **Wi-Fi comes back after a drop, and internal RAM defends itself**: the firmware never
  reconnected a station that lost its AP (ESP-IDF does not do it by itself), so one weak-signal
  drop left Molty offline until a reboot — no relay, no OTA over Wi-Fi — while internal RAM kept
  sinking (2 KB after ~1 h, LCD flushes failing). Now a drop is retried (1 s doubling to 30 s,
  for as long as it takes; never during a join or after a forget). While the station is down the
  sampler no longer asks the driver for an rssi every second and the gateway ping pauses. A heap
  guard watches internal free RAM: 3 s under 12 KB logs one `heap.low` with the suspects and
  moves the fluid's hot fields to PSRAM (frees what it held; slower frames, never a starved
  radio); `heap.ok` once 20 KB stay free. Timeline lines carry `blk=` (allocated internal
  blocks) so a slow leak shows as a climbing count. Host tests: the reconnect and guard
  policies, the shed, and a 200-round link soak that every allocation of a turn is given back.

- **Molty's reflex**: Molty reacts by itself to what the human says while they talk, with any
  agent (also one that never reads partials). Each phrase of the voice box's live transcript is
  classified into a reaction (surprised, happy, sad, laugh, curious, none) and an utterance
  category (question, request, hard, other) and shown screen-only through the react path —
  agents' reacts win, never over a question / pairing / update, one per 3 s. Four engines behind
  one owner setting (`reflex` = off | auto | local | jev | openai | keywords, console Voice card):
  a small CPU emotion model on the voice box (`molty-voice --reflex`), TypeSafe Jev
  (`TYPESAFE_API_KEY`), OpenAI Decisions (`OPENAI_API_KEY`) and a keyword heuristic, which also
  answers whenever the chosen engine has no key or misses its 600 ms deadline. A compare mode
  runs every engine on each phrase; every decision (engine, latency, size, pick, confidence,
  outcome, fallback) is in the turn trace and the Activity timeline. `molty-sim reflex-bench`
  scores the engines on 40 labelled phrases.
  **Liveliness** (`liveliness` = calm | normal | lively, default normal, console Voice card): the
  first decision at three words (no longer at a sentence end — short turns used to get theirs
  after they closed), then every 3–6 words or 1.2–2.5 s; lower gates, and an unsure-but-plausible
  pick shows as a small gesture (a glance, a look-up, a wiggle); one reflex per 1.5–3 s. A newer
  phrase's answer wins over an older one still in flight (`stale`); at most two engine calls at
  once. The voice box classifies every word count from three. On the device, Molty keeps an
  attentive baseline while the human talks (a blink or a small glance on the voice's rhythm once
  nothing moved it for 2.5 s; calm idle life 5 s; off: none), and its own listener reactions wait
  for a reflex / agent react instead of cutting it short. `molty-sim reflex-turns` measures it
  over whole spoken turns (fires per turn, time to the first reaction, false fires).
- **Quick phrases and the auto filler**: agents register their own short phrases
  (`molty_phrases`, ≤ 20 × 60 characters; built-ins `hmm`, `let_me_think`, `one_sec`, …) whose
  audio the relay pre-synthesises in Molty's current voice and caches; `quick {id}` /
  `molty_quick` plays one at once with no TTS, and the agent's talk queues right behind it.
  When nobody answers a human turn within the owner's delay (console "filler": off or
  1.5–5 s, 2.5 s by default; firmware reports `filler_ms` per listen), Molty says a built-in
  filler itself; the agent's reply queues after it. Draft openers, quick phrases and fillers
  share one lead-in queue and one pre-synthesised speech path; a played opener counts as the
  agent answering (no filler); fillers and quicks appear in the turn trace. Agents may mark their
  phrases `filler: true` (`for: question | request | hard | any`) and turn the built-ins off; Molty's
  filler uses the last answering agent's fillers, picked by the utterance's category from a
  pluggable classifier — Molty's reflex engine when it is on, keywords otherwise.

- **Expressive Molty**: inline mood / action cues in what agents say (`"[happy] Build passed!
  … [nod]"`: stripped from the speech and the screen, fired on their word — firmware cap
  `say_marks`); `act {action}` / `molty_act` one-shot gestures (wiggle, nod, shake_head,
  bounce, peek, glances, blink, wobble, stretch, look_up, yawn — cap `act`); an idle life of
  small actions every 10–40 s, a yawn before dozing off and a smile after a finished say, with
  a console setting (lively / calm / off). The actions' curves have a JS twin
  (`device/creatures/js/actions.js`) held to the firmware's by a golden.

- **Repo layout**: components grouped by where they run. `firmware/`, `console/`, `creatures/`
  → `device/`; `relay/` → `services/relay/`; `voicebox/` → `services/molty-voice/` (the
  `molty_voice` package and `molty-voice` command keep their names); `cli/`, `website/` →
  `apps/`; `scripts/` → `tools/scripts/`. `make` targets are unchanged.
- **Firmware** (Waveshare ESP32-S3-Touch-LCD-1.46): the fluid of characters, the `.molty`
  creature runtime with 29 cute-kit creatures, speaker + microphone, IMU, touch, USB-C
  networking (NCM) + Wi-Fi with mDNS, rollback-protected OTA, and the agent link
  (pair with an on-device code and a tap). `/api/info` reports the product version as `fw`.
- **Console**: the setup flow (Wi-Fi → Molty → Agent → Tap → Done) and settings pages,
  served by the device from its SPIFFS partition.
- **Relay** (Cloudflare Worker): device WSS, pairing, REST and remote MCP for agents,
  Workers AI TTS/STT. `GET /v1/health` reports the version.
- **CLI**: `molty` device verbs and a stdio MCP server; `molty --version`.
- **Molty talks back** (relay + CLI): `shake`, `tap` and `button` events reach every paired
  agent through a per-agent inbox with a read cursor (storage-backed, 200 events / 24 h,
  30/min). Agents receive them riding along on every REST/MCP response, by pulling
  (`GET /v1/devices/<id>/events`, `molty_events`, long-poll), through an agent-defined signed
  webhook (`PUT /v1/webhook`, `molty_webhook`: templated HTTPS request, HMAC
  `Molty-Signature`, retries, auto-disable) or with `molty events [--exec CMD]`. The skill
  explains the modes so each agent sets up its own end. `mood` takes an optional `hold_s`.
- **Agent skill as a plugin**: the agent skill is one folder in the repo, `skills/molty/`
  (a short `SKILL.md`, references, shell helpers), distributed only through the git plugin
  marketplaces: `claude plugin marketplace add apresmoi/molty.uno && claude plugin install
  molty@molty`, or `codex plugin marketplace add apresmoi/molty.uno && codex plugin add
  molty@molty`. The relay no longer serves or versions it: `GET /skill.md` is a deprecated `301`
  to the skill on GitHub, the `skill_update` / `skill.updated` notices are gone, and agents read
  the contract as `api_version` (pair result, `status` `relay.api_version`, `/v1/health`,
  `molty_updates`, MCP `serverInfo`). The console's setup message points at the skill and names
  the relay only when it is a private one.
- **Boards**: a board layer (`firmware/main/board/molty_board.h`, one folder per board
  under `firmware/boards/`, [docs/PORTING.md](docs/PORTING.md)). The fluid is sized at
  runtime from the board's display; absent hardware degrades (`unsupported` voice/IMU
  endpoints, button-only pairing). Second board: Waveshare ESP32-S3-Touch-LCD-1.28
  (240×240, no audio, Wi-Fi only), experimental. OTA refuses another board's image; the
  agent link reports `board` and only the `caps` the board has.
- **Setup over USB for serial boards**: boards whose USB-C is a USB-UART bridge (the 1.28)
  get Wi-Fi from the browser over the cable (Improv Wi-Fi Serial on the console UART);
  `molty.uno/setup` installs molty.UNO per board and sets Wi-Fi with ESP Web Tools, with
  the images served by the site.
- **molty.uno/build**: will my board work (from docs/PORTING.md), the reference build's
  parts (board, cable, optional battery and an untested vibration add-on with its wiring),
  community builds from every `board.json` (new optional `buy` and `photos`), and a
  "want one built?" waitlist. The relay stores it: `POST /waitlist` into D1 (address,
  time, form only), per-IP rate limit, honeypot, optional Turnstile. New board PRs use
  `.github/PULL_REQUEST_TEMPLATE/board.md`.
- **Stable presence**: the device boots into the creature and stays there; re-sending a
  state or mood no longer melts it (moods blend), the demo reel runs only on
  `state:"autoplay"`, `show` returns to the creature 1.5 s after its last word, quick
  moods (`happy`, `surprised`) revert (`mood` takes an optional `hold_s`). Idle life: a
  short mood flash every 20–60 s, sleepy after 5 min alone. Wi-Fi / relay / auth trouble
  is a small mark at the screen's edge instead of a state change.
- **Device events**: `shake`, `tap` and `button` evt frames to the agent
  ([docs/AGENT-LINK.md](docs/AGENT-LINK.md#device-events)); a shake makes the Molty jump.
- **Release assets**: per board, OTA image and full factory image
  (`molty-X.Y.Z-<board>-{ota,factory}.bin`); console page, CLI binaries for
  linux/darwin/windows × amd64/arm64, `SHA256SUMS`.
- **While the human speaks** (relay + firmware + skill): during a shake turn agents get
  `utterance.partial` chunks by meaning (sentence end, ~1/3 of the human's usual turn, none
  before 3 s) as context only; their `say` / `talk` / `ask` / `mood` / `show` get `409
  human_speaking`. They may `draft` an opener — synthesised while the human talks, played the
  instant they finish when the last words don't change the meaning (`draft.used`, the follow-up
  queues behind it with no gap; else `draft.dropped`) — and `react` with a mood (screen only,
  firmware cap `react`: not an explicit mood, so the turn's chirp stays) or a gesture (`act`).
  Partials, drafts, reacts and the draft outcome are in the turn trace. The relay calibrates
  from usage (turn length, speech rate, pauses, draft outcomes → partial points and the draft
  threshold; owner `GET /v1/devices/<id>/speech`, console **speech habits** card, activity
  log). Per-agent private notes (`/v1/notes`, `molty_notes`) for the behaviour ledger.
  Webhooks get partials only when `events` names them
  ([docs/AGENT-LINK.md](docs/AGENT-LINK.md#while-the-human-speaks)).
