---
title: Connect an agent
description: Pair any agent with one message and a tap on Molty's screen.
---

## Send one message

The setup console (and its Agent page) gives you a message like this:

```
Hi! Please connect yourself to my Molty.

Skill: https://github.com/apresmoi/molty.uno/tree/main/skills/molty
Device: molty-xxxxxxxxxx
Code: K7M2W9C4

I'll tap Molty to accept you.
```

Paste it into Claude, Grok, OpenClaw, or any agent that can make HTTPS requests. The
skill is the agent's own guide: it explains what Molty is, how to pair, and every command.
A Molty on a private relay adds a `Relay:` line.

**Claude Code or Codex?** Install the skill once as a plugin from the git marketplace, then
paste the message:

```sh
claude plugin marketplace add apresmoi/molty.uno
claude plugin install molty@molty
```

```sh
codex plugin marketplace add apresmoi/molty.uno
codex plugin add molty@molty
```

## Tap to accept

The agent asks the relay to pair. Molty condenses a **4-digit code** out of the fluid.
If your agent shows the same code, **tap the screen** to accept. Press PWR to reject.
Nothing happens without that tap.

The agent receives a token scoped to this one Molty. An agent can revoke its own token;
forgetting the link on the device (`POST /api/link/forget`) revokes every agent at once.

## What your agent can do

| Command | Effect |
|---|---|
| `mood` | idle, listening, thinking, speaking, happy, sleepy, surprised |
| `show` | speed-read text in the fluid |
| `say` | speak through Molty's speaker while the words flow on screen |
| `listen` | record from the mic and return the transcript |
| `status`, `imu`, `volume` | read state, motion, set volume |

`say` and `listen` return a job ID right away; the agent polls for the result.

## Three ways in

- **Remote MCP** — `https://<relay>/mcp` with the token as a bearer. Tools are named
  `molty_pair`, `molty_say`, `molty_show`, and so on.
- **REST** — `POST /v1/devices/<id>/cmd {name, args}` and `GET /v1/jobs/<job>`.
- **CLI / local MCP** — the `molty` binary, either through the relay or directly over
  USB-C / Wi-Fi with no cloud at all. For Claude Code:

  ```sh
  claude mcp add molty -- /path/to/molty mcp
  ```

Full details: [Agent link protocol](/agents/protocol/) and [CLI and local MCP](/agents/cli/).

## Run your own relay

The default relay is a hosted Cloudflare Worker. The relay is open source too; deploy
your own and point the device at it from the console's Agent page (Advanced). See the
[default relay](/agents/protocol/#default-relay) section.
