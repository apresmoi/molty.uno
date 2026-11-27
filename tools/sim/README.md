# Simulated Molty (`tools/sim`)

Measure and tune the conversation loop without the physical device:

```
scripted human ──mic, 100 ms chunks / 300 ms frames──▶ sim device ──WS /v1/device──▶ relay (wrangler dev)
 (Kyutai TTS, cached)                                   (firmware wire + timing)          │  ▲
                                                                                          ▼  │ /v1/voice
agent (scripted | local LLM) ──REST talk/say──▶ relay ◀────────────── molty-voice (this repo, own --config)
                                                                         └─ ssh tunnel ─▶ Kyutai STT/TTS on the box
```

- **Sim device** (`molty_sim/device.py`) speaks the firmware's side of `docs/AGENT-LINK.md`: hello with
  its own key and the 1.46's caps (`say_cues listen_stop listen_window ack …`), TOFU, auto-accepted
  pairing (it "taps" itself), cmd/res, the mic as the firmware records it (100 ms chunks, 300 ms
  frames, `listen_stop` honoured within one chunk, end reason + `speech_ms` like the firmware, the
  device VAD ported 1:1 for listens without `vad:false`), the turn acknowledgement rule, and a
  speaker model for `say` (first-frame time, underruns, chirp cut, `say.done` after the DMA tail,
  every word cue and when it would show).
- **Simulated human** (`scenarios/*.yaml`): per turn, TTS text with `{pause S}` markup (synthesised in
  one stream by the box's Kyutai TTS with varied voices, pauses cut in between words, cached in
  `.cache/` so every run replays identical audio), a WAV, or silence; room noise (pink/white/brown),
  level, start delay, listen window. An `{llm: …}` user line is reserved in the format (not implemented).
- **Agent**: scripted lines with a fixed think time (`--agent-gap-ms`, default 0, so the gap is the
  pipeline's own), `--agent llm` (any OpenAI-compatible endpoint, default Ollama on the box), or
  `--agent facts` (a fact sheet and seeded think times, see "Talk baseline" below).
- **Metrics** per turn and summarised (`runs/<ts>/report.{md,json}`, plus every WAV).

Why Python: it reuses `services/molty-voice/` directly (Kyutai TTS/STT clients, resampler, framing) and runs the
second molty-voice bridge from the same venv; the relay side needs no hook (a stock `wrangler dev`).

## The voice front (`molty-sim front`)

The realtime voice front end to end, no device and no key: a local `wrangler dev`, a sim device,
a local `molty-voice run --engine realtime` and a fake realtime provider
(`services/molty-voice/molty_voice/fake_realtime.py`). The person shakes Molty and says a WAV;
the front answers, hands the turn to a scripted agent, says its answer, listens again, and the
conversation ends when nobody speaks. It prints each turn's activity line and the sim device's own
measures (voice ended -> listen_stop -> Molty's first sound -> the agent's answer).

```sh
cd services/relay && pnpm install --frozen-lockfile && cd ../..
cd tools/sim && uv run molty-sim front                          # synthetic clip, fake provider
say -o /tmp/q.wav --data-format=LEI16@16000 "What is on my calendar today?"
uv run molty-sim front --wav /tmp/q.wav --live                  # the real provider: MOLTY_REALTIME_KEY in this shell
```

`--live` passes the key from this command's environment to the bridge child process only; the run
directory keeps the logs (no key in them) and deletes the bridge's voice token at the end.

### Scripted conversations (`molty-sim front --script`)

A whole conversation, end to end, with its checks: a script of the person's turns (what they say,
the pause before, how long, voiced or not), what the fake provider's front does with each (answers
itself, or a holding line and a hand-off), and a fake agent that behaves like a real webhook agent
(it answers EVERY utterance seconds later with a chatty line in_reply_to that turn, sometimes twice).
`scenarios/front-run5.yaml` is the fifth live run (2026-11-27) as a script.

```sh
cd services/relay && pnpm install --frozen-lockfile && cd ../..
cd tools/sim && .venv/bin/molty-sim front --script scenarios/front-run5.yaml
# the real provider: one WAV per turn, in order; the key only in this shell's environment
.venv/bin/molty-sim front --script scenarios/front-run5.yaml --live --wav /tmp/t1.wav --wav /tmp/t2.wav --wav /tmp/t3.wav --wav /tmp/t4.wav
```

It prints one row per turn (heard, answered / handed off / NO REPLY, what Molty said, first audio,
hand-off) and the checks, and exits 1 when one fails: `one_reply` (exactly one front response per
turn), `agent_waiting` (an agent line is voiced only for a turn the front handed off), `never_twice`
(no agent line spoken twice), `not_over_them` (no response created while the person speaks; fake
provider only), `not_cut` (no end while a reply is made or the front waits for an answer that comes),
`first_audio` (the script's bound), `no_session` (no box session opened for a line that is only
context). With `--live` the front's behaviour is the real model's: the script's `front` / `said`
are ignored, the agent's lines and the checks are the same. The run directory keeps
`activity.json` and the logs (no key in them).

## Run it (from clean state)

```sh
cd services/relay && pnpm install --frozen-lockfile && cd ../..  # (1)
cp tools/sim/local.env.example tools/sim/local.env && $EDITOR tools/sim/local.env   # (2)
ssh "$MOLTY_SIM_BOX" 'docker ps --format "{{.Names}} {{.Status}}" | grep molty-voice'
make sim                                                     # (3)
make sim SIM_ARGS='--turn-end-hold 1.5 --only meters-from,i-think-that'        # (4)
make sim-test                                                # (5)
```

1. The local relay is this repo's `services/relay/` under `wrangler dev --local` (`VOICE_PROVIDER=none`: only the box speaks).
2. `local.env` (gitignored, read by `make sim`) names your box: `MOLTY_SIM_BOX` (ssh host), `MOLTY_SIM_ALIGN_PYTHON`
   (a python with torchaudio on it), `MOLTY_SIM_LLM_URL` / `MOLTY_SIM_LLM_MODEL` (for `--agent llm`). Kyutai
   STT/TTS must be up on the box; the sim only connects to them as a client.
3. Opens `ssh -N -L 18089:127.0.0.1:8089 -L 18090:127.0.0.1:8090 $MOLTY_SIM_BOX` (skipped when both ports
   already answer), starts `wrangler dev` on a free port, a sim device `molty-sim…`, pairs the agent
   and a local `molty-voice` (config in the run dir, deleted at the end), plays the default suite,
   force-aligns the received say audio on the box, prints the table. ~6 min for the default suite.
4. Bridge tuning goes straight to `molty-voice run`: `--turn-end`, `--turn-end-hold`, `--turn-end-skip`.
   Other knobs: `--agent llm --llm-model …`, `--agent-gap-ms N`, `--filler-ms N` (the sim device's
   auto filler setting: 0 = off, 400..5000, default 2500; -1 = not reported), `--align none`, `--no-audio`.
   A slow agent with and without Molty's own filler: `make sim SIM_ARGS='scenarios/slow-agent.yaml
   --agent-gap-ms 6000 --filler-ms 0 --align none'`, then the same with `--filler-ms 2500`; compare
   `silence` (`gap` stays ~6 s either way).
   `molty-sim check [scenario]` parses a scenario offline; `molty-sim show runs/<ts>` reprints a run.
5. Unit tests of the harness itself (scenario parsing, metrics, VAD port, mic framing, listen_stop,
   the turn-trace uplink: shake settle, resend ring, trace header fields, shake to talk; react moods / gestures,
   the drafts scenario and metrics).

**Script-lock suite** (`scenarios/script.yaml`): English turns the STT spelled in another script
(Persian / Chinese), each at a few sub-frame start offsets; the `script` column flags such a
transcript (`FOREIGN`) and the summary counts them. TTS voices never trigger it, so the suite plays
real kept turns from `scenarios/recordings/` (gitignored — a person's voice, never commit it; copy
the relay's kept turn WAVs there under the names in the file).

**Turn-trace suite** (`scenarios/trace.yaml`, `make sim SIM_ARGS=scenarios/trace.yaml`): `shake-noise`
(a turn started by an 8.2 g shake: seeded handling noise on the mic for the shake's first 0.45 s, an IMU
trace for the device's settle rule, the listen adopted by the relay), `shake-quick`, and `wifi-stall`
(the uplink fails 3 s mid-utterance: the sim device keeps the frames and resends them in order).
Scenario keys: `shake: {peak_g, after_s, noise_peak}`, `stall: {at_s, for_s}` (or a list).
`molty-sim measure` prints the same fixes' numbers offline (no box, no relay): what a shake listen sends
in its first 600 ms with / without the settle trim, a 9 s listen with a 3 s stall through the old uplink
vs the resend ring, and saturated samples of a loud clip at mic gain 8 vs 6.

Deployed relay (only on purpose): `--relay https://… --allow-remote` uses a separate, persisted
`molty-sim…` device (`state/`, 0600) and its own bridge pairing. The sim refuses any device id that
does not start with `molty-sim`, so it can never impersonate a real Molty.

## Metrics (per turn)

| column | meaning |
|---|---|
| `eos→end` | the human's last voiced sample → `listen_stop` reaching the device (mic clock); negative = cut off |
| `cut` / `lost` | turn ended while the clip still had > 150 ms of speech / how much |
| `WER` | relay transcript vs scenario text (case, punctuation, fillers ignored) |
| `stt` | relay `timings.listen.stt`: end frame → STT final |
| `gap` | human stops → the agent's answer at the speaker; `gap_parts_ms` splits it into eos→turn end, turn end→agent has the transcript, agent think, request→voice |
| `silence` | human stops → Molty's FIRST voice at the speaker: the relay's auto filler, a quick phrase or the answer (what the human hears as dead air); `filler` names a relay filler that came first |
| `say 1st` | agent's request → first say audio at the device (relay `first_audio` also kept) |
| `cue` | median (word cue − forced-aligned word onset) in the received say audio; + = text late |
| `dry` | say underruns at the speaker |
| `ack` | what the device's turn acknowledgement does at the end (chirp / thinking / off) |

Also in report.json: false/missed `no_speech`, time to `no_speech`, ack latency, chirp delay, cue
arrival lateness, relay timings. Cue ground truth is MMS forced alignment (torchaudio) run on the
box's CPU over ssh (`molty_sim/mms_remote.py`, `MOLTY_SIM_ALIGN_PYTHON`); the STT's word
`start_time` is not used for it — it lands ~0.25 s late with a word-length-dependent spread. The
alignment self-check aligns the human's clips too (their onsets are known from the TTS).

## Talk baseline: labelled turns, the facts agent, the scoreboard

Stage 0 of `docs/talk-proposal.html` (section 9): today's pipeline on `scenarios/talk-baseline.yaml`,
11 labelled turns in one conversation (small talk, a plain question, a mid-sentence pause, "um"
and "wait, actually the other branch", mumbling in noise, a 2.5 s pause, 12 s of silence, a
question nobody should guess, a long answer, a tap while Molty reads it, thanks).

- **Labels** (scenario keys): `label` `alone` (a talker may answer) | `hand_off` (needs the agent) |
  `refuse` (nobody guesses), `truth` (the fact id that answers it; `unknown` = the right answer is
  "I don't know"), `tap: {from_s, to_s}` (the human taps Molty during the say before this listen,
  at a seeded moment; tap is bound to `event` out of the box, so the say plays on). A top-level
  `facts` sheet (`id: {keys, say}`, with `unknown` and `idle`).
- **The facts agent** (`molty_sim/facts.py`): says only lines of the sheet (the fact whose keys
  match what Molty heard, the words after "wait / actually" first), thinks for a seeded time per
  turn (median about 4 s, 12 % about 12 s, 6 % never: it then stays quiet for 20 s and the human
  speaks again). It uses the skill's calls: one `talk` per turn; `--quick` plays `quick one_sec`
  before thinking; with `--first-words` it `keep`s the line up to its first clause end.
- **Scoreboard** (`molty_sim/score.py`, `board.txt` / `board.json` in the run dir): first sound
  p50 / p90 (the human's last voiced sample to Molty's first voiced sample of words, the chirp
  apart), the filler / quick / first-words time, grounded answer p50 / p90 (to the first voiced
  sample of the right fact; minus the agent's think time too), false cut-offs, talk-over ms, pause
  takeover rate, interruption recovery (quiet within 300 ms of the tap and the new question
  answered), answer fidelity (numbers and names of the fact in what Molty was sent), and per turn
  how many repeats passed. Agent-free turns, hand-off precision / recall and one voice need a
  talker: reported as not applicable today.
- **Judge** (`molty_sim/judge.py`, `--judge CMD`): any command that reads the prompt on stdin and
  prints JSON; the timed transcript, labels and sheet; a fixed rubric (invented facts, right
  answers, smooth 1-5, one voice 1-5); `--judge-votes` (3), the median. No key: a subscription
  CLI in print mode, e.g. `claude -p --model opus --no-session-persistence --tools '' --strict-mcp-config`
  (no temperature setting there), or a local model's CLI.
- **Repeatable**: the human's audio is cached as above; `--seed S --reps N` replays the
  conversation N times on one stack with seeds S..S+N-1 (think times, tap moments).
- **Arms**: box (default), `--first-words` (the sim bridge's `--opener`, Ollama at
  `MOLTY_SIM_OPENER_URL`, the owner's `first_words: opener`), `--quick`, and `--cloud` (Workers AI:
  the local relay gets a remote AI binding on the owner's Cloudflare account, `stack.CF_ACCOUNT_ENV`;
  nothing is deployed, no bridge). A model-played human (`{llm: ...}`) is still reserved, not built.

```sh
make sim SIM_ARGS="scenarios/talk-baseline.yaml --agent facts --reps 5 --seed 1 --align none --arm box \
  --judge \"claude -p --model opus --no-session-persistence --tools '' --strict-mcp-config\""
make sim SIM_ARGS="scenarios/talk-baseline.yaml --agent facts --reps 5 --seed 1 --align none --arm first-words --first-words"
```

## Shake turns: partials, drafts, reacts

`scenarios/drafts.yaml` (`shake: true`) plays the human **shaking** Molty and talking: the sim device
starts a listen at once under a local id, the relay adopts it (`cmd listen {adopt}`) and sends
`utterance.partial` chunks to the agent. A scripted **drafting agent** (its whole inbox as one stream:
a long-poll plus every ride-along) drafts one opener on the first partial that contains the turn's
`draft.when`, reacts on `react.when`, then answers: `follow` after a played opener (`draft.used`,
queued behind it), else `reply`. Each turn runs once per arm (`arms`, default `baseline` then
`drafts`, back to back on the same audio). `answer` names the opener family that fits the FULL
utterance, so a played opener of another family counts as wrong. Device moods are recorded:
`react:true` ones never cancel the turn's acknowledgement, like the firmware.

```sh
make sim SIM_ARGS='scenarios/drafts.yaml --align none'
```

| column / summary | meaning |
|---|---|
| `parts` / `1st part` | partials this turn / the first one's `elapsed_ms` |
| `gap` | the human's last voiced sample → Molty's first sample at the speaker (opener or reply) |
| gap saved | per turn, baseline gap − drafts gap (paired) |
| `draft` / `dropped` | the opener played (`draft.used`) / the relay's drop reason |
| `wrong` | a played opener whose family does not fit what the human finally said |
| saved / lost by relay | dropped openers that would have been wrong / would have fit |
| `seam` | the opener's last sample → the follow-up's first (it queues behind the opener) |
| `react lag` | the triggering word spoken → the react mood at the device (also request → device) |

The run ends with the relay's own calibration (`GET /v1/devices/<id>/speech`): turns sampled, the
partial policy and the draft threshold after the run.

## Reflex engines: `molty-sim reflex-bench`

Molty's reflex engines (docs/AGENT-LINK.md "Reflex") on 40 labelled partial sentences
(`scenarios/reflex.yaml`: a reaction and an utterance category each), the production code of
each engine, one sentence at a time:

```sh
make sim-env
cd tools/sim && .venv/bin/molty-sim reflex-bench --box <ssh host of your box> --misses
```

- `keywords`, `jev`, `openai`: the relay's `core/reflex.ts` through `services/relay/tools/reflex-bench.ts`
  (Node type stripping). `jev` needs `TYPESAFE_API_KEY`, `openai` `OPENAI_API_KEY` in the
  environment (skipped without; never printed).
- `local`: `molty_voice/reflex.py` on the box over ssh, in its own venv
  `~/molty-voice/reflex-lab/.venv` (`onnxruntime tokenizers huggingface_hub numpy`; the code is
  rsynced to `~/molty-voice/reflex-lab/src`) — never the running service.
- Columns: reaction accuracy after each engine's gate, false fires on the neutral sentences,
  category accuracy, p50 / p90 latency (remote: one call from this machine; local: the box's
  inference), mean input tokens, $ per 1000 partials (input tokens × list price).

## Reflex over whole turns: `molty-sim reflex-turns`

The reflex as the human sees it: each turn of `scenarios/reflex-turns.yaml` (8 to react to, 4 long
explanatory ones like Juan's, 6 neutral instructions) is spoken in real time, one partial per word
at 150 wpm, through the relay's own `ReflexSession` (`services/relay/tools/reflex-turns.ts`), with
the room's fire gate emulated; the listen closes 600 ms after the last word.

```sh
git archive main services/relay/src services/relay/tools | tar -x -C /tmp/main   # the code before, to compare
cd tools/sim && .venv/bin/molty-sim reflex-turns before=jev@/tmp/main/services/relay \
    calm=jev:calm normal=jev:normal lively=jev:lively --detail
```

Configs are `label=engine[:liveliness][@relay_dir]`. Columns: fires per react turn, react turns
with a reaction, the median time from the first word to the first reaction, decisions that
landed after the turn closed, fires per explain turn and its longest stretch with no reflex,
false fires on neutral turns, and engine decisions per turn (cost: ~$0.03 per 1000 Jev calls).

Before / after the liveliness change (Jev, 2026-10-31; `before` = main's code):

| config | react: fires/turn | react: turns with a reaction | first reaction (ms from first word) | after the turn closed | explain: fires/turn | explain: longest still (s) | neutral: false fires | decisions/turn |
|---|---|---|---|---|---|---|---|---|
| before (jev) | 0.9 | 7/8 | 2170 | 0 | 0.8 | 15.1 | 0 | 3.3 |
| calm (jev) | 1.1 | 7/8 | 988 | 0 | 0.5 | 15.4 | 0 | 3.8 |
| normal (jev) | 1.6 | 8/8 | 973 | 0 | 1.0 | 14.7 | 0 | 4.9 |
| lively (jev) | 2.0 | 8/8 | 964 | 0 | 2.5 | 12.0 | 0 | 6.1 |
| before (keywords) | 1.0 | 8/8 | 2000 | 0 | 1.2 | 13.8 | 0 | 3.3 |
| lively (keywords) | 2.1 | 8/8 | 800 | 0 | 2.2 | 12.9 | 0 | 6.1 |

The reflex alone cannot fill a calm explanation: Jev says `none` at 0.9+ on two of the four
explain turns at every chunk, and lowering its gates further fires on the neutral ones. The
device's attentive baseline (`listen_react.h`, host-tested) covers that: a micro-motion at most
~2.9 s apart while a voice is heard (lively idle life), which the sim does not render.

On the labelled phrases (`reflex-bench --liveliness …`, Jev): before 38/40, calm 38/40, normal
40/40, lively 40/40, no false fires on the 6 neutral phrases at any liveliness.

## Caveats

- The local relay runs on this Mac, not Cloudflare's edge, and the bridge reaches Kyutai through an
  ssh tunnel: absolute latencies differ a little from production (box → Cloudflare → device). Use
  the sim for A/B comparisons of tuning; confirm wins on the device.
- The sim mic is the clip's audio, not the device's mic (no AEC, no gain chain, no room acoustics).
