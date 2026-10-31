#!/usr/bin/env bash
# quick.sh <phrase id> [--reply <turn_id>]: play a cached quick phrase NOW (hmm, let_me_think, one_sec,
# let_me_see, good_question, on_it, checking, or your own); returns at once — then answer with talk.sh
. "$(dirname "$0")/_lib.sh"
args=$(python3 -c '
import json,sys
a={"id":sys.argv[1]}
if len(sys.argv)>3 and sys.argv[2]=="--reply": a["in_reply_to"]=sys.argv[3]
print(json.dumps({"name":"quick","args":a}))' "$@")
post "/v1/devices/$D/cmd" "$args" | head -c 300; echo
