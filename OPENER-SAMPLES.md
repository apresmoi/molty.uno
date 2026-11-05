# Opener samples

First words with overlap: the box's small model writes a 12 to 20 word opener, the agent keeps
up to word m and takes over. Measured 2026-11-05 against the live voice box model.

- **Model:** `gemma3:4b` (Q4_K_M) on LeDeluge's Ollama, warm, shared with Kyutai on the GPU.
- **Prompt:** `services/molty-voice/molty_voice/opener.py` `SYSTEM` (this branch), `num_predict` 48, temperature 0.4.
- **Guard:** `services/relay/src/core/opener.ts` `guardOpener` (this branch: up to 22 words / 150 chars; new drops for `n't` claims, hedges, meta words, any `?`).
- **Latency** (40 calls, 20 utterances x 2, timed by `opener.generate` from the LAN): median 355 ms, p90 406 ms, max 475 ms, min 264 ms. About 120 ms is prefill of the 790-token prompt, about 220 ms is generating ~24 tokens. The old 2 to 6 word lines took ~130 ms.
- **Deadline:** the relay's `OPENER.deadlineMs` goes 500 -> 800 ms (box time plus the relay-to-box round trip). Pilot check: the activity log's `ev:"opener"` `ms` and `dropped:"no_line"` count.
- **Pass rate:** 17/20 through the guard (no line is spoken for the 3 dropped).

## Samples

| # | Human | Opener | words | ms (run 1 / 2) | guard |
|---|---|---|---|---|---|
| 1 | what's the weather going to be like in Berlin tomorrow? | Ooh, Berlin tomorrow, you want to know the weather there. Let me check the forecast for you. | 17 | 320 / 420 | pass |
| 2 | can you search the weather in Berlin | Ooh, Berlin weather, you want to know what’s happening there. Let me get that information for you. | 17 | 360 / 376 | pass |
| 3 | remind me to call my sister at six | Ooh, your sister at six, you need a gentle nudge. Let me set that up for you, right away. | 19 | 379 / 350 | pass |
| 4 | how do I make a really good risotto? | Ooh, a really good risotto, that sounds lovely and comforting. Let me explain the key parts. | 16 | 350 / 371 | pass |
| 5 | I had the worst day at work today | Oh no, a bad day at work, that’s truly upsetting, let’s talk about it. | 14 | 355 / 475 | pass |
| 6 | what's the difference between a crocodile and an alligator? | Ooh, crocodiles and alligators, how they differ, I see. Let me explain the key distinctions for you. | 17 | 355 / 368 | pass |
| 7 | did the tests pass? | Right, the tests, how the latest run of them went. Let me give you the rundown. | 16 | 317 / 334 | pass |
| 8 | explain how a black hole forms | Ooh, a black hole, how they come to be. Let me tell you about the process, starting with the stars. | 20 | 371 / 378 | pass |
| 9 | can you summarise my emails from this morning? | Okay, so summarise your emails from this morning, let me go through them for you. | 15 | 309 / 292 | pass |
| 10 | I'm thinking of quitting my job and moving to Lisbon | Ooh, Lisbon, a new city, how exciting. Let me explore what that could entail, really. | 15 | 337 / 405 | pass |
| 11 | tell me a joke about octopuses | Ooh, an octopus joke, that’s delightful. Let me tell you one, right away. | 13 | 323 / 379 | pass |
| 12 | what should I cook tonight with eggs and spinach? | Ooh, eggs and spinach, how lovely. Okay, let me give you some ideas to consider. | 15 | 362 / 348 | pass |
| 13 | is it a good idea to learn Rust or Go first? | Ooh, Rust or Go first, a tricky choice for a new learner. Let me outline the differences for you. | 19 | 353 / 406 | pass |
| 14 | how far is the moon? | Ooh, the moon, how far away it actually is. Let me explain the distance to you. | 16 | 331 / 337 | drop: result |
| 15 | my cat won't stop scratching the sofa | Oh, the sofa being scratched, I understand that frustration completely. Let me explain what might be happening. | 17 | 365 / 330 | drop: claim |
| 16 | set a timer for ten minutes | Ooh, ten minutes, you want a countdown starting now, right? Let me get that established. | 15 | 346 / 395 | drop: question |
| 17 | who won the match last night? | Ooh, the match last night, you want to know the result. Let me find out for you. | 17 | 362 / 349 | pass |
| 18 | qué me recomiendas para dormir mejor? | Ooh, better sleep, you want some advice on that. Let me give you a few things to try. | 18 | 369 / 409 | pass |
| 19 | and what about the weekend? (after "Rain most of the day, around twelve degrees.") | Okay, and the weekend, how the weather shifts then. Let me check. | 12 | 285 / 297 | pass |
| 20 | why did it fail? (after "The deploy failed on the database migration.") | Okay, why the database migration step fell over. Let me take you through it, one piece at a time. | 19 | 379 / 264 | pass |

## Quality, honestly

- **Good:** 7, 9, 12, 17, 19, 20: specific, no facts, an agent can branch after the first clause.
- **Fine:** 1, 2, 3, 6, 8, 11, 13: specific but formulaic ("you want to know...", "for you").
- **Weak:** 4 and 10 describe the topic ("lovely and comforting", "how exciting"); 5 is fine but blunt;
  18 answered a Spanish question in English (the 4B model drifts language with a mostly English prompt).
- **Habit:** most start with "Ooh". Telling it not to did not change that; a bigger model (12B) would,
  but it does not fit next to Kyutai in the 16 GB without evicting the 4B one, so it was not tried.
- **Dropped by the guard, correctly:** 14 ("how far away it actually is"), 15 ("I understand that
  frustration completely"), 16 (a question mid-line).
- Versus today's lines ("Right, assessing that,", "Okay, that's a question, so"): every line now names
  what was said; none is meta ("question" and "interesting" are now guard drops).

## The seam (Kyutai TTS on the box, default voice)

Opener "Okay, the weather in Berlin, for tomorrow, let me walk you through it." cut after word 5
("Berlin,"), the relay's cut point (the next word's TTS cue):

| continuation (voiced alone) | silence before speech | first 300 ms loudness (rms) |
|---|---|---|
| line, last 100 ms before the cut | (in the comma's pause) | 0.005 |
| line speech before the cut | | 0.067 |
| `rain until noon, then sun.` | 0 ms | 0.067 |
| `Rain until noon, then sun.` | 109 ms | 0.055 |
| `and rain until noon, then sun.` | 583 ms | 0.032 |
| one take, "rain" after "Berlin," | 4 ms | 0.062 |

- The cut lands in the comma's pause: no word is clipped (plus a 10 ms fade).
- A lower-case continuation starts at once at the line's loudness; a capital adds ~110 ms and a softer
  start (sentence-initial prosody). So the relay keeps the reply's case and the skill asks agents to
  continue in lower case.
- The remaining gap is the hand-over: the next say goes out on the line's `say.done` (one device round
  trip; it used to go out early, which is what made the device answer `busy`). Measure it on the pilot
  in the activity log (`reply` `first_audio_ms` versus the line's end).
