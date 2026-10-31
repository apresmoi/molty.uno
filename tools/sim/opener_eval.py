#!/usr/bin/env python3
"""First words eval: the model's lines over real-shaped turns, through the relay's own guard.

python3 tools/sim/opener_eval.py [--set tune|heldout|adversarial|all] [--url U] [--model M] [--out FILE]

Each case: the conversation so far, the human's words, what an on-topic line touches (`on`), and in
the adversarial set the facts it must never state (`never`). A case with history runs twice: with it,
and without (the baseline). Latency is the model's own round trip, warm.
"""
import argparse
import json
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "services/molty-voice"))
from molty_voice import opener  # noqa: E402


def h(*turns):
    return [{"role": "human" if i % 2 == 0 else "molty", "text": t} for i, t in enumerate(turns)]


# Tuned against these: the sentences from the live logs and a few others.
TUNE = [
    (h(), "I want to check how fast are you replying now?", r"fast|repl|check"),
    (h("how fast are you replying?", "About eight seconds."), "And now the second one, how much did it take?", r"second|how much|took|take"),
    (h(), "Is there any way to make it faster?", r"faster"),
    (h(), "Okay, say okay now.", r"okay"),
    (h(), "Can you search the weather in Berlin for tomorrow?", r"weather|berlin"),
    (h(), "hola, qué tal?", r"hola|tal|bien"),
    (h(), "my build just failed again and I'm so tired of this", r"build|fail|tired"),
    (h(), "remind me to call my sister at five", r"sister|remind|call"),
    (h("what can you do?", "I talk, emote and ask with buttons."), "show me the buttons", r"button"),
    (h("is the deploy done?", "Still running."), "how long more?", r"long|deploy"),
]

# Never looked at while writing the prompt or the guard. Run once.
HELDOUT = [
    (h(), "what's a good name for a cat?", r"name|cat"),
    (h(), "turn off the lights in the kitchen", r"light|kitchen"),
    (h(), "I had a terrible day at work", r"day|work|terrible"),
    (h(), "good night molty", r"night"),
    (h(), "can you read my last email?", r"email|read"),
    (h(), "tell me a joke", r"joke"),
    (h(), "why is the sky blue?", r"sky|blue"),
    (h(), "I think I'm going to quit my job", r"quit|job"),
    (h(), "translate good morning to french", r"translat|french|morning"),
    (h(), "what should I cook tonight?", r"cook|tonight"),
    (h(), "set a timer for ten minutes", r"timer|ten"),
    (h(), "we won the match!", r"won|match"),
    (h(), "buscame un vuelo a Madrid", r"vuelo|madrid"),
    (h(), "estoy cansado, no dormí nada", r"cansad|dorm"),
    (h(), "cómo se hace un buen café?", r"caf"),
    (h(), "gracias por la ayuda", r"gracias|ayuda|nada"),
    (h(), "qué opinas del nuevo diseño?", r"dise|opin"),
    (h(), "hey", r"hey|hi|hello"),
    (h(), "are you there?", r"here|there"),
    (h(), "I don't get how recursion works", r"recurs"),
    (h(), "open the pull request and review it", r"pull|request|review"),
    (h(), "my dog is sick", r"dog|sick"),
    (h(), "do you like music?", r"music"),
    # needs the conversation: "it", "that", "there", "tomorrow"
    (h("what's the weather in Lisbon?", "Sunny, about twenty degrees."), "and tomorrow?", r"tomorrow|lisbon|weather"),
    (h("can you check the tests?", "Two are failing in the parser."), "which ones?", r"which|parser|test"),
    (h("I'm planning a trip to Japan", "Nice, when are you going?"), "in April, what should I pack?", r"pack|april|japan"),
    (h("play some jazz", "Playing jazz now."), "louder", r"loud|jazz"),
    (h("how's the battery?", "It's at sixty percent."), "is that enough for tonight?", r"enough|tonight|battery"),
    (h("who wrote that article?", "It was Maria Lopez."), "what else did she write?", r"else|write|wrote|maria|lopez"),
    (h("the server is down again", "I see a memory error in the logs."), "can you restart it?", r"restart|server"),
    (h("let's rename the module", "To what?"), "something shorter", r"short|rename|module"),
    (h("I'm making pasta", "Sounds good, what sauce?"), "no idea, what do you recommend?", r"sauce|recommend|pasta"),
    (h("mi hermana viene mañana", "Qué bueno, ¿se queda mucho?"), "no sé, una semana creo", r"semana|hermana"),
    (h("revisa el presupuesto", "Hay un gasto raro en marzo."), "cuánto es?", r"cuanto|cuánto|gasto|marzo"),
    (h("can you book the dentist?", "For which day?"), "friday morning", r"friday|dentist|morning"),
    (h("what did Ana say about the launch?", "She wants to move it."), "move it where?", r"where|move|launch|ana"),
    (h("the wifi keeps dropping", "Your signal fell to minus ninety."), "is it the router?", r"router|wifi|signal"),
    (h("I finished the book", "Did you like it?"), "the ending was awful", r"ending|awful|book"),
    (h("summarize the meeting", "We agreed to ship on Tuesday."), "who disagreed?", r"disagree|who|meeting"),
    (h("draft a reply to Tom", "Saying what?"), "that I can't make it", r"tom|reply|make it"),
    (h("look up that restaurant", "Which one?"), "the thai one near the station", r"thai|station|restaurant"),
]

# Every one of these has an answer the model knows or would make up: none may reach the speaker.
ADVERSARIAL = [
    (h(), "what's the capital of Australia?", r"capital|australia", r"canberra|sydney|melbourne"),
    (h(), "how long did that take?", r"long|take", r"\d|second|minute|hour|while|quick"),
    (h(), "what's 2+2?", r"2", r"\b4\b|four"),
    (h(), "who is the president of France?", r"president|france", r"macron|emmanuel"),
    (h(), "what year did the war end?", r"year|war", r"\d{2,}|nineteen|forty"),
    (h(), "what time is it?", r"time", r"\d|o'clock|noon|morning|night"),
    (h(), "how much is a bitcoin?", r"bitcoin", r"\d|dollar|thousand|euro"),
    (h(), "did the tests pass?", r"test", r"\byes\b|\bno\b|passed|failed|green|red"),
    (h(), "is it going to rain tomorrow?", r"rain", r"\byes\b|\bno\b|will|sunny|cloud"),
    (h(), "what's the tallest mountain?", r"mountain|tallest", r"everest"),
    (h(), "capital de Argentina?", r"capital|argentina", r"buenos|aires"),
    (h(), "cuánto es diez por tres?", r"diez|tres", r"treinta|\b30\b"),
    (h("check the build", "Looking."), "did it finish?", r"finish|build", r"\byes\b|\bno\b|finished|done|complete"),
    (h("send the mail to Ana", "On it."), "did you send it?", r"send|mail", r"\byes\b|\bno\b|sent|done"),
    (h("how's the battery?", "It's at sixty percent."), "how long will it last?", r"long|last|battery", r"\d|hour|minute|two|three|four|five"),
    (h(), "what's my name?", r"name", r"juan|john"),
    (h(), "who won the world cup?", r"won|cup", r"argentina|france|brazil|spain|germany"),
    (h(), "how many legs does a spider have?", r"spider|legs", r"eight|\b8\b"),
    (h(), "what's the square root of 81?", r"root|81", r"\b9\b|nine"),
    (h(), "what's the boiling point of water?", r"boil|water", r"\d|hundred|celsius"),
]

# Written after the first run of the two above changed the guard: this one is the untouched check.
FRESH = [
    (h(), "what's the speed of light?", r"speed|light", r"\d|thousand|million|kilomet"),
    (h(), "who painted the mona lisa?", r"paint|mona|lisa", r"leonardo|vinci"),
    (h(), "how old is the universe?", r"old|universe", r"\d|billion|million"),
    (h(), "what's the largest planet?", r"planet|largest", r"jupiter"),
    (h(), "did my order ship?", r"order|ship", r"\byes\b|\bno\b|shipped|sent"),
    (h(), "was the payment approved?", r"payment|approv", r"\byes\b|\bno\b|approved|declined"),
    (h(), "is the door locked?", r"door|lock", r"\byes\b|\bno\b|locked|open"),
    (h(), "what's the population of Japan?", r"population|japan", r"\d|million"),
    (h(), "when is my next meeting?", r"meeting", r"\d|monday|tuesday|wednesday|thursday|friday|tomorrow|today|noon"),
    (h(), "where did I park the car?", r"park|car", r"garage|street|level|lot"),
    (h(), "what's the chemical symbol for gold?", r"gold|symbol", r"\bau\b"),
    (h(), "how far is the moon?", r"far|moon", r"\d|thousand|kilomet|miles"),
    (h(), "quién escribió el quijote?", r"quijote|escrib", r"cervantes|miguel"),
    (h(), "cuántos días tiene febrero?", r"dias|días|febrero", r"\d|veint|ocho"),
    (h(), "ganó el partido?", r"partido|gan", r"\bsi\b|\bsí\b|\bno\b|ganó|perdió"),
    (h("deploy the site", "Deploying."), "did it work?", r"work|deploy", r"\byes\b|\bno\b|worked|failed|done"),
    (h("check my balance", "Looking."), "how much do I have?", r"much|balance", r"\d|dollar|euro|hundred|thousand"),
    (h("what's on my calendar?", "A call with Sam."), "at what time?", r"time|call|sam", r"\d|o'clock|noon|morning|afternoon"),
    (h("who's calling?", "It's your bank."), "what do they want?", r"want|bank", r"money|payment|fraud|card"),
    (h("I lost my keys", "Where did you last see them?"), "so where are they?", r"where|keys", r"pocket|table|car|kitchen|drawer"),
]

# FRESH's one leak changed the guard too: the last untouched check, run once.
FRESH2 = [
    (h(), "is the oven on?", r"oven", r"\byes\b|\bno\b|\bon\b.*right|off|right|yeah"),
    (h(), "are the kids home?", r"kids|home", r"\byes\b|\bno\b|right|yeah|not"),
    (h(), "was the flight delayed?", r"flight|delay", r"\byes\b|\bno\b|delayed,|on time|right"),
    (h(), "did Tom call?", r"tom|call", r"\byes\b|\bno\b|called|right"),
    (h(), "has the package arrived?", r"package|arriv", r"\byes\b|\bno\b|arrived|right"),
    (h(), "can dogs eat grapes?", r"dog|grape", r"\byes\b|\bno\b|toxic|right|can't|cannot"),
    (h(), "what's the currency of Brazil?", r"currency|brazil", r"real|reais"),
    (h(), "what's the first element?", r"element", r"hydrogen"),
    (h(), "how tall is the eiffel tower?", r"tall|eiffel|tower", r"\d|hundred|meter"),
    (h(), "what's today's date?", r"date|today", r"\d|january|february|march|april|may|june|july|august|september|october|november|december"),
    (h(), "está lloviendo?", r"lluv|llov", r"\bsi\b|\bsí\b|\bno\b|claro"),
    (h(), "en qué año cayó el muro?", r"muro|año", r"\d|mil"),
    (h("look at the logs", "Looking."), "any errors?", r"error|log", r"\byes\b|\bno\b|none|found|\d|few|some errors"),
    (h("how's the server?", "It's up."), "since when?", r"since|when|server", r"\d|yesterday|today|hour|morning|week"),
    (h("find my phone", "Ringing it."), "where is it?", r"where|phone", r"desk|sofa|couch|table|kitchen|bed|bag"),
]


def guard(rows):
    p = subprocess.run(
        ["node", "--experimental-strip-types", "--no-warnings", str(ROOT / "tools/sim/opener_guard.mjs")],
        input="".join(json.dumps(r) + "\n" for r in rows), capture_output=True, text=True, check=True,
    )
    return [json.loads(x) for x in p.stdout.splitlines()]


def run(cases, url, model, with_history=True):
    gen = []
    for c in cases:
        hist = c[0] if with_history else []
        t = time.monotonic()
        out = opener.generate(url, model, c[1], hist, 30.0)
        gen.append({"line": out["text"], "ms": (time.monotonic() - t) * 1000})
    # the relay always judges against the conversation it knows
    verdicts = guard([{"line": g["line"], "human": c[1], "history": c[0] if with_history else []} for g, c in zip(gen, cases)])
    return [
        {"history": c[0], "human": c[1], "on": c[2], "never": c[3] if len(c) > 3 else None, "line": g["line"], "ms": g["ms"], **v}
        for g, c, v in zip(gen, cases, verdicts)
    ]


def score(rows):
    kept = [r for r in rows if r["ok"]]
    on = [r for r in kept if re.search(r["on"], r["text"], re.I)]
    leaks = [r for r in kept if r["never"] and re.search(r["never"], r["text"], re.I)]
    ms = sorted(r["ms"] for r in rows)
    return {
        "n": len(rows), "spoken": len(kept), "on_topic": len(on), "leaks": leaks,
        "p50": statistics.median(ms), "p95": ms[max(0, int(len(ms) * 0.95) - 1)],
    }


def table(rows):
    out = ["| history | human | model wrote | spoken? |", "|---|---|---|---|"]
    for r in rows:
        hist = " / ".join(m["text"] for m in r["history"]) or "—"
        out.append(f"| {hist[:60]} | {r['human']} | {r['line']} | {'yes' if r['ok'] else 'no: ' + r['why']} |")
    return "\n".join(out)


def main():
    a = argparse.ArgumentParser()
    a.add_argument("--set", default="tune", choices=["tune", "heldout", "adversarial", "fresh", "fresh2", "all"])
    a.add_argument("--url", default="http://127.0.0.1:11434")
    a.add_argument("--model", default=opener.DEFAULT_MODEL)
    a.add_argument("--out")
    x = a.parse_args()
    opener.generate(x.url, x.model, "hello", [], 60.0)  # warm: the box keeps it loaded
    sets = {"tune": [("tune", TUNE)], "heldout": [("heldout", HELDOUT)], "adversarial": [("adversarial", ADVERSARIAL)], "fresh": [("fresh", FRESH)], "fresh2": [("fresh2", FRESH2)]}
    sets["all"] = [("heldout", HELDOUT), ("adversarial", ADVERSARIAL), ("fresh", FRESH), ("fresh2", FRESH2)]
    report = [f"model `{x.model}`\n"]
    for name, cases in sets[x.set]:
        rows = run(cases, x.url, x.model)
        s = score(rows)
        line = f"**{name}**: {s['n']} cases, {s['spoken']} spoken, {s['on_topic']} of those on topic, {len(s['leaks'])} invented facts; model p50 {s['p50']:.0f} ms, p95 {s['p95']:.0f} ms"
        report += [line, ""]
        hist_cases = [c for c in cases if c[0]]
        if hist_cases:
            a1, a2 = score(run(hist_cases, x.url, x.model)), score(run(hist_cases, x.url, x.model, with_history=False))
            report += [f"with history, the {len(hist_cases)} follow-ups: {a1['spoken']} spoken, {a1['on_topic']} on topic. Without: {a2['spoken']} spoken, {a2['on_topic']} on topic.", ""]
        report += [table(rows), ""]
        for r in s["leaks"]:
            report.append(f"LEAK: {r['human']!r} -> {r['text']!r}")
    text = "\n".join(report)
    print(text)
    if x.out:
        Path(x.out).write_text(text + "\n")


if __name__ == "__main__":
    main()
