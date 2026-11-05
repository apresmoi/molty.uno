#!/usr/bin/env bash
# act.sh <gesture>: one of device.creature.gestures (the shared wiggle, nod, shake_head, bounce, peek, glance_left, glance_right, blink, wobble, stretch, look_up, yawn, inspect, paddle, and the creature's own, like the cat's eat)
. "$(dirname "$0")/_lib.sh"
args=$(python3 -c 'import json,sys;print(json.dumps({"name":"act","args":{"action":sys.argv[1]}}))' "$@")
post "/v1/devices/$D/cmd" "$args" | head -c 300; echo
