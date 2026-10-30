#!/bin/zsh
# mood.sh <idle|listening|thinking|speaking|happy|sleepy|surprised> [hold_s]
source ${0:A:h}/_lib.sh
args=$(python3 -c 'import json,sys;a={"mood":sys.argv[1]};len(sys.argv)>2 and a.update(hold_s=int(sys.argv[2]));print(json.dumps({"name":"mood","args":a,"wait":True}))' "$@")
post "/v1/devices/$D/cmd" "$args" | head -c 200; echo
