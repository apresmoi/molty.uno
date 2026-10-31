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
  pipeline's own), or `--agent llm` (any OpenAI-compatible endpoint, default Ollama on the box).
- **Metrics** per turn and summarised (`runs/<ts>/report.{md,json}`, plus every WAV).

Why Python: it reuses `services/molty-voice/` directly (Kyutai TTS/STT clients, resampler, framing) and runs the
second molty-voice bridge from the same venv; the relay side needs no hook (a stock `wrangler dev`).

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
   Other knobs: `--agent llm --llm-model …`, `--agent-gap-ms N`, `--align none`, `--no-audio`.
   `molty-sim check [scenario]` parses a scenario offline; `molty-sim show runs/<ts>` reprints a run.
5. Unit tests of the harness itself (scenario parsing, metrics, VAD port, mic framing, listen_stop,
   the turn-trace uplink: shake settle, resend ring, trace header fields, shake to talk; react moods / gestures,
   the drafts scenario and metrics).

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
| `gap` | human stops → Molty's next voice at the speaker; `gap_parts_ms` splits it into eos→turn end, turn end→agent has the transcript, agent think, request→voice |
| `say 1st` | agent's request → first say audio at the device (relay `first_audio` also kept) |
| `cue` | median (word cue − forced-aligned word onset) in the received say audio; + = text late |
| `dry` | say underruns at the speaker |
| `ack` | what the device's turn acknowledgement does at the end (chirp / thinking / off) |

Also in report.json: false/missed `no_speech`, time to `no_speech`, ack latency, chirp delay, cue
arrival lateness, relay timings. Cue ground truth is MMS forced alignment (torchaudio) run on the
box's CPU over ssh (`molty_sim/mms_remote.py`, `MOLTY_SIM_ALIGN_PYTHON`); the STT's word
`start_time` is not used for it — it lands ~0.25 s late with a word-length-dependent spread. The
alignment self-check aligns the human's clips too (their onsets are known from the TTS).

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

## Caveats

- The local relay runs on this Mac, not Cloudflare's edge, and the bridge reaches Kyutai through an
  ssh tunnel: absolute latencies differ a little from production (box → Cloudflare → device). Use
  the sim for A/B comparisons of tuning; confirm wins on the device.
- The sim mic is the clip's audio, not the device's mic (no AEC, no gain chain, no room acoustics).
