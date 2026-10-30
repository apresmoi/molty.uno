#!/bin/zsh
# talk.sh "text" [--no-listen]  → speaks, then (default) listens; prints HEARD/reason/timings
source ${0:A:h}/_lib.sh
listen=true; [[ $2 == --no-listen ]] && listen=false
body=$(python3 -c 'import json,sys;print(json.dumps({"say":sys.argv[1],"listen":sys.argv[2]=="true"}))' "$1" $listen)
post "/v1/devices/$D/talk" "$body" | python3 -c "
import sys,json;d=json.load(sys.stdin);r=d.get('result',d);m=r.get('ms') or {}
if not d.get('ok',True) or r.get('error'): print('ERROR', r.get('error') or d.get('error'), r.get('message') or d.get('message') or '')
else: print('HEARD:', repr(r.get('heard')), '| reason', r.get('reason'), '| tts_first', m.get('tts_first_audio'), 'stt', m.get('stt'), 'total', m.get('total'))"
