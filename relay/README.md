# molty-relay

The cloud relay for Molty's agent link (spec: [`docs/AGENT-LINK.md`](../docs/AGENT-LINK.md)).
Molty dials **out** over WSS. Agents reach it over remote MCP or REST, and the
relay does TTS/STT. No laptop is in the loop.

```
Molty ──WSS /v1/device──▶ Worker ──▶ Durable Object "MoltyDevice" (one per device_id)
                            │            └─ DeviceRoom (src/core): TOFU, pairing, tokens, cmd routing, jobs
                            ├─ /mcp  ◀── any MCP client (Claude, Grok, …)   (src/surfaces/mcp.ts)
                            ├─ /v1/* ◀── molty CLI --relay, scripts         (src/surfaces/rest.ts)
                            └─ voice: workers-ai | openai | none             (src/voice/*)
```

## Layout

```
relay/
├─ src/
│  ├─ core/            pure TS, no Cloudflare imports (enforced by test/boundaries.test.ts)
│  │  ├─ room.ts       DeviceRoom: device auth, pairing, tokens, routing, say/listen jobs
│  │  ├─ api.ts        AgentApi / RelayBackend: the only thing surfaces see
│  │  ├─ commands.ts   agent command validation, shared by every surface
│  │  ├─ protocol.ts   wire constants, text/audio frame codecs
│  │  ├─ audio.ts      WAV parse/encode, mono, resample, PCM16, levels
│  │  ├─ crypto.ts     Web Crypto helpers, codes, base64
│  │  ├─ store.ts      KV + Clock seams (+ in-memory versions)
│  │  └─ waitlist.ts   waitlist validation, WaitlistStore seam, rate-limit rule
│  ├─ surfaces/        Fetch-API only: mcp.ts (Streamable HTTP), rest.ts, waitlist.ts
│  ├─ voice/           VoiceProvider + workers-ai.ts, openai.ts, none.ts, index.ts (selection)
│  ├─ device-object.ts Cloudflare host: DO + hibernatable WebSockets + storage adapter
│  ├─ waitlist-host.ts Cloudflare host: D1 WaitlistStore + Turnstile check
│  └─ worker.ts        Cloudflare host: routing + DO-backed RelayBackend
├─ migrations/         D1 schema for the waitlist (wrangler d1 migrations)
├─ test/               vitest unit tests
└─ tools/              fake-device.mjs, agent-e2e.mjs, e2e.mjs (local end-to-end)
```

To swap the host, write another `RelayBackend` and `SocketHost` and reuse
`src/core` and `src/surfaces` as they are. A Node server would need about 100
lines: a `ws` server feeding `DeviceRoom.socketMessage`, plus `MemoryKV` or
SQLite. To swap the voice provider, implement `VoiceProvider` and add one case to
`src/voice/index.ts`.

## Design notes

- **Pattern: one Durable Object per device with hibernatable sockets.** That means:
  - a Durable Object with hibernatable sockets;
  - the rule that admission lives in the socket's serialized attachment, never in an instance field;
  - raw-socket tests against `wrangler dev --local`;
  - a static test that guards the hibernation rule.

  Its rooms semantics (two opaque peers) are **not** reused. Here the relay understands the protocol.
- **No `partyserver`.** Its `/parties/<party>/<room>` routing doesn't match
  `/v1/device?id=`. The native DO hibernation API is about 40 lines and has no dependencies.
- **Pings never wake the object.** `ctx.setWebSocketAutoResponse` answers the device's
  exact `{"t":"ping"}` text with `{"t":"pong"}`. Any other ping formatting still gets a pong from the room.
- **In-flight work is in memory.** That covers pending commands, pairing waiters, the say
  stream and the listen buffer. All of it runs only while a request, timer or frame
  keeps the object awake. If the object is evicted anyway, a job that was left
  `running` reports `failed: "interrupted"` and never hangs.
- **Remote MCP is a minimal JSON-RPC implementation, not `@modelcontextprotocol/sdk`.**
  - What it supports: Streamable HTTP (POST `/mcp`, protocol versions 2025-03-26, 2025-06-18 and 2025-11-25), stateless, tools only, JSON responses, no SSE.
  - Why not the SDK on the server:
    - every tool returns within about 8 s by design, so there is nothing to stream;
    - stateless mode means no session store in the DO;
    - it matches the local Go stdio server, which made the same choice, so errors and argument validation behave the same;
    - there are zero runtime dependencies on Workers.
  - Compliance is checked by driving it with the **official SDK client** (`Client` + `StreamableHTTPClientTransport`) in the e2e.
- **Token format.** `agent_token` is `<device_id>.<base64url(32 random bytes)>`. The
  public prefix lets the stateless `/mcp` endpoint route to the right DO without a global
  index. Only `sha256(token)` is stored.

## Protocol details the spec left open (as implemented)

| Topic | Implemented as |
|---|---|
| Bad first frame (not `hello`, malformed) | close **4400**. Wrong id or key: **4401**. Replaced by a newer device connection: **4409**. After `forget`: **4410**. |
| Hello timeout | 10 s. At most 4 unauthenticated sockets are kept waiting. |
| Link-code updates | `hello.link_code`, `{"t":"link_code","link_code":"…"}`, `evt link_code {link_code}`, or `pair.result.link_code`. The used code dies on approval even before a new one arrives. |
| Pair result | `{"t":"pair.result","req","ok", "reason"?:"timeout", "link_code"?}`. `reason:"timeout"` maps to the agent error `timeout`; any other rejection maps to `rejected`. |
| Console "forget link" | Device sends `{"t":"forget"}` on its authenticated socket. The relay wipes the key hash, tokens, link code and jobs, replies `{"t":"forgotten"}`, and closes 4410. The next hello claims afresh (TOFU). |
| `say` to device | `cmd say {job, text, ms, rate:16000, voice?}`. After `res ok`, binary frames follow: `s=job`, 320 ms (10 240 B) payloads, the first 3 frames ahead of real time and the rest paced, `end:true` on the last. The job is `done {ms, text}` at the stream's playback end, or earlier on `evt say.done {job}`. |
| `listen` to device | `cmd listen {job, ms, rate:16000}`. The device streams `s=job` frames and ends with `end:true`. Grace is `ms + 5 s`. The result is `{text, duration_ms, peak, rms}`; with no frames it fails `no_audio`. |
| `state` | Empty args read the state. `{state:{…}}` or flat fields patch it. The device always receives the patch **flat**, like the body of `/api/creature/state`. |
| Concurrency | One audio job (say or listen) per device; otherwise `audio_busy`. One pending pairing per device; otherwise `pair_busy`. |
| Timeouts | Device command 6 s, which is under the Go CLI's 8 s budget and the common 10 s tool-call cap of agent runtimes. TTS 20 s, STT 30 s. Pairing window 120 s. Jobs kept 15 min. |

## Run locally (nothing is deployed, no login)

```sh
cd relay
pnpm install --ignore-workspace
pnpm typecheck
pnpm test
pnpm e2e
(cd ../cli && make build) && node tools/e2e.mjs --go-cli ../cli/bin/molty
pnpm dev
node tools/fake-device.mjs --relay ws://127.0.0.1:8787 --id molty-k7m2w9c4ya --key-file /tmp/molty.key --link-file /tmp/molty.link
```

1. Use pnpm. `npm install` currently crashes in arborist on vitest 5's optional peers.
2. `pnpm e2e` covers:
   - **Phase 1** (`VOICE_PROVIDER=none`): the fake device pairs. The scripted agent then calls all 10 MCP tools and every REST command, including say/listen through `molty_wait` and `GET /v1/jobs`. A device with the wrong key gets 4401.
   - **Phase 2**: a restart on the same local storage with `VOICE_PROVIDER=workers-ai`. The token survives and `say` fails cleanly.
3. The Go CLI runs with `HOME` pointed at a temp dir, so `~/.config` is never touched.
4. `pnpm dev` serves http://127.0.0.1:8787 with `VOICE_PROVIDER=none`. Workers AI only runs remotely: `wrangler dev --local` starts, but `AI.run` throws "Binding AI needs to be run remotely".
5. Run the fake device alongside `pnpm dev`. It prints its link code, auto-accepts pairing and answers commands.

## Deploy your own relay

```sh
cd relay
pnpm install --ignore-workspace
pnpm test
pnpm e2e
npx wrangler login
npx wrangler deploy
npx wrangler secret put OPENAI_API_KEY
npx wrangler tail molty-relay
```

1. This creates a Worker named `molty-relay` (DO class `MoltyDevice`, migration `v1`) on your Cloudflare account. Point devices at it with the console's relay setting (or build the firmware with your URL as `CONFIG_MOLTY_DEFAULT_RELAY`, see docs/AGENT-LINK.md).
2. The `OPENAI_API_KEY` secret is optional. Set it only to use `--var VOICE_PROVIDER:openai`, or set `VOICE_PROVIDER` to `openai` under `vars` in `wrangler.jsonc`.
3. Smoke-test after deploy:
   - Pair a fake device against `wss://molty-relay.<subdomain>.workers.dev`.
   - Call `molty_say` to confirm real Aura TTS. This is the first point where the `@cf/deepgram/aura-2-en` linear16/wav output path is exercised for real.
   - Wait more than 30 s idle, then send a command to confirm hibernation survival. `wrangler dev` never evicts, so only a deployed Worker can prove it.

## Waitlist

The website's "want one built?" form (molty.uno/build) posts to `POST /waitlist` here.

| | |
|---|---|
| Accepts | JSON `{email, website, source}` (the page's script) or a form-encoded POST (no JavaScript: answered with a 303 back to `<origin><return>?waitlist=ok#waitlist-ok`) |
| Answers | `{ok:true}` for a new address, a known one (idempotent) and a filled honeypot (`website`) alike; `400 bad_email`, `429 rate_limited` (+ `Retry-After`), `403 forbidden_origin` / `human_check_failed`, `415`, `503 unavailable` |
| CORS | `https://molty.uno`, `https://www.molty.uno`, `http://localhost:*`, `http://127.0.0.1:*`; another Origin is refused |
| Rate limit | 5 requests per IP per hour, counted in D1 under `sha256(IP)`; past windows are deleted |
| Stored | `email` (trimmed, lower-cased, unique), `created_at`, `source`. No IP, no user agent |
| Human check | Cloudflare Turnstile, only when the `TURNSTILE_SECRET` secret is set (then set `TURNSTILE_SITE_KEY` in `website/src/config/site.mjs` too; signing up then needs JavaScript) |

Set it up once, on the account the relay is deployed to (nothing in the repo does this):

```sh
cd relay
pnpm install --frozen-lockfile
npx wrangler whoami
npx wrangler d1 create molty-waitlist
npx wrangler d1 migrations apply molty-waitlist --remote
npx wrangler secret put TURNSTILE_SECRET
pnpm run deploy
curl -s -X POST https://<relay>/waitlist -H 'content-type: application/json' -d '{"email":"you@example.org","source":"test"}'
make -C .. waitlist-export
```

1. `whoami` shows the account every following command acts on.
2. `d1 create` prints a `database_id`: paste it over the placeholder in `wrangler.jsonc`
   (`d1_databases`). Until then a deploy fails, because the placeholder database does not exist.
3. Applies `migrations/` (the `waitlist` and `waitlist_rate` tables). New migrations later:
   the same command.
4. Optional. Skip it and no human check runs.
5. The website's `WAITLIST_URL` (`website/src/config/site.mjs`) must point at this relay's
   `/waitlist`.
6. Smoke test; then delete the test row: `make waitlist-delete EMAIL=you@example.org`.
7. Writes `waitlist.csv` at the repo root (gitignored: personal data).

Deleting an address (GDPR request): `make waitlist-delete EMAIL=someone@example.org` from
the repo root. Everything about an address is that one row.

Locally (no account): `pnpm exec wrangler d1 migrations apply molty-waitlist --local`, then
`pnpm dev`; signups land in `.wrangler/state` (`--local` instead of `--remote` on any
`d1 execute` reads them).

## Configure an agent

Relay URL below is `https://molty-relay.<subdomain>.workers.dev`. Pairing needs no
token: connect without the header, call `molty_pair`, then add the header.

**Claude Code**

```sh
claude mcp add --transport http molty-pair https://molty-relay.<subdomain>.workers.dev/mcp
claude mcp remove molty-pair
claude mcp add --transport http molty https://molty-relay.<subdomain>.workers.dev/mcp --header "Authorization: Bearer <agent_token>"
```

In between, ask Claude to call `molty_pair {device_id, link_code, agent_name}`.

**Any MCP client with Streamable HTTP + headers** (Claude Desktop / Cursor `mcp.json` style):

```json
{
  "mcpServers": {
    "molty": {
      "type": "http",
      "url": "https://molty-relay.<subdomain>.workers.dev/mcp",
      "headers": { "Authorization": "Bearer <agent_token>" }
    }
  }
}
```

**Clients without custom-header support** (stdio-only clients), through `mcp-remote`:

```json
{
  "mcpServers": {
    "molty": {
      "command": "npx",
      "args": ["-y", "mcp-remote", "https://molty-relay.<subdomain>.workers.dev/mcp", "--header", "Authorization: Bearer ${MOLTY_TOKEN}"],
      "env": { "MOLTY_TOKEN": "<agent_token>" }
    }
  }
}
```

**Hosted agents** (e.g. Grok's bots): the agent's machine must reach the URL publicly;
a workers.dev URL does.

```json
{ "type": "http", "url": "https://molty-relay.<subdomain>.workers.dev/mcp", "headers": { "Authorization": "Bearer <agent_token>" } }
```

**molty CLI** (Go harness):

```sh
molty pair --relay https://molty-relay.<subdomain>.workers.dev --device molty-xxxxxxxxxx --code <LINKCODE> --name "<agent>"
molty status
```

The CLI saves the link to `~/.config/molty/link.json`.

Not verified: hosted agent apps honoring custom `headers`, and claude.ai web connectors. Those connectors
expect OAuth rather than a static bearer, so use Claude Code, Claude Desktop with
`mcp-remote`, or the CLI there.

## Instruction block for the device console ("Connect an agent")

The console fills `{relay}`, `{device_id}` and `{link_code}`. The code rotates every
15 min and after each pairing.

```text
Connect an agent to Molty
  Relay:     {relay}
  Device ID: {device_id}
  Link code: {link_code}   (valid ~15 min, single use)

1. Add this MCP server to your agent (no token needed yet):
     {relay}/mcp            (Streamable HTTP)
2. Ask the agent to call:
     molty_pair {"device_id":"{device_id}","link_code":"{link_code}","agent_name":"<your agent's name>"}
3. Molty shows the agent's name and a 4-digit code. Check that the code matches what
   the agent reports, then tap the screen (or press BOOT) to accept. PWR or 30 s = reject.
   If the agent gets status "pending", it calls molty_pair again with the pair_id it got.
4. The agent receives an agent_token once. Add it to the MCP config as
     Authorization: Bearer <agent_token>
   and reconnect. All molty_* tools now work.

CLI instead:  molty pair --relay {relay} --device {device_id} --code {link_code} --name <name>
REST instead: POST {relay}/v1/pair {"device_id":"{device_id}","link_code":"{link_code}","agent_name":"<name>"}
```
