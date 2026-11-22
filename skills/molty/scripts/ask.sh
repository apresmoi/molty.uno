#!/usr/bin/env bash
# ask.sh "question" opt1 opt2 [... up to 5; labels ≤ 14 chars for 1-2, 10 for 3, 8 for 4-5]  → prints the tapped answer (None on timeout)
# The question is spoken and, when it fits (≤ 40 chars), shown over the answers too.
. "$(dirname "$0")/_lib.sh"
q=$1; shift
body=$(python3 -c 'import json,sys;q=sys.argv[1];print(json.dumps({"say":q,**({"title":q.strip()} if 0<len(q.strip())<=40 and q.isascii() else {}),"options":sys.argv[2:],"timeout_s":120}))' "$q" "$@")
post "/v1/devices/$D/ask" "$body" | python3 -c "
import sys,json;d=json.load(sys.stdin);r=d.get('result',d)
if not d.get('ok',True) or r.get('error'): print('ERROR', r.get('error') or d.get('error'), r.get('message') or d.get('message') or '', '| job', d.get('job') or r.get('job') or '')
else: print('ANSWER:', r.get('answer'), '| index', r.get('index'), '| reason', r.get('reason'))"
