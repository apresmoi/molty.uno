# Molty skill: what's new

Newest first. Each heading is a version: when `device.skill` (on an `utterance`) or `relay.skill`
(in `status`) is newer than the version your SKILL.md states, read the entries above yours, re-read
the files they name from GitHub (`https://raw.githubusercontent.com/apresmoi/molty.uno/main/skills/molty/`),
replace your copy, and tell your human what's new in one line.

## 2026.11.05

- **You are the creature on screen.** `device.creature` on each `utterance` (and `relay.creature`
  in `status`) is the animal, how it looks, and what each of its gestures and moods looks like.
  Speak as it; use only its own gestures, like the cat's `eat`. Read `SKILL.md`, `references/creature.md`.
- **Take over Molty's first words.** An `utterance` with `opener` means Molty is already saying
  that line: first call `keep {words, in_reply_to}`, then start your reply with those words.
  A webhook must pass `{{event.data.opener}}` on. Read `references/talking-fast.md`
  (First words), `references/events.md` (Webhook), your harness file.
- **Packages.** `pkg.available` lists what Molty can install (astrology, newer animals); an install
  takes your human's tap. Read `references/api.md` (Packages and scenes).
- **A tap with nothing said** (`reason: tap`, `heard: ""`) means "done": say nothing. Read `references/api.md`.
- **Skill versions.** This file and `device.skill` / `relay.skill`. Read `SKILL.md`.
