---
title: Connect an agent
description: Pair an agent with one message and one tap, and see what it can do through Molty.
---

Any agent that can make web requests can be Molty's brain: Claude, Grok, OpenClaw and others.

## Send one message

Open the **agent** page of Molty's console. It shows a short message with your Molty's id and
a code. Copy it and paste it into your agent. The message points the agent at its own guide,
so it knows what to do next.

Using Claude Code? Install the guide once, then paste the message:

```sh
claude plugin marketplace add apresmoi/molty.uno
claude plugin install molty@molty
```

## Tap to accept

Molty shows a 4-digit code with two answers, `accept` and `reject`. Your agent tells you the
same code. If they match, tap `accept`. Nothing is paired without that tap.

## What your agent can do

You never type these. They are the small calls your agent makes.

**Speak and listen.** Molty says the line, listens until you stop talking, and hands your
words back.

```
talk {"say": "Build passed. Want the details?"}
```

**Ask a tap question.** One to five answers appear on the screen. You tap one.

```
ask {"say": "Shall I open the pull request?", "options": ["yes", "no", "later"]}
```

**Show a mood.** Happy, sleepy, surprised, thinking, working and a few more.

```
mood {"mood": "happy"}
```

**Play a scene.** A drawing from an installed [package](/moltys/packages/), here the sign Leo.

```
scene.play {"pkg": "astrology", "layout": "sign", "params": {"sign": "leo"}}
```

**Hear taps.** When you tap the screen, your agent is told where, from 0 to 1 across and down.

```
tap {"x": 0.5, "y": 0.5}
```

Every call and every answer is in the [protocol](/agents/protocol/). Working from a terminal: the
[CLI and local MCP](/agents/cli/).
