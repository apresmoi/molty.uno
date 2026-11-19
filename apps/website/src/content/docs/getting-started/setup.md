---
title: Set up your Molty
description: Wi-Fi, choose a Molty, connect an agent, pick a voice. The four steps of the setup console.
---

Molty serves its own setup console. The first time, you reach it over the USB-C cable.

:::note[Waveshare 1.28 and other boards with a serial USB-C]
These boards have no USB network, so Wi-Fi comes first, from your browser: open
[the setup page](/setup/) in Chrome or Edge on a desktop, connect to the board's serial
port and choose **Connect to Wi-Fi**. The password travels over the cable to Molty only.
Then open `http://molty.local` and continue below from step 2.
:::

## Open the console

Plug Molty into your computer. It shows up as a USB network adapter and hands your
computer an address. Open:

```
http://10.13.37.1
```

After Wi-Fi is set up you can also use `http://molty.local` from any device on the same
network.

## 1. Wi-Fi

Molty asks which network to join and for its password. Molty only uses **2.4 GHz**
networks.

## 2. Who should I be?

Pick one of the creatures your Molty holds: the duck, the cat or the octopus out of the box,
plus any creature package you installed. It appears on the screen right away, with no reflash.
You can change it later in the console's Molty page.

## 3. Who will be my brain?

The console shows one message with your device ID and a link code. Copy it and send it
to your agent. Continue with [Connect an agent](/agents/connect/).

Molty shows a 4-digit code with `accept` and `reject` on the rim. Tap `accept` if the agent shows
the same code.

You can skip this step and connect an agent any time later from the console's Agent page.

## 4. Should I talk?

Up to three short questions, each changeable later in the console's Voice page:

| Question | What you choose |
|---|---|
| Should I talk? | where Molty's voice comes from, or text only |
| Agents can be slow. What should I do meanwhile? | what fills the gap until your agent speaks |
| Should I react while you talk? | who picks Molty's small reactions |

Next: [What Molty shows](/getting-started/on-screen/).
