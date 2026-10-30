#!/usr/bin/env bash
# talk.sh "text" [--no-listen] [--reply <turn_id>]  → speaks, then (default) listens; prints HEARD/reason/timings
. "$(dirname "$0")/_lib.sh"
say=$1; shift; listen=true; reply=
while [ $# -gt 0 ]; do case $1 in --no-listen) listen=false ;; --reply) reply=$2; shift ;; esac; shift; done
body=$(python3 -c 'import json,sys;b={"say":sys.argv[1],"listen":sys.argv[2]=="true"};sys.argv[3] and b.update(in_reply_to=sys.argv[3]);print(json.dumps(b))' "$say" "$listen" "$reply")
post "/v1/devices/$D/talk" "$body" | python3 -c "
import sys,json;d=json.load(sys.stdin);r=d.get('result',d);m=r.get('ms') or {}
if not d.get('ok',True) or r.get('error'): print('ERROR', r.get('error') or d.get('error'), r.get('message') or d.get('message') or '', '| job', d.get('job') or r.get('job') or '')
else: print('HEARD:', repr(r.get('heard')), '| reason', r.get('reason'), '| tts_first', m.get('tts_first_audio'), 'stt', m.get('stt'), 'total', m.get('total'))"
