---
title: Introduction
description: What Molty is, what you need, and where to go next.
---

Molty is a small round screen for your desk. The whole screen is one calm fluid drawn in
braille characters. Creatures — **Moltys** — emerge from it, dance, read text out of it,
and melt back in.

Molty has no brain of its own. **Your AI agent is its brain**: Claude, Grok, OpenClaw, or
anything that speaks MCP or HTTP. You pair the agent with one message and a tap on the
screen, and from then on the agent can set Molty's mood, show text in the fluid, speak
through its speaker and listen through its mic.

```
your agent ──MCP / REST──▶ relay ◀──outbound WSS── Molty on your desk
```

## What you need

- A **Waveshare ESP32-S3-Touch-LCD-1.46** board (round 412×412 touch screen, mic,
  speaker, IMU, battery charging).
- A USB-C cable and a computer for the first flash.
- An agent you already use.

## Where to go next

1. [Hardware, build and flash](/getting-started/hardware/) — the board and the toolchain,
   if you build from source.
2. [Flash a release](/getting-started/flash-a-release/) — the shorter path, once releases
   are published.
3. [Set up your Molty](/getting-started/setup/) — Wi-Fi, pick a Molty, connect an agent.
4. [Connect an agent](/agents/connect/) — what the one message does, and what your agent
   can do afterwards.

Molty is open source under the MIT licence. The firmware, console, relay, CLI and the
creature engine all live in one repository.
