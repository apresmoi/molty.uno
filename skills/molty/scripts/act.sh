#!/usr/bin/env bash
# act.sh <wiggle|nod|shake_head|bounce|peek|glance_left|glance_right|blink|wobble|stretch|look_up|yawn>
. "$(dirname "$0")/_lib.sh"
args=$(python3 -c 'import json,sys;print(json.dumps({"name":"act","args":{"action":sys.argv[1]}}))' "$@")
post "/v1/devices/$D/cmd" "$args" | head -c 300; echo
