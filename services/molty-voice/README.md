# molty-voice — your own voice box for Molty

Run Molty's speech on your own GPU instead of the cloud: Kyutai's
[moshi-server](https://github.com/kyutai-labs/delayed-streams-modeling) does STT and TTS,
and `molty-voice` bridges it to the Molty relay.

```
Molty ──WSS──▶ relay ◀──WSS── molty-voice ──ws://127.0.0.1──▶ moshi-server (STT :8090, TTS :8089)
                 ▲                (your GPU box: dials out,
              agents               no inbound ports, no tunnel)
```

- Molty doesn't change: it talks to the relay as before.
- The box is one more relay client. It pairs like an agent (same tap on Molty) with the
  role `voice`. Its token opens only the voice socket and is refused on every agent call.
- Speech-to-text streams while you talk, so the transcript arrives about 0.1–0.5 s after
  you stop speaking instead of after a 3–8 s batch upload.
- If the box goes offline, the relay falls back to its cloud voice (mode `auto`, the
  default). The modes are set on the agent page of Molty's console:
  - `auto`: the box when it's connected, else the cloud
  - `box`: the box only
  - `cloud`: the cloud only

Wire protocol: `docs/AGENT-LINK.md`, section "Voice box".

## Deploy

You need Docker with Compose, an NVIDIA GPU with its container toolkit, and the Molty relay's
address. On Molty's console, the **agent** page shows the device id and the current link code; the
code rotates every 15 minutes. Then, from a checkout of this repo:

```sh
cd services/molty-voice
docker compose build
docker compose run --rm --no-deps bridge pair --relay https://<your relay> --device molty-xxxxxxxxxx --link-code K7M2W9C4
docker compose up -d stt tts
docker compose run --rm --no-deps bridge check   # once STT and TTS are up; repeat until it prints ok
docker compose up -d
```

Molty shows a 4-digit code during `pair`: tap it to accept. `check` speaks a line through TTS and
hears it back through STT. The first start compiles Kyutai's server and downloads its models, so
expect 15 minutes or more; follow it with `docker compose logs -f tts`. Later starts take seconds.

Everything is kept in `./data`, including the pairing token (`config/config.json`, mode 0600).

| | |
|---|---|
| GPU memory | 9.6 GB for STT + TTS; first words add 3.9 GB (measured on an RTX 4090) |
| Disk | about 40 GB: server image 11, build caches 20, models 6 |
| Smaller GPU | lower `batch_size` in `moshi/stt.toml` (fewer simultaneous listens); not measured |
| Hugging Face | no account needed. A busy IP can get `429 Too Many Requests` on the voices: the container restarts and resumes. To avoid it, put `HUGGING_FACE_HUB_TOKEN=...` (a read token) in `.env` |

Update with `git pull && docker compose up -d --build`.

Without Docker, run your own moshi-server (STT at `ws://127.0.0.1:8090/api/asr-streaming`, TTS at
`ws://127.0.0.1:8089/api/tts_streaming`, header `kyutai-api-key: public_token`; configs in `moshi/`)
and install the bridge with Python ≥ 3.12: `uv tool install './services/molty-voice[reflex]'`.
Then use `molty-voice pair`, `check` and `run` directly; `examples/molty-voice.service` keeps it running.

## Pairing

The console names the request after `--name` (default `voice box`). The token is stored with
mode 0600 and never printed.

Pairing a new box replaces the old one: the relay keeps one voice box per device.

To unpair, use either:

- `molty-voice unpair`, or
- **unpair** on the console's Voice card.

## Run

```sh
molty-voice run                      # reconnects on its own (backoff 1 s … 60 s)
molty-voice run --default-voice expresso/ex03-ex01_calm_001_channel2_1081s.wav \
                --stt-url ws://127.0.0.1:8090/api/asr-streaming --tts-url ws://127.0.0.1:8089/api/tts_streaming
```

Flags override the config file (`--name`, `--voices a.wav,b.wav`, `--no-stt`, `--no-tts`,
`--api-key`, `--reflex`).

**First words** are optional: install [Ollama](https://ollama.com) on the box and run
`ollama pull gemma3:4b`. The compose bridge runs with `--opener`; elsewhere pass it, or set
`"opener": true` in the config (`opener_url`, `opener_model` too). It offers the relay a small model that
starts Molty's reply before the agent can speak. The bridge loads it on connect
and keeps it loaded (about 3.9 GB; about 150 ms a line). No model: a warning, and no first words. The owner
turns it on in the console; the relay's guard decides what may be said.

**Molty's reflex** (docs/AGENT-LINK.md "Reflex"): `--reflex` (or `"reflex": true` in the config)
loads a small emotion classifier — `SamLowe/roberta-base-go_emotions-onnx`, int8 ONNX, ~125 MB,
on the CPU (~35 ms a phrase with 4 threads; nothing on the GPU next to Kyutai) — and offers it
to the relay. The compose bridge has it on; a host install needs the `reflex` extra. The
model downloads from Hugging Face on the first run. Without the extras the box logs a warning
and offers no reflex. The owner picks the engine in the console (box = this classifier). Exit codes:

- `2`: not paired.
- `3`: the relay refused the token (unpaired or replaced). Run `pair` again.

Voices are paths in Kyutai's `kyutai/tts-voices` repository. The default is
`expresso/ex03-ex01_happy_001_channel1_334s.wav`. An agent's `voice` argument passes
through when it looks like such a path; otherwise the box uses its default (or the
default the owner picked in the console).

## How it maps to Kyutai

- **STT:** One moshi-server connection per listen. Molty's 16 kHz PCM16 chunks are
  resampled to 24 kHz float as they arrive and sent in 80 ms frames.
  - At the end of the listen the box sends a `Marker`, then silence as fast as the
    server takes it. That pushes the marker through the model's delay.
  - The words seen before the marker comes back are the transcript.
  - Every session closes its socket at once: the server has a fixed number of channels.
  - A socket that closes before the marker (e.g. "no free channels") is reported as an
    error. So is a malformed server message. The relay then falls back in `auto` mode;
    it is never an empty transcript.
  - Model: built and measured for `kyutai/stt-1b-en_fr`, which needs no leading silence.
    Kyutai's reference client sends 1 s of silence first for the 2.6B model. That is not
    done here; add it before switching to that model.
- **End of turn:** While you talk, the STT's per-step pause estimate (`Step.prs[2]`) is
  smoothed exactly as Kyutai Unmute does it. Once you said a word and it passes 0.6
  (`--turn-end`, 0 = off), the box sends `stt.turn_end` — at once when the latest word ends a
  sentence (`.` `?` `!`); mid-sentence ("…2 meters from", "it,") only once the pause has lasted
  `--turn-end-hold` seconds (default 1.2, 0 = at once; a word landing meanwhile restarts it). The relay then stops Molty's mic
  (firmware with the `listen_stop` cap) and finishes the transcript at once, with reason
  `turn_end`, instead of waiting for Molty's silence timer or the listen window. Silence
  alone never ends a turn that way. With the box serving the listen, this is THE end: the
  relay turns Molty's own silence VAD off and passes `no_speech_ms` in `stt.start` (8 s unless
  the agent set one), and when nobody spoke for that much captured audio the box sends
  `stt.turn_end` with `reason: "no_speech"` once: the relay ends the listen with an empty
  transcript. "Nobody spoke" is voice activity, not words (words lag the voice by 1–1.6 s):
  the model's `prs[2]` under 0.3 for 3 steps restarts the clock, and a loud frame in the last
  ~0.5 s postpones the verdict. `--turn-end-skip` sets how many 80 ms steps at a listen's start
  the end-of-turn rule ignores (default 12, as Unmute).
- **Turn trace and hints** (`molty_voice/trace.py`): right before each `stt.turn_end` and before
  `stt.final` the box sends `stt.trace` — the model's speech-activity spans, the pause estimate
  (downsampled), every word with its start/stop time, the end-of-turn decision (when, which rule:
  `punct` / `hold` / `unmute`, the word judged, the hold that elapsed) or the no-speech decision,
  and the final's latency; ≤ 3 KB. The relay stores it with the turn (console › activity). While
  you talk it sends `stt.hint` `pause` (a mid-sentence pause begins) and `question` (a word ends
  in `?`), at most one per 1.5 s of audio: the relay passes them to Molty, which nods / glances
  as a listener. Older relays ignore both.
- **Stream head / language anchor** (`molty_voice/kyutai.py` `stream_head`): every STT stream
  opens with Kyutai's 1 s digital-silence prefix, then an English audio prompt (`stt_prompt` in
  the config, default "Okay, so here is what I think.", spoken once by the box's own TTS in the
  default voice and checked once through the STT — a take the STT does not hear as exactly that
  line is dropped and made again later) and 0.8 s of silence, then a marker; every word before
  that marker comes back is the prompt's and is dropped. Without it stt-1b-en_fr sometimes locks
  its transcript into another script on the first words (English heard as Persian or Chinese
  spelling: about a third of the start offsets for the turns affected). `"stt_prompt": ""` turns
  the prompt off (the prefix stays). Cost: ~5 s of head audio, ~0.4 s of model time at the start
  of each listen, caught up within the first second.
- **TTS:** One connection per request (the relay groups sentences up to ~220 characters for the
  box; connecting costs ~1 ms, so there is no pool). The box sends the words, then `Eos`. The
  24 kHz float audio streams back resampled to 16 kHz PCM16, in ≤ 16 KB frames. The quiet lead
  of the first frame (≤ 70 ms before the voice) is dropped (`trim_lead`), and word timings
  (`tts.word`) are Kyutai's `start_s` less one 80 ms step and less that trim.

## Tests

```sh
cd services/molty-voice && uv run --with pytest --with . pytest -q
```

The tests cover:

- framing and resampling;
- a bridge test against a fake relay and fake STT/TTS servers;
- the error paths: no free channel, cancel, refused token.
