# Claude Code: attach this session to Molty

A Claude Code session reaches Molty only while it runs: you can speak, ask and hear the human
while this session lives; when it ends, Molty can't wake you. (A long-lived setup that wakes on
its own is [openclaw.md](openclaw.md).)

Install once per machine (the human runs it, or you with their OK):

```sh
claude plugin marketplace add apresmoi/molty.uno
claude plugin install molty@molty
```

Helpers live in `<skill-dir>/scripts/` (this skill's directory inside the plugin).

## Connect

1. **Paired?** `~/.config/molty/token` exists → yes. If not, ask the human for the setup message
   (console → agent page) and run `"<skill-dir>/scripts/pair.sh" <DEVICE> <CODE> "Claude Code"`;
   tell them which code Molty shows; they tap. (An older install's `~/.config/molty-claude` is
   copied over by the helpers on first use.)
2. **Start the event watcher with Monitor** (persistent, one notification per event):
   `python3 "<skill-dir>/scripts/watch.py"` — description "Molty events". Lines:
   - `MOLTY utterance: '…' (reason …, turn <turn_id>)` — the human shook Molty and spoke; Molty
     already listened.
   - `MOLTY shake/tap/button/answer: {…}` — ignore a `shake` with `"listening": true` (its
     `utterance` follows).
   No Monitor? Run it with the Bash tool's `run_in_background` and read its output between turns,
   or call `watch.py --once` at the start of each turn.
3. **Say hello once**, short: `"<skill-dir>/scripts/talk.sh" "Claude here, connected." --no-listen`.

## Converse

- **Answer an `utterance` with one call:** `talk.sh "<reply>" --reply <turn_id>` — it speaks your
  reply and hears their next turn (`HEARD: '…' | reason …`). Keep going turn by turn with
  `talk.sh "<reply>"`; when they're done (or `reason no_speech`), end with `--no-listen`.
- **Blocked or need a decision →** `ask.sh "question" yes no later` instead of waiting in the
  terminal. Finished something they're waiting for → one short `talk.sh … --no-listen`.
- **Emote with intent:** `mood.sh happy`, not around talking.
- `ERROR … | job <id>` → [activity.md](../activity.md) with that job id.

The rules that matter are in SKILL.md (Key rules) and [talking-fast.md](../talking-fast.md).

## Disconnect

When the session's work is done or they say stop: `talk.sh "Signing off." --no-listen`, then stop
the Monitor (TaskStop). Leave the token in place for next time.

## MCP instead of the helpers

`claude mcp add --transport http molty "$MOLTY_RELAY/mcp" --header "Authorization: Bearer <agent_token>"`
gives you the `molty_*` tools ([api.md](../api.md#mcp-optional)); it appears after a session
restart. The helpers need no restart and keep the token out of your config.
