#!/usr/bin/env python3
"""One line per Molty event (shake, tap, button, utterance, …), forever. Run it under Monitor."""
import json, os, sys, time, urllib.request

C = os.environ.get("MOLTY_CONFIG_DIR", os.path.expanduser("~/.config/molty-claude"))
env = dict(l.strip().split("=", 1) for l in open(f"{C}/env") if "=" in l)
R, D, T = env["MOLTY_RELAY"], env["MOLTY_DEVICE"], open(f"{C}/token").read().strip()

def get(q, timeout):
    req = urllib.request.Request(f"{R}/v1/devices/{D}/events?{q}", headers={"authorization": f"Bearer {T}", "user-agent": "molty-claude-skill"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r)

since = get("wait_s=0", 15).get("next", "")
print("MOLTY watching events", flush=True)
while True:
    try:
        d = get(f"wait_s=25&since={since}" if since else "wait_s=25", 40)
    except Exception as e:  # network blips: retry quietly
        time.sleep(5); continue
    for e in d.get("events", []):
        t, data = e.get("type"), e.get("data") or {}
        if t == "utterance":
            print(f"MOLTY utterance: {data.get('text')!r} (reason {data.get('reason')}, turn {data.get('turn_id')})", flush=True)
        elif t not in ("skill_update", "skill.updated"):
            print(f"MOLTY {t}: {json.dumps(data)[:160]}", flush=True)
    since = d.get("next") or since
