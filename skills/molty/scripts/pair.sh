#!/usr/bin/env bash
# pair.sh <DEVICE> <LINK_CODE> [agent name]  → Molty shows a code, the human taps; stores env + token (0600). Never prints the token.
# A private relay: MOLTY_RELAY=https://… pair.sh …  (stored in the env file)
. "$(dirname "$0")/_config.sh"
[ $# -ge 2 ] || { echo "usage: pair.sh <DEVICE> <LINK_CODE> [agent name]" >&2; exit 2; }
D=$1; code=$2; name=${3:-Molty agent}
j() { python3 -c "import sys,json;d=json.load(sys.stdin);d=d.get('result',d) if isinstance(d.get('result'),dict) else d;print(d.get('$1') or '')" 2>/dev/null; }
req=$(python3 -c 'import json,sys;print(json.dumps({"device_id":sys.argv[1],"link_code":sys.argv[2],"agent_name":sys.argv[3],"wait_s":0}))' "$D" "$code" "$name")
out=$(curl -sS -m40 -X POST "$R/v1/pair" -H 'content-type: application/json' -H "user-agent: $UA" -d "$req")
pid=$(printf '%s' "$out" | j pair_id); shown=$(printf '%s' "$out" | j code)
[ -n "$pid" ] || { echo "pairing failed: $(printf '%s' "$out" | j error) $(printf '%s' "$out" | j message)"; exit 1; }
echo "Molty shows $shown — tap Molty to accept"
for i in 1 2 3 4 5 6; do
  out=$(curl -sS -m40 -X POST "$R/v1/pair" -H 'content-type: application/json' -H "user-agent: $UA" -d "{\"device_id\":\"$D\",\"pair_id\":\"$pid\",\"wait_s\":25}")
  tok=$(printf '%s' "$out" | j agent_token)
  if [ -n "$tok" ]; then
    (umask 077; mkdir -p "$C"; printf '%s' "$tok" > "$C/token"; chmod 600 "$C/token"
     { echo "MOLTY_DEVICE=$D"; [ "$R" != "$MOLTY_DEFAULT_RELAY" ] && echo "MOLTY_RELAY=$R"; } > "$C/env")
    echo "paired (api_version $(printf '%s' "$out" | j api_version)); token stored in $C/token"; exit 0
  fi
  st=$(printf '%s' "$out" | j status); [ -n "$st" ] || st=$(printf '%s' "$out" | j error)
  [ "$st" = pending ] || { echo "pairing failed: $st"; exit 1; }
done
echo "timed out"; exit 1
