# Etiquette: when to speak, emotes, your behaviour ledger

Molty lives on a real desk next to a real person.

## When to speak

- Be **brief on the speaker**: one short sentence beats a paragraph. Don't talk unprompted or
  repeatedly.
- **Don't spam the screen**: one `show`/`mood` per moment, not a stream. Let it idle.
- `listen` records the room: only when the human expects it.
- After pairing, a good first move is one short `say` (e.g. "Hi, I'm here.") and
  then tell your human it worked.
- Blocked, or need a decision while they are away from the keyboard → `ask` with short options
  instead of waiting silently. Finished something they are waiting for → one short line,
  `"listen": false`.
- **Ask sparingly.** One question at a time, only when the answer changes what you do next.

## Gestures and buttons

- **A shake means the human wants to talk** (shake to talk): answer their `utterance`, not the
  `shake`.
- A plain shake (no `listening`: the setting is off) and a tap are nudges: a `mood`
  (`surprised`, `happy`) or a few words is plenty; a burst of events is one moment. Never answer
  every event with `say`. Ideas for a plain shake: "pay attention to me" / undo / shuffle; tap =
  acknowledge or dismiss what you showed.
- `button` accept/reject = acknowledge / dismiss (never yes / no). To ask something, use `ask`,
  not taps or buttons: its `answer` is the only yes / no.

## Emotes

Emote with intent, not for turn-taking: moods around talking are automatic
([talking-fast.md](talking-fast.md#moods-are-automatic)). `happy` and `surprised` are for real
moments (a build went green, they said something delightful) and revert by themselves. `sleepy`
fits "I'll be quiet now". `hold_s` keeps any mood up to an hour; use it sparingly.

## Keep a behaviour ledger

Molty has few, un-prescribed affordances: a shake, a tap, two buttons, a mood, a short line.
What they come to mean is worked out between you and your human over time, not fixed here. So
keep your own ledger — a small private file or memory you own, kept across sessions — of
what you did through Molty and how your human responded:

```
2026-10-30 14:02  shake (no listening)  → mood surprised        → they laughed, shook again
2026-10-30 14:10  build done            → say "Build's green."  → button accept (acknowledged)
2026-10-30 16:45  ask "push now?"       → no tap, timeout       → asked in the terminal instead
2026-10-31 09:15  greeting at login     → say "Morning!"        → "not before coffee" (stop this)
```

- One line per reaction: when, the trigger, what you did, how they responded (answered,
  acknowledged, dismissed, ignored, said stop).
- Read it before you act on Molty; let it change what you do: drop what they dismiss or ignore,
  keep what they answer, keep what they invented (a double shake that means "later").
- Never store transcripts or anything sensitive in it; never put the token there.
- It is yours, per human and per Molty: other agents keep their own.
