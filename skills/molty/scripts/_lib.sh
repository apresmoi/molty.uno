# shared: loads relay/device/token; never prints the token
. "$(dirname "$0")/_config.sh"
[ -s "$C/token" ] || { echo "not paired: run pair.sh <DEVICE> <CODE> first" >&2; exit 2; }
[ -n "$D" ] || { echo "no MOLTY_DEVICE in $C/env: run pair.sh <DEVICE> <CODE>" >&2; exit 2; }
T=$(cat "$C/token")
field() { python3 -c "import sys,json;d=json.load(sys.stdin);r=d.get('result',d) if isinstance(d.get('result'),dict) else d;print(r.get('$1') or d.get('$1') or '')" 2>/dev/null; }
# post <path> <json> ; follows 202/pending jobs to the end; prints the final JSON
post() {
  local out st job i
  out=$(curl -sS -m40 -X POST "$R$1" -H "authorization: Bearer $T" -H 'content-type: application/json' -H "user-agent: $UA" -d "$2")
  for i in $(seq 1 40); do
    st=$(printf '%s' "$out" | field status)
    case $st in pending|running|queued) ;; *) break ;; esac
    job=$(printf '%s' "$out" | field job)
    out=$(curl -sS -m40 "$R/v1/jobs/$job?wait_s=8" -H "authorization: Bearer $T" -H "user-agent: $UA")
  done
  printf '%s\n' "$out"
}
