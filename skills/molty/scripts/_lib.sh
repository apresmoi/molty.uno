# shared: loads relay/device/token; never prints the token
C=${MOLTY_CONFIG_DIR:-$HOME/.config/molty-claude}
[[ -f $C/env ]] && source $C/env
[[ -s $C/token ]] || { echo "not paired: run pair.sh <LINK_CODE> first" >&2; exit 2; }
T=$(<$C/token); R=$MOLTY_RELAY; D=$MOLTY_DEVICE
# post <path> <json> ; follows 202/pending jobs to the end; prints the final JSON
post() {
  local out st job
  out=$(curl -sS -m40 -X POST "$R$1" -H "authorization: Bearer $T" -H 'content-type: application/json' -H 'user-agent: molty-claude-skill' -d "$2")
  for i in $(seq 1 40); do
    st=$(print -r -- "$out" | python3 -c "import sys,json;d=json.load(sys.stdin);r=d.get('result',d);print(r.get('status') or d.get('status') or '')" 2>/dev/null)
    [[ $st == pending || $st == running || $st == queued ]] || break
    job=$(print -r -- "$out" | python3 -c "import sys,json;d=json.load(sys.stdin);r=d.get('result',d);print(r.get('job') or d.get('job'))")
    out=$(curl -sS -m40 "$R/v1/jobs/$job?wait_s=20" -H "authorization: Bearer $T" -H 'user-agent: molty-claude-skill')
  done
  print -r -- "$out"
}
