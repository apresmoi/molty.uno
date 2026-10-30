---
title: Introduction
description: What molty.UNO is — the fluid, the Moltys, the bridge — and which screens it runs on.
---

molty.UNO is not one device. It is three portable things you bake onto a screen:

1. **The fluid.** One liquid of braille characters fills the screen. Everything it shows —
   words, creatures, ripples — is a shape the fluid flows toward.
2. **The Moltys.** Small creatures that emerge from the fluid, dance and melt back. Each is a
   ~1 KB `.molty` file: the same file brings the same creature to any screen, no reflash.
3. **The bridge.** Moltys have no brain of their own. Any agent — Claude, Grok, OpenClaw, or
   anything that speaks MCP or HTTP — becomes one, paired with one message and a tap on the
   screen. It can then set moods, show text in the fluid, speak and listen.

```
agent ──MCP / REST──▶ relay ◀──outbound WSS── your screen
```

## Boards

| Board | Screen | Status |
|---|---|---|
| Waveshare ESP32-S3-Touch-LCD-1.46 | 412×412 round, touch, mic, speaker | reference |
| Waveshare ESP32-S3-Touch-LCD-1.28 | 240×240 round, touch, no audio | supported |
| Your screen | anything with an ESP32-S3 | port it |

You also need a USB-C cable and an agent you already use.

## Where to go next

1. [Flash a release](/getting-started/flash-a-release/) — the short path.
2. [Build and flash](/getting-started/hardware/) — from source, with the toolchain.
3. [Set up your Molty](/getting-started/setup/) — Wi-Fi, pick a Molty, connect an agent.
4. [Connect an agent](/agents/connect/) — what the one message does.

Everything — firmware, console, relay, CLI and the creature engine — is MIT-licensed and
lives in one repository.
