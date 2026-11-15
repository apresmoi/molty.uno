#!/usr/bin/env bash
# progress.sh <pct|-> ["step"] | progress.sh done: the ring on Molty's rim fills to pct (- = unknown: a line circles), or finishes
. "$(dirname "$0")/_lib.sh"
args=$(python3 -c '
import json,sys
a=sys.argv[1:]
if a and a[0]=="done": p={"done":True}
else:
    p={}
    if a and a[0]!="-": p["pct"]=float(a[0])
    if len(a)>1: p["step"]=a[1]
print(json.dumps({"name":"progress","args":p}))' "$@")
post "/v1/devices/$D/cmd" "$args" | head -c 300; echo
