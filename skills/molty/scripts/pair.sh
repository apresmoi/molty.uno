#!/bin/zsh
# pair.sh <LINK_CODE> [agent name]  → Molty shows a code, the human taps; stores the token (0600). Never prints it.
C=${MOLTY_CONFIG_DIR:-$HOME/.config/molty-claude}; source $C/env
name=${2:-Claude Code}
out=$(curl -sS -m40 -X POST "$MOLTY_RELAY/v1/pair" -H 'content-type: application/json' -H 'user-agent: molty-claude-skill' \
  -d "$(python3 -c 'import json,sys;print(json.dumps({"device_id":sys.argv[1],"link_code":sys.argv[2],"agent_name":sys.argv[3],"wait_s":0}))' $MOLTY_DEVICE "$1" "$name")")
pid=$(print -r -- "$out" | python3 -c "import sys,json;d=json.load(sys.stdin);print(d.get('pair_id') or (d.get('result') or {}).get('pair_id') or '')")
code=$(print -r -- "$out" | python3 -c "import sys,json;d=json.load(sys.stdin);print(d.get('code') or (d.get('result') or {}).get('code') or d.get('error'))")
echo "Molty shows $code — tap Molty to accept"
for i in 1 2 3 4 5 6; do
  out=$(curl -sS -m40 -X POST "$MOLTY_RELAY/v1/pair" -H 'content-type: application/json' -H 'user-agent: molty-claude-skill' -d "{\"device_id\":\"$MOLTY_DEVICE\",\"pair_id\":\"$pid\",\"wait_s\":25}")
  tok=$(print -r -- "$out" | python3 -c "import sys,json;d=json.load(sys.stdin);d=d.get('result',d);print(d.get('agent_token') or '')")
  [[ -n $tok ]] && { umask 077; print -rn -- "$tok" > $C/token; echo "paired; token stored in $C/token"; exit 0; }
  st=$(print -r -- "$out" | python3 -c "import sys,json;d=json.load(sys.stdin);d=d.get('result',d);print(d.get('status') or d.get('error'))")
  [[ $st == pending ]] || { echo "pairing failed: $st"; exit 1; }
done
echo "timed out"; exit 1
