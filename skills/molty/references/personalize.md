# Personalize: learn your human, offer what they can change

Molty should get better the longer you know each other. Two jobs: remember, and offer.

## Learn

Keep your own notes on this human ([etiquette.md](etiquette.md) has the behaviour ledger; use
relay notes if you have no memory). Note what they tell you and what you can see:

- Pace: do they want one word or some context? Do they interrupt?
- Language, names, topics, jokes that landed, moods they dislike.
- A correction ("stop saying that", "too long") is a rule from then on. Apply it right away,
  and say so in a few words.
- Your phrases: replace generic fillers with the ones they'd say.

Don't announce that you are learning, and don't ask a questionnaire. Pick it up as you go.

## Offer

Mention **one** thing, in one sentence, at a moment it would help: never a feature tour, and
never twice if they pass. Change a setting only when they ask.

| They… | Offer |
|---|---|
| Finished setup or a first good chat | "You can double tap me to talk, or give my buttons a job." |
| Shake a lot, or dislike shaking | Bind `double_tap` or a button to `talk` |
| Hit the 30 s recording limit | "Tap me when you're done; or I can listen without a limit." |
| Get stray events from touches | Set `tap` to `none` |
| Hear the same filler all the time | Register phrases in their words |
| Want a quiet Molty | `idle_life`, and the filler setting in the device console |

## What they can customise

- **Gestures** (`gestures`, cap `gestures`; [api.md](api.md#gestures-what-tap-double-tap-and-the-buttons-do)):
  `tap`, `double_tap`, `button_accept`, `button_reject` each do `none`, `talk`, `event` or `stop`.
  A tap while recording always stops and sends.
- **A button with your own meaning:** set it to `event` and ask them what it should do ("next
  song", "start my day"), then write that down. You receive a `button`, `tap` or `double_tap`
  event. If your harness wakes only on `utterance` (see its recipe), add that one event to your
  webhook only if they want it: a run it starts can collide with a live conversation.
- **Quick phrases** (`molty_phrases`): your own instant lines, in their language and your voice.
- **Device console:** ring colour, listening window, filler delay, shake to talk, idle life, ack.
- **Creature and voice:** whichever your `relay.caps` and [voice.md](voice.md) offer.
