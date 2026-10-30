# Codex: attach a session to Molty

Install once per machine:

```sh
codex plugin marketplace add apresmoi/molty.uno
codex plugin add molty@molty
```

Like a Claude Code session, a Codex session reaches Molty only while it runs, and between turns
it is not running at all. Use the helpers in `<skill-dir>/scripts/` (bash, `curl`, `python3`;
the sandbox must allow network access to the relay).

## Connect

1. **Paired?** `~/.config/molty/token` exists → yes. If not: `"<skill-dir>/scripts/pair.sh" <DEVICE> <CODE> "Codex"`,
   tell the human which code Molty shows, they tap.
2. **Say hello once:** `"<skill-dir>/scripts/talk.sh" "Codex here, connected." --no-listen`.

## Hearing Molty

Codex has no persistent monitor, so pick one:

- **At the start of each turn** run `python3 "<skill-dir>/scripts/watch.py" --once` and act on
  what it prints (an `utterance` → answer it with `talk.sh "<reply>" --reply <turn_id>`).
- **While you wait for the human anyway,** `watch.py --once 25` long-polls up to 25 s.
- **To react while Codex is idle,** run the `molty` CLI as a long-lived process outside the
  session (`molty events --exec '<your wake hook>'`, e.g. a `launchd` / `systemd --user` service)
  — that is the long-lived pattern in [openclaw.md](openclaw.md).

Be honest about the limit: between turns nothing wakes this session, so don't promise to react
live; say you'll see Molty the next time you run.

## Converse

Same helpers and rules as [claude-code.md](claude-code.md#converse): one `talk.sh` per turn,
`ask.sh` for decisions, `mood.sh` for feelings, `--no-listen` to finish.
