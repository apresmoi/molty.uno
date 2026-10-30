#!/usr/bin/env python3
"""Molty events, one line each (utterance, shake, tap, button, answer, ...).

watch.py               forever, from now on (run it under a monitor / in the background)
watch.py --once [N]    your unread events (waiting up to N s, default 0), then exit
Config as the shell helpers (_config.sh): ~/.config/molty/{env,token}. Never prints the token.
"""
import json, os, shutil, sys, time, urllib.request

DEFAULT_RELAY = "https://molty-relay.alicenet.workers.dev"
custom = os.environ.get("MOLTY_CONFIG_DIR")
C = custom or os.path.expanduser("~/.config/molty")
OLD = os.path.expanduser("~/.config/molty-claude")
if not custom and not os.path.exists(f"{C}/env") and not os.path.exists(f"{C}/token") and os.path.isdir(OLD):
    os.makedirs(C, mode=0o700, exist_ok=True)
    for f in ("env", "token"):
        if os.path.exists(f"{OLD}/{f}"):
            shutil.copy2(f"{OLD}/{f}", f"{C}/{f}")
    print(f"molty: config copied from {OLD} to {C} (the old dir can go)", file=sys.stderr)
env = {}
if os.path.exists(f"{C}/env"):
    env = dict(l.strip().split("=", 1) for l in open(f"{C}/env") if "=" in l)
R = (os.environ.get("MOLTY_RELAY") or env.get("MOLTY_RELAY") or DEFAULT_RELAY).rstrip("/")
D = os.environ.get("MOLTY_DEVICE") or env.get("MOLTY_DEVICE")
try:
    T = open(f"{C}/token").read().strip()
except OSError:
    T = ""
if not T or not D:
    sys.exit("not paired: run pair.sh <DEVICE> <CODE> first")


def get(q, timeout):
    req = urllib.request.Request(f"{R}/v1/devices/{D}/events?{q}", headers={"authorization": f"Bearer {T}", "user-agent": "molty-skill"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r)


def show(events):
    for e in events:
        t, data = e.get("type"), e.get("data") or {}
        if t == "utterance":
            print(f"MOLTY utterance: {data.get('text')!r} (reason {data.get('reason')}, turn {data.get('turn_id')})", flush=True)
        else:
            print(f"MOLTY {t}: {json.dumps(data)[:160]}", flush=True)


if sys.argv[1:2] == ["--once"]:
    wait = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    d = get(f"wait_s={max(0, min(wait, 25))}", wait + 15)
    show(d.get("events", []))
    if d.get("more"):
        print("MOLTY more unread events: run again", flush=True)
    sys.exit(0)

since = get("wait_s=0", 15).get("next", "")  # start from now: older unread events are skipped
print("MOLTY watching events", flush=True)
while True:
    try:
        d = get(f"wait_s=25&since={since}" if since else "wait_s=25", 40)
    except Exception:  # network blips: retry quietly
        time.sleep(5)
        continue
    show(d.get("events", []))
    since = d.get("next") or since
