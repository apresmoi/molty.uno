# Running your own private relay

Molty works with the official relay out of the box. An owner who wants everything on their own
account runs the same relay code as a Cloudflare Worker of their own, points Molty at it, and
pairs agents there. Nothing else changes for agents: same API, same skill, a different
`$MOLTY_RELAY`.

```
Molty ──WSS──▶ your relay (Worker + one Durable Object per Molty) ◀── agents (REST / MCP)
                 └─ voice: Workers AI on your account | OpenAI-compatible key | none | your voice box
```

## 1. Deploy the relay to your Cloudflare account

```sh
git clone https://github.com/apresmoi/molty.uno.git
cd molty.uno/services/relay
pnpm install --ignore-workspace
pnpm test
npx wrangler login
npx wrangler whoami
$EDITOR wrangler.jsonc
pnpm run deploy
npx wrangler tail molty-relay
```

1. Node 22 and pnpm. Tests run locally, no account needed.
2. `whoami` names the account every following command acts on: check it is yours.
3. Edit `wrangler.jsonc` before deploying:
   - **Remove the `d1_databases` block** (it names the project's own waitlist database, which your
     account cannot reach; without it `/waitlist` simply answers 503 — agents never use it). Or
     create your own with `npx wrangler d1 create molty-waitlist` and paste its `database_id`.
   - **Voice** under `vars` (see [voice.md](voice.md)): keep `VOICE_PROVIDER: "workers-ai"` (the
     `ai` binding, billed to your account), or `"openai"` plus
     `npx wrangler secret put OPENAI_API_KEY` (and `OPENAI_BASE_URL` for any OpenAI-compatible
     server), or `"none"`.
   - `RELEASES_REPO` stays `apresmoi/molty.uno` so firmware update checks keep working; optional
     `npx wrangler secret put GITHUB_TOKEN` raises GitHub's rate limit.
4. `pnpm run deploy` bakes the product version into `/v1/health` (plain `wrangler deploy` reports
   `dev`). It creates the Worker `molty-relay`, so the URL is
   `https://molty-relay.<your-subdomain>.workers.dev`. Credentials come from wrangler's login or
   `CLOUDFLARE_API_TOKEN` + `CLOUDFLARE_ACCOUNT_ID` (yours).
5. Check it: `curl https://molty-relay.<your-subdomain>.workers.dev/v1/health` →
   `{"ok":true,"service":"molty-relay","version":…,"api_version":1}`.

Full operator notes (local runs, limits, smoke tests): `services/relay/README.md` in the repo.

## 2. Point Molty at it

On Molty's console (`molty.local`, or USB): the agent page (or setup's agent step) →
**Advanced** → **relay** → your relay's origin (`https://molty-relay.<your-subdomain>.workers.dev`,
no path) → **Save**. The console stores it on Molty as a `wss://` origin
(`POST /api/link {"relay_url":"wss://…"}`) and Molty reconnects at once; on the new relay it
claims its device id afresh.

Building your own firmware instead? Set `CONFIG_MOLTY_DEFAULT_RELAY` (see "Default relay" in
`docs/AGENT-LINK.md`).

## 3. Pair agents (and the voice box) again

Tokens belong to the relay that minted them: nothing carries over from the official relay.

- The console's setup message now names your relay (`Relay: https://…`). An agent using the
  helpers pairs with `MOLTY_RELAY=<your relay> "<skill-dir>/scripts/pair.sh" <DEVICE> <CODE> "<name>"`;
  the relay is stored in `~/.config/molty/env` with the device.
- MCP clients use `<your relay>/mcp`; the `molty` CLI `molty pair --relay <your relay> …`.
- A voice box: `molty-voice pair --relay <your relay> …` (the console's Voice card shows it filled in).

## What you gain and lose

| Gain | Lose / take on |
|---|---|
| Speech, transcripts, events and the activity log pass through and stay in your own account | You pay for and run it: Workers + Durable Object usage, and Workers AI speech if you use it |
| Your choice of voice provider (Workers AI, any OpenAI-compatible API, none) and your own limits | No speech until you configure a provider (or pair a voice box) |
| Independence from the project's hosted service | Updates are yours: redeploy to get relay fixes. Agents compare `api_version` with the one their skill targets ([updates.md](updates.md#api_version)) |
| Firmware update checks still work (`/v1/releases/latest`, GitHub releases of `RELEASES_REPO`) | The project's waitlist form is not part of it |
