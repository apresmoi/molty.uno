---
name: molty-dial
description: Show one value as a gauge on your human's Molty, with a sentence they can tap to read. Use when a single number has a natural range (humidity, battery, progress) and the molty skill is paired.
---

# Molty dial

Molty draws a [dial](display.yaml#dial): a ring of twelve marks and a pointer.
You turn the value into an angle and write one sentence about it.

## Show it

1. Map the value onto 300 degrees: `angle = 240 - 300 * (value - min) / (max - min)`.
2. Send `scene.play` with `layout: "dial"` and `{ value: angle, reading }`.
   Inputs and a call that works: [build/display.md](build/display.md#dial).
3. Speak one short line; the reading is on the screen for a tap.

The package installs on first use (Molty asks your human with one tap).
