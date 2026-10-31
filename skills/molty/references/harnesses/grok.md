# Grok (and other hosted bots): long-lived, woken by a webhook

A hosted bot is not running between messages and has no shell on the human's machine. It reaches
Molty over plain HTTPS (or remote MCP) and is woken by the relay's webhook. It keeps its own
config: the relay (`$MOLTY_RELAY`, the official relay unless the setup message names one), the
device id, and the agent token in its secret store — never in chat.

## Set up once

1. **Pair** over REST or MCP ([api.md](../api.md#pair)); tell the human the 4-digit code; store
   the token as a secret.
2. **Register a webhook** that wakes you with everything a turn needs
   ([events.md](../events.md#webhook)). Point it at whatever starts a run of yours — your own
   endpoint, your platform's message/run API, or a routine's trigger URL — with
   `"events":["utterance"]` and a body template that carries the words and the turn id:

   ```sh
   curl -sS -X PUT "$MOLTY_RELAY/v1/webhook" -H "authorization: Bearer $TOKEN" -H 'content-type: application/json' -d '{
     "url": "https://<your wake endpoint>",
     "headers": {"authorization": "Bearer <your endpoint secret>"},
     "events": ["utterance"],
     "body": "{\"message\":\"Molty: the human said \\\"{{event.data.text}}\\\" (turn {{event.data.turn_id}}, device {{device}})\"}"
   }'
   ```

   Store the returned `secret` and verify `Molty-Signature` if your endpoint can. Keep
   `events` to `["utterance"]`: every other event you add (`tap`, `button`, `shake`) starts its
   own run, and a run that reacts to a tap while you are mid-conversation collides with the
   conversation's run (extra `show`/`mood` calls, a reply cut short). Molty already reacts to
   taps and shakes by itself.
3. **A routine as the backstop.** If your platform runs scheduled routines, one that calls
   `GET $MOLTY_RELAY/v1/devices/<id>/events?wait_s=0` now and then catches anything a failed
   delivery missed (the inbox keeps 24 h; dedupe by event `id`). It is not how you converse.

## On each wake

- **Act on the payload.** The webhook body already holds `text` and `turn_id`: call
  `POST $MOLTY_RELAY/v1/devices/<id>/talk {"say":"<answer>","in_reply_to":"<turn_id>"}` straight
  away — no `events` / `status` call first, no bookkeeping.
- **Use your fastest model** for the reply; escalate only for real work, and then say one short
  line first ([talking-fast.md](../talking-fast.md)).
- **Keep a warm session** if your platform allows it (a kept-alive conversation per Molty beats a
  cold start per event).
- `talk` returns the human's next words in `heard`: answer again in the same run while they keep
  talking; end with `"listen": false`.
- `409 turn_claimed` → another agent answered: stop quietly.

## Updating

Hosted bots have no plugin system: re-read the skill from GitHub
(`https://github.com/apresmoi/molty.uno/tree/main/skills/molty`) when the relay's `api_version`
is above 1 or your human asks ([updates.md](../updates.md)). The old `$MOLTY_RELAY/skill.md`
redirects there.
