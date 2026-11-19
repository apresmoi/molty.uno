---
title: Introduction
description: A small creature on a round screen that any AI agent can speak, listen and ask through.
---

Molty is a small creature that lives on a round screen on your desk. It has no brain of its
own. You connect an agent you already use, and the agent speaks, listens and asks through it.

## Three parts

- **The water.** The screen is a liquid made of tiny dots. Words, creatures and questions come
  out of it and melt back in.
- **The creature.** A duck, a cat or an octopus. You pick one, and you can change it any time.
- **The agent.** Claude, Grok, OpenClaw, or anything that can make web requests. You pair it
  with one message and one tap on the screen.

## What you do with it

- Shake Molty and talk. Your agent hears you and answers out loud.
- Answer your agent's questions with one tap.
- Watch a ring fill while your agent works on something long.
- Ask for the sky. Your agent draws tonight's moon, the planets and the zodiac.

## What you need

A board, a USB-C data cable, 2.4 GHz Wi-Fi and an agent.

| Board | Screen | Sound |
|---|---|---|
| Waveshare ESP32-S3-Touch-LCD-1.46 | 412×412, round | mic and speaker |
| Waveshare ESP32-S3-Touch-AMOLED-1.75C | 466×466, round, the largest | mic and speaker |
| Waveshare ESP32-S3-Touch-LCD-1.28 | 240×240, round | none: it shows, it does not talk |

The 1.46 is the board we build on. The other two run, but are less tested. Have another
screen with an ESP32-S3 chip? You can [port Molty to it](/getting-started/porting/).

## Next

1. [Set up your Molty](/getting-started/setup/)
2. [Connect an agent](/agents/connect/)
3. [What Molty shows](/getting-started/on-screen/)
4. [Packages and scenes](/moltys/packages/)

Everything is open source under the MIT licence.
