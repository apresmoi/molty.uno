#!/usr/bin/env bash
# buzz.sh [tick|tap|double|nudge] [strength 0..100]: Molty's vibration motor, once (default double; cap haptic)
. "$(dirname "$0")/_lib.sh"
args=$(python3 -c 'import json,sys;a={"pattern":sys.argv[1] if len(sys.argv)>1 else "double"};len(sys.argv)>2 and a.update(strength=int(sys.argv[2]));print(json.dumps({"name":"buzz","args":a}))' "$@")
post "/v1/devices/$D/cmd" "$args" | head -c 300; echo
