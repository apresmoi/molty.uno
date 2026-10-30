#!/bin/zsh
# ask.sh "question" opt1 opt2 [... up to 5, labels ≤ 8 chars]  → prints the tapped answer (or null on timeout)
source ${0:A:h}/_lib.sh
q=$1; shift
body=$(python3 -c 'import json,sys;print(json.dumps({"say":sys.argv[1],"options":sys.argv[2:],"timeout_s":120}))' "$q" "$@")
post "/v1/devices/$D/ask" "$body" | python3 -c "
import sys,json;d=json.load(sys.stdin);r=d.get('result',d)
if not d.get('ok',True) or r.get('error'): print('ERROR', r.get('error') or d.get('error'), r.get('message') or d.get('message') or '')
else: print('ANSWER:', r.get('answer'), '| index', r.get('index'), '| reason', r.get('reason'))"
