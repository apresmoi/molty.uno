# Updates: firmware, api_version, this skill

## What Molty can do: caps, not versions

`status` returns `relay.caps`, the features Molty's firmware and board actually have (e.g. `ask`
in caps = `molty_ask` works; `say` / `listen` = speaker / mic; `shake_talk`; `update` = it can
update itself). Decide from caps — never from the firmware version, the release list or this
skill's history.

## api_version

The relay states the agent API contract it speaks as an integer `api_version`: in the pair
result, `status` → `relay.api_version`, `GET $MOLTY_RELAY/v1/health`, `molty_updates` and the MCP
`serverInfo` ("Molty relay (api v1)"). This skill is written for **api_version 1**. A relay
reporting a higher number has changed something agents must handle: tell your human to update
the molty plugin (below) and carry on with what still works.

## Updating this skill

The relay does not serve or announce the skill. It ships only as the **molty plugin** from the
git marketplace at `apresmoi/molty.uno`:

```sh
claude plugin marketplace update molty && claude plugin update molty@molty   # Claude Code
codex plugin marketplace upgrade molty && codex plugin add molty@molty         # Codex
```

Then start a new session. Agents without a plugin system read the skill from GitHub:
`https://github.com/apresmoi/molty.uno/tree/main/skills/molty` (raw files under
`https://raw.githubusercontent.com/apresmoi/molty.uno/main/skills/molty/`).

`$MOLTY_RELAY/skill.md` is **deprecated**: it answers a permanent redirect to the raw
`SKILL.md` on GitHub, for agents set up before the plugin. Point your own notes at GitHub instead.

## Firmware updates

**When your human asks to check Molty for updates (any wording — "is Molty up to date?",
"any news for Molty?"): call `molty_updates`** (REST `GET $MOLTY_RELAY/v1/devices/<id>/updates`),
then follow its `actions` in order:

1. If the firmware is behind, say so and **ask before updating**. On yes, call `molty_update`
   (REST `POST $MOLTY_RELAY/v1/devices/<id>/update` with `{}`): Molty asks your human on its screen too
   (tap the version, e.g. "v0.2.0", or "later"), downloads and checks the official release, installs it
   and restarts (about a minute; finish with `molty_wait`). If `can_self_update` is false, tell them
   to open Molty's console → home instead; it shows the update and installs it.
2. If nothing is new, say so in one sentence.

Tell your human what's new **in plain words** (what you can do for them now — not version numbers
or tool names).

```
{"api_version":1,
 "firmware":{"current":"0.1.0","board":"…","online":true,"latest":"0.2.0","status":"update_available","update_available":true,
             "notes_url":"…","needed_for":[{"feature":"molty_ask","what":"…","needs":"0.2.0","released":true}],
             "can_self_update":false,"release_check":"fresh"},
 "actions":["Tell your human Molty firmware 0.2.0 is out …"]}
```

- `firmware.status`: `current`, `update_available`, `no_build_for_board`, `no_release` (nothing
  published yet), `unknown` (a development build, or the release list was unreachable: say so).
- `molty_update` results: `{"status":"up_to_date"}` (nothing to do), or a job that ends
  `done` with `outcome` `updated` / `declined` (the human tapped "later": fine, don't push),
  or fails with `update_failed` / `rolled_back` (Molty kept its old firmware) / `update_timeout`.
- Never update without asking; never more than once per request.
- `molty_status` carries the same firmware block as `relay.firmware`.

### firmware_too_old

A feature that needs newer firmware than Molty runs answers
`409 {"error":"firmware_too_old","feature","current","needed","latest","how"}` (e.g. `molty_ask`
needs 0.2.0). Tell your human which update unlocks it (`how` says whether it is out yet), and
carry on without it (e.g. `molty_talk` instead of `molty_ask`).

### The passive notice

You also hear about firmware without asking: once per new firmware release your next successful
call carries `firmware_update` `{current, latest, board, notes_url, can_self_update, how}`
(MCP: plus a line "Molty firmware … is out"). Mention it once.
