# OpenClaw and other long-lived agents: webhook or event loop

A long-lived agent (OpenClaw, PicoClaw, a resident bot, a server) can hear Molty at any time,
not only during a session. Pair once ([api.md](../api.md#pair)), keep the token in your secret
store, and pick one way to be woken. Examples, not rules: you know your environment.

## With a reachable HTTPS endpoint: webhook

Register `PUT $MOLTY_RELAY/v1/webhook` pointing at your own endpoint
([events.md](../events.md#webhook)):

- Default body (`{"device","event"}` JSON) and verify `Molty-Signature` with the returned secret;
  or a template that matches the input your runtime already accepts.
- `"events":["utterance"]` for conversation; add `tap`, `button`, `answer`, `shake` if you act on
  them.
- Webhooks wake you; the inbox stays the record. On start-up, drain
  `GET …/events?wait_s=0` once and dedupe by `id`.

## Without one: an event loop

Run the `molty` CLI as a long-lived child or a `systemd --user` / `launchd` service:

```sh
molty pair --relay "$MOLTY_RELAY" --device <DEVICE> --code <CODE> --name "OpenClaw"
molty events --exec '<your wake hook>'
```

Each event's JSON arrives on the hook's stdin: post it to your local wake endpoint or append it to
a file you watch (`--exec 'cat >> ~/.molty-events.jsonl'`). Or long-poll yourself:
`GET $MOLTY_RELAY/v1/devices/<id>/events?wait_s=25` in a loop, passing `since=<next>`. At most 2
concurrent waits per token.

## On each event

- `utterance` → one `talk` with `in_reply_to` at once, from the payload ([talking-fast.md](../talking-fast.md)).
- `shake` with `"listening": true` → nothing (the `utterance` follows).
- `tap`, plain `shake`, `button` → a nudge: a mood or a few words, once per moment
  ([etiquette.md](../etiquette.md)).
- `answer` → the end of one of your asks (you also got it from the `ask` call itself).
- Keep a warm session per Molty and your fastest model for conversation.

## Updating

Install the skill where your runtime reads skills, from
`https://github.com/apresmoi/molty.uno/tree/main/skills/molty`, and refresh it when the relay's
`api_version` rises above the one in [updates.md](../updates.md).
