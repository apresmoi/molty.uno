---
title: Set up your Molty
description: Put Molty on a board, join your Wi-Fi, pick a creature and connect an agent.
---

You need a board, a USB-C data cable, a desktop computer with Chrome or Edge, and 2.4 GHz
Wi-Fi. It takes a few minutes.

## 1. Put Molty on the board

Open [the setup page](/setup/), plug the board in and press **Connect**. The page works out
which board you have and offers the right install. Installing erases the board.

On the 1.46 and the 1.75C, hold the **BOOT** button while you plug the cable in. When the
install ends, unplug the cable and plug it back in, with no button held.

If the page has no install for your board yet, you can
[build it from source](/getting-started/hardware/).

## 2. Open Molty's console

Molty serves its own settings page, the console.

- **1.46 and 1.75C.** Leave the cable in and open this address:

  ```
  http://10.13.37.1
  ```

- **1.28.** Go back to the setup page and choose **Connect to Wi-Fi**. Then open this
  address from any device on the same network:

  ```
  http://molty.local
  ```

Your Wi-Fi password goes over the cable to Molty and nowhere else.

## 3. Answer four questions

The console asks them one at a time.

| Molty asks | You do |
|---|---|
| Which Wi-Fi should I use? | pick your network and type its password |
| Who should I be? | pick the duck, the cat or the octopus |
| Who will be my brain? | copy the message into your agent, then tap `accept` on Molty |
| Should I talk? | choose a voice, or text only |

You can skip the agent and come back later. Every answer can be changed in the console.

Next: [Connect an agent](/agents/connect/).
