---
title: Introduction
description: What molty.UNO is (the fluid, the Moltys, the bridge) and which screens it runs on.
---

molty.UNO is not one device. It is three portable things you bake onto a screen:

1. **The fluid.** One liquid of braille characters fills the screen. Everything it shows
   (words, creatures, questions, ripples) comes out of the water and goes back into it.
2. **The Moltys.** Small creatures that live on the screen. A device ships with a duck, a cat
   and an octopus as data packages, swapped without a reflash. The lighter cute-kit Moltys are
   ~1 KB `.molty` files.
3. **The bridge.** Moltys have no brain of their own. Any agent (Claude, Grok, OpenClaw, or
   anything that speaks MCP or HTTP) becomes one, paired with one message and a tap on the
   screen. It can then speak and listen, ask a question you answer with a tap, set moods, and
   draw scenes from installed packages, such as the real sky.

```
agent ──MCP / REST──▶ relay ◀──outbound WSS── your screen
```

## Boards

| Board | Screen | Status |
|---|---|---|
| Waveshare ESP32-S3-Touch-LCD-1.46 | 412×412 round, touch, mic, speaker | reference |
| Waveshare ESP32-S3-Touch-AMOLED-1.75C | 466×466 round AMOLED, touch, mic, speaker | experimental |
| Waveshare ESP32-S3-Touch-LCD-1.28 | 240×240 round, touch, no audio | experimental |
| Your screen | anything with an ESP32-S3 | port it |

You also need a USB-C cable and an agent you already use.

## Where to go next

1. [Flash a release](/getting-started/flash-a-release/): the short path.
2. [Build and flash](/getting-started/hardware/): from source, with the toolchain.
3. [Set up your Molty](/getting-started/setup/): Wi-Fi, pick a Molty, connect an agent, voice.
4. [What Molty shows](/getting-started/on-screen/): the screen, and what your hands do.
5. [Connect an agent](/agents/connect/): what the one message does.

Everything (firmware, console, relay, CLI, packages and the creature engine) is MIT-licensed and
lives in one repository.
