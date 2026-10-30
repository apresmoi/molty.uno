# molty CLI and MCP server

`molty` is a Go CLI and stdio MCP server for the device (source: `cli/`). Both
adapters call `cli/internal/molty`; `molty.Transport` selects direct HTTP or relay
REST; `cli/internal/device` provides the direct HTTP request and IMU WebSocket plumbing.

## Build and test

Go 1.25 or newer.

```sh
cd cli
make build        # -> bin/molty
make test         # go build ./... && go test ./...
./bin/molty --help
```

The tests run offline against in-process fakes (`internal/testdevice`, a fake relay).

> **Known gap — direct-HTTP moods.** In direct HTTP mode, `mood`, `say` and `listen`
> (which set a mood around audio) and `show` still use the mapping below, which
> switches to the retired `claude` / `eye` / `reader` creatures. Current firmware
> only has the `fluid` creature, so those verbs fail in direct mode. **Relay mode is
> unaffected**: there the device maps moods onto the fluid itself
> (`{"state":"creature","mood":…}`, see [AGENT-LINK.md](AGENT-LINK.md)). Porting the
> direct mapping to the same fluid patch is the fix.

## Device selection and output

Selection order is `--device URL`, then `MOLTY_URL`, then a read-only
`/api/info` probe of `http://10.13.37.1` followed by `http://molty.local`.
An explicit or environment URL is used directly, without fallback. The selected
address is pinned for the process; failed mutations are never replayed.
Only HTTP(S) origins are accepted, without credentials, query or path prefixes.

Options can appear before or after the subcommand. `--` ends option parsing
when text begins with a dash. Normal CLI output is indented JSON; `--json`
produces compact JSON, including errors on stdout, and a nonzero exit status.
Without `--json`, errors go to stderr. Events always produce JSON lines.

| CLI | MCP tool | Result / effect |
|---|---|---|
| `status` | `molty_status` | `reachable`, selected `device`, nested `info`, `wifi`, `creature`, `imu`; preserves partial results and reports endpoint errors |
| `creature [name]` | `molty_creature` | List current/available creatures, or switch |
| `mood <mood>` | `molty_mood` | Switch and patch the visual mood below |
| `show [text] [--wpm 300]` | `molty_show` | Switch to reader, upload UTF-8 text, restart and play; returns immediately after starting |
| `say <text>` or `say --wav PATH` | `molty_say` | Synthesize or read a local WAV, show speaking mood, play synchronously, restore prior visuals |
| `listen [--ms 4000] [--out PATH.wav] [--force]` | `molty_listen` | Capture, restore visuals, save WAV, report `path`, `duration_ms`, `peak`, `rms`, optional `text` |
| `volume <0-21>` | `molty_volume` | Set speaker volume |
| `imu` | `molty_imu` | One acceleration/gyro snapshot with device timestamp |
| `events [--n N] [--exec CMD]` | — | Direct: IMU WebSocket JSON lines. Relay: your Molty events, see [below](#events-and-receiving). Default 0 streams until interrupted |
| `mcp` | — | Serve JSON-RPC on stdin/stdout; diagnostics use stderr |

`show` reads stdin if text is omitted or is `-`. Text must be nonempty UTF-8,
without NUL, at most 16 KB. WPM is 100–800; ramp endpoints are set to the same
WPM and ramps disabled so stale reader settings cannot alter the requested pace.

MCP arguments use `name`, `mood`, `text`, `wpm`, `wav`, `ms`, and `volume`
as applicable. MCP listen always creates a fresh private temporary WAV; it accepts
no `out` or `force` argument. CLI `--out` requires a `.wav` suffix and refuses an
existing file or symlink unless `--force` is supplied. Defaults otherwise match
the CLI. Unknown arguments are rejected. Results
include both JSON text content and `structuredContent`; operational errors set
`isError: true`, `ok: false`, and `error`. Tool descriptions explain limits and
physical effects without requiring this document.

## Mood mapping

Direct HTTP mode only (see the known gap above). `MoodMapper.Map` returns a
`MoodPlan` (optional creature name plus state patch). `MappedMoods` is the default;
a future mapper can leave the creature empty and return `{"mood":"happy"}`.
Callers and both adapters stay unchanged.

| Mood | Creature | State patch specific to this mood |
|---|---|---|
| idle | claude | `pulse_amp: 0.04`, `pulse_period_s: 3`, `body_color: #d49070` |
| listening | claude | `pulse_amp: 0.06`, `pulse_period_s: 1.5`, `body_color: #60b8ff` |
| thinking | claude | `pulse_amp: 0.10`, `pulse_period_s: 0.8`, `body_color: #b090ef` |
| speaking | claude | `pulse_amp: 0.14`, `pulse_period_s: 0.5`, `body_color: #70d8a0` |
| happy | claude | `pulse_amp: 0.12`, `pulse_period_s: 1`, `body_color: #ffce60` |
| sleepy | eye | `pupil_size: 0.20`, `blink: 0.75`, `autoblink: false`, iris inner/outer `#8090c0` / `#303850` |
| surprised | eye | `pupil_size: 0.75`, `blink: 0`, `autoblink: true`, iris inner/outer `#ffe090` / `#b07030` |

Every claude patch also sets `scale: 0.7`, `dynamics: true`,
`bg_color: #101018`, `eye_color: #101018`. Both eye patches set `x: 0`, `y: 0`,
`dynamics: false`; colors use the nested `iris.inner_color` / `iris.outer_color`
fields. These fields and limits were checked against the firmware setters.
The emotional interpretation and visual quality of this palette remain design
choices awaiting physical verification.

Audio snapshots the actual active creature before changing mood. For eye, claude,
and reader it also snapshots writable visual state. Glyph, ui, and world restore
only the creature switch: glyph/ui GET state cannot be POSTed back, and reposting
world's scene reloads it and resets animation. This works across separate CLI
invocations. If a required snapshot fails, audio is not started.

Restoration runs even after an audio error or cancellation, with a separate
cleanup deadline of remaining clip duration plus 10 seconds. Both restore requests
share that deadline; the usual five-second JSON request limit does not shorten it.
Restore failures appear as `restore_warning`; they never turn successful audio
into a verb error. A valid captured recording is still saved and reported.
Firmware runtime clocks, animation phase and read-only counters cannot be rewound.
External clients changing state during an operation can race restoration.

## Speech backends and audio

`TTS.Synthesize` and `STT.Transcribe` are replaceable Go interfaces. Default TTS
uses the installed macOS `say` executable with text on stdin:
`-o <temporary.wav> --data-format=LEI16@16000 --file-format=WAVE`.
No voice/model downloads are performed. On other platforms configure
`--tts-cmd` or supply `--wav`.

`--tts-cmd COMMAND` runs a trusted operator-configured `/bin/sh -c` command:
text stdin → WAV stdout. `--stt-cmd COMMAND` (or `MOLTY_STT_CMD`) receives WAV
stdin → transcription stdout. STT stdout is trimmed and returned as `text`.
There is no bundled STT backend. Configure commands when starting MCP, never
through tool arguments. Text and WAV data are piped, never interpolated into
shell command strings. Commands have a two-minute deadline, bounded output,
and a two-second subprocess pipe-drain limit. Operators own any child processes
spawned by their configured shell commands.

WAV input/output must be nonempty RIFF PCM, 16 kHz, signed int16, mono, at most
4 MB. Other rates/formats are rejected; an external TTS command must produce the
required format. The CLI validates chunks and removes metadata before
uploading, because the firmware's small parser treats trailing bytes as audio.
Playback waits for completion with a timeout of clip duration plus 10 seconds.
Recording accepts 1–10000 ms at 16000 Hz, with duration plus 10 seconds allowed.
Cancellation applies during request upload. Once the WAV upload (or record request)
is sent, the CLI drains the synchronous response under `context.WithoutCancel`
with that bounded deadline. Firmware keeps playing/recording after a disconnect;
on response loss, restoration gets the remaining clip time plus 10 seconds to
queue behind it while the whole-verb lock remains held.

Volume is sent as a bare integer in a `text/plain` body. Firmware's quoted-string
JSON parser would silently interpret a numeric JSON object as zero.
`mic_busy`, `wrong_creature`, and other firmware `ok:false` JSON responses are
errors even when the HTTP status is 200.

Recordings use private files (0600), defaulting to the OS temp directory. An
explicit output path requires a `.wav` suffix and is published atomically without
replacing an existing file, including one created during capture. `--force` permits
atomic replacement. MCP exposes no destination path argument. Peak and RMS are normalized against
32768; duration is computed from actual PCM samples. A failed STT command leaves
the recording intact and includes its path in the error/result. Delete temporary
recordings when no longer needed.

A shared mutex serializes complete verbs and a transport mutex covers each
request through response-body consumption. Long audio blocks every other
request from the same client. Events reserve their client's device connection
until N frames or cancellation, with a 10-second idle-frame timeout. Locks are
per process/client: run one CLI/MCP owner per device at a time.

OTA, reboot, forget, credentials, HTML upload/reset and coredump erase are absent
from the CLI and MCP tools.

## MCP configuration

```sh
cd cli && make build
claude mcp add molty -- "$PWD/bin/molty" mcp
```

Generic configuration (WAV paths refer to the server machine):

```json
{
  "mcpServers": {
    "molty": {
      "command": "/absolute/path/to/molty/cli/bin/molty",
      "args": ["mcp"],
      "env": {"MOLTY_URL": "http://10.13.37.1"}
    }
  }
}
```

Add `--tts-cmd` / `--stt-cmd` to `args` when needed. stdout is reserved for
newline-delimited JSON-RPC. The server supports initialization/version
negotiation (2025-06-18 and 2025-11-25), ping, tools/list, tools/call,
notifications/initialized and notifications/cancelled. Device operations serialize;
tool execution is asynchronous so cancellation and ping remain responsive.
Input messages are capped at 256 KB, with at most 64 pending tool calls.
On stdin EOF the server waits up to 150 seconds for accepted calls and restoration,
then reports a drain timeout if they have not completed; EOF does not cancel them
immediately. Clients must keep reading stdout during this drain.

The MCP server is a small stdlib JSON-RPC implementation (no MCP SDK dependency);
`github.com/coder/websocket` is used only for IMU events.

## Relay mode

CLI and MCP share `Transport`: `HTTPTransport` keeps the direct HTTP behavior,
local TTS/STT, WAV handling and restoration; `RelayTransport` sends authenticated
REST commands and runs no local speech backend. No new dependencies are needed.

Connection fields resolve independently: `--relay/--device/--token` →
`MOLTY_RELAY/MOLTY_DEVICE/MOLTY_TOKEN` → `~/.config/molty/link.json`.
The file contains `relay`, `device_id`, and `agent_token`. Without a relay,
selection falls back to the direct HTTP rules above. An explicit `--device
http://...` or `https://...` selects direct HTTP even when relay settings exist.
Invalid saved JSON is reported instead of silently probing a device.

`molty pair` uses `--relay`, else `MOLTY_RELAY`, else the default relay
(`DefaultRelay` in `cli/internal/molty/link.go`, see AGENT-LINK.md "Default relay").
Replace the device ID and link code with the ones the console shows:

```sh
cd cli && make build
./bin/molty pair --device molty-xxxxxxxxxx --code ABCDEFG2 --name 'My agent'
./bin/molty --json status
./bin/molty --timeout 30s --json say 'Hello from the relay'
./bin/molty --timeout 30s --json listen --ms 4000
./bin/molty mcp
```

- The 4401 code is defined for device WSS closes, not agent REST. Numeric/string
  JSON code/error 4401 is recognized defensively; other non-2xx bodies are included
  in the error. `relay/tools/e2e.mjs --go-cli cli/bin/molty` runs this CLI against
  a local relay and fake device.

Offline fake-relay tests cover pairing approval/rejection/cancellation, private
credential replacement, config precedence, command names/args/auth, job polling,
deadlines, async MCP submission and wait, redirects and auth errors.

## Events and receiving

In relay mode `molty events` follows this agent's event inbox on the relay
(`GET /v1/devices/<id>/events`, long-poll ≤ 25 s; [AGENT-LINK.md](AGENT-LINK.md#events-device--agents))
and prints one compact JSON line per event: `{"data":{…},"id":"e42","ts":<ms>,"type":"shake|tap|button"}`.
It is how a shell-capable agent hears Molty: run it in the background and treat each line as
a wake-up.

```sh
molty events                                    # until interrupted
molty events --n 1                              # block until the next event, print it, exit 0
molty events --exec 'jq -r .type >> ~/molty.log' # also run a command per event
```

- Start: resumes at the relay's cursor for this token, so a restart neither replays read
  events nor loses unread ones (within the relay's 200 events / 24 h). After that it passes
  the last `next` as `since`, so a response lost in transit is read again.
- `--exec CMD` runs `sh -c CMD` (Windows: `cmd /C`) once per event, in order, after the line
  is printed: event JSON on stdin, `MOLTY_EVENT_ID` and `MOLTY_EVENT_TYPE` in the
  environment, its output on stderr (stdout stays pure JSON lines). A failing command is
  reported on stderr and the stream continues; a slow one delays later events (they wait
  on the relay).
- Network errors, 5xx, 408 and 429 reconnect with backoff (1 s doubling to 30 s, notice on
  stderr). A rejected token (401/403) or another 4xx exits **1** (with `--json`, the error
  is a JSON object on stdout): re-pair instead of looping.
- Direct HTTP mode keeps the old behaviour (IMU frames from the device's WebSocket).

Examples of running it as an agent (not prescriptions):

- Claude Code: start `molty events` through the Bash tool with `run_in_background`, then
  watch its output with the Monitor tool; each line is one event.
- Codex CLI, local OpenClaw or any long-lived agent: run
  `molty events --exec '<command that wakes the agent>'` as a child process or a
  `systemd --user` / `launchd` service.

## Live check against a device

Speak near the mic during capture, listen to both playback paths, and inspect each
displayed mood. The STT command below is a pipeline check, not a transcription model.
Direct-HTTP mood verbs are affected by the known gap above.

```sh
cd cli && make build
export MOLTY_URL=http://10.13.37.1
./bin/molty --json status
./bin/molty --device http://molty.local --json status
./bin/molty --json creature
for mood in idle listening thinking speaking happy sleepy surprised; do
  ./bin/molty --json mood "$mood"
  sleep 1
done
printf 'The Molty CLI reads this at three hundred words per minute.\n' | ./bin/molty --json show
./bin/molty --json volume 10
./bin/molty --json mood happy
./bin/molty --json say 'Molty speech test. I should return to the happy mood.'
./bin/molty --json listen --ms 4000 --out /tmp/molty-live.wav --force
./bin/molty --json say --wav /tmp/molty-live.wav
./bin/molty --json listen --ms 1000 --out /tmp/molty-stt-live.wav --force --stt-cmd 'cat >/dev/null; printf "pipeline-ok\n"'
./bin/molty --json imu
./bin/molty events --n 10
python3 - <<'PY'
import json, subprocess
p = subprocess.Popen(['./bin/molty', 'mcp'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
def request(i, method, params):
    p.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': i, 'method': method, 'params': params}) + '\n')
    p.stdin.flush()
    response = json.loads(p.stdout.readline())
    assert response['id'] == i and 'error' not in response, response
    print(json.dumps(response))
    return response['result']
try:
    request(1, 'initialize', {'protocolVersion': '2025-11-25', 'capabilities': {}, 'clientInfo': {'name': 'live-check', 'version': '1'}})
    p.stdin.write(json.dumps({'jsonrpc': '2.0', 'method': 'notifications/initialized'}) + '\n')
    p.stdin.flush()
    assert len(request(2, 'tools/list', {})['tools']) == 11
    result = request(3, 'tools/call', {'name': 'molty_status', 'arguments': {}})
    assert not result['isError'] and result['structuredContent']['reachable'], result
finally:
    p.stdin.close()
    p.wait(timeout=20)
assert p.returncode == 0, p.returncode
print('MCP LIVE CHECK PASSED')
PY
```

Verify positive side effects: reader advances at the requested speed; all seven
moods appear; speech completes and restores happy; the recordings contain audible
sound; WAV playback matches capture; peak/RMS are nonzero when speaking; STT
returns `pipeline-ok`; events emit ten distinct timestamped frames; MCP reports
eleven tools and a reachable device. USB/Wi-Fi availability is a live dependency.

## Test notes

Tests use `httptest.Server` with a `net.Pipe` listener (no TCP binds needed).
Requests still traverse the actual HTTP client, server and WebSocket upgrade;
unknown test hosts fail closed. Tests cover selection/fallback, response-body and
whole-verb serialization, mood mapping, default `say` argv via a fake executable,
fake command TTS/STT pipelines, WAV validation/levels, audio deadlines,
restoration/errors/cancellation, atomic output-file protection (including racing
writers), CLI `--force`, and MCP initialization/tools/errors/cancellation/EOF drain.
Physical behaviour, native macOS speech synthesis, real STT quality and external MCP
client interoperability are live checks.
