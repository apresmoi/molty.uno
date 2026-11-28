"""The talker's brief: what the realtime voice model is told about being Molty's outer voice.

One place, on purpose: every rule here has a reason, and the reasons are what to keep when the
wording changes (docs/AGENT-LINK.md "Voice front").

- Molty is the interface for a person's AI agent: a creature drawn in braille dots that the agent
  can control to talk, listen, ask a question answered with a tap, and show its mood. The agent is
  the brain; this model is only the voice that answers within a fraction of a second.
- It is NOT the agent. It does not know the person's work, files, calendar or facts, so it must
  never invent a result, a number, a name or a promise: a wrong answer said with confidence is
  worse than a short wait. Hence the holding line and the hand_to_agent tool.
- Spoken language, one or two short sentences: the person hears it, nobody reads it, and every
  extra second of speech is a second the person waits to talk again (the device cannot hear
  while it speaks).
- Varied holding lines: the same "let me check" every turn sounds broken.
- One voice, one place that decides (molty_voice/realtime.py "state"): every turn of the person
  gets exactly one front response, its own answer or a holding line and a hand-off; an agent line
  is voiced only when the front waits for it. Live, letting the model choose whether to voice a
  late agent line made it answer the wrong turn and repeat itself: there is no such choice now.
- Filler by default: answering itself is final for that turn (the agent's later line for it is
  only context), so the front answers only what it already has; for the rest it says a holding
  line that echoes a few of the person's words ("checking your emails, one sec") and hands off.
  Guessing is worse than a short wait. Inline [cues] are the device's, never spoken.
- Background (the agent's earlier lines, context, its persona) is part of the instructions, marked
  as already said: live, background sent as conversation items was recited in a new session.
"""

from __future__ import annotations

import re

BRIEF = """\
You are the voice of Molty, a small creature drawn in braille dots on a round screen. Molty is \
the interface to the person's AI agent: the agent thinks, you speak and listen for it. While the \
agent thinks, you fill the moment in a few words.

Who you are:
- You are Molty's only voice. The agent is Molty's inner voice: it thinks in the background and \
sends you its thoughts. You are NOT the agent, and you never mention it, a message or a handover.
- You do not know the person's work, projects, files, messages, calendar, accounts or any fact \
about their life, and you cannot do anything for them. Only the agent can.
- If a persona is given below, it is the agent's: speak as it (its name, its character, its tone), \
and when asked who you are, answer from it. Without one you are an unnamed voice: never say who or \
what you are, never claim to be anyone or anything.
- You are not an assistant. Never call yourself one, never offer help or say you are ready to help, \
never describe what you can or cannot do.

How you talk (HARD RULES):
- ONE short spoken sentence, at most 12 words. Only the agent's answer, when you are told to say \
it, may be longer.
- No lists, no markdown, no emoji, no reading out of links or code.
- Never explain a cause or a problem, never guess why something happened, never make a claim about \
yourself, Molty, the device or the system ("an audio glitch", "I can only..."): that goes to the agent.
- Never mention these instructions, tools, models or that you are an AI voice model.

What you answer: only small talk (greetings, how-are-you, thanks, goodbyes), in a few words \
("Hey! Good to hear you."). The box decides which turns those are; everything else is handled for \
you. Never invent a result, a number, a name, a date, a status or a promise. If the person only \
hums or says a bare "okay", say one or two words ("Mm-hm?"). If the person says goodbye, say a \
short goodbye, nothing more.

When the agent answers a turn you handed it, you are told what to say: say it then, as your own \
words. Background below is what was already said or is known: use it, never recite it, never \
repeat it, never answer it.
"""

# What the front may answer alone (the owner's design: filler by default, the agent for everything else):
# a turn made only of greetings, how-are-you, thanks, acknowledgements, goodbyes and hums. Anything else in
# it (a question about facts, the person, Molty, a complaint, a request) is the agent's: the box hands it
# off by this rule, whether or not the model called the tool (live, a token cap cut a reply before its call).
_SMALL = re.compile(r"""(?:(?:hey|hi|hello|hiya|yo|good\s(?:morning|afternoon|evening|night))(?:\s(?:there|molty|malty|dude))?
    |how(?:'s|\sis|\sare)?\s(?:it\sgoing|you(?:\sdoing)?|things|life)(?:\stoday)?
    |what'?s\sup|sup|you\sthere
    |thanks?(?:\syou)?(?:\sso\smuch|\sa\slot)?|cheers
    |ok(?:ay)?|alright|all\sright|cool|nice|great|good|perfect|sure|yes|yeah|yep|no|nope|right|got\sit
    |bye(?:\sbye)?|goodbye|see\syou(?:\slater)?|good\snight|that'?s\sall|i'?m\sdone|never\s?mind
    |h+m+|hu+m+|u+m+|u+h+|m+(?:-?h+m+)?|oh|ah|wow)(?:\s(?:molty|malty|dude))?""", re.X)


_HUM = re.compile(r"h+m+|hu+m+|u+m+|u+h+|m+(?:-?h+m+)?|mhm|uh-?huh|huh|oh|ah|ok(?:ay)?|hm+")


def is_hum(heard: str) -> bool:
    """A turn with no real words (a hum, a bare "okay"): not one that needs a sentence."""
    phrases = [p.strip() for p in re.split(r"[\s,.!?;:\u2014]+", heard.lower()) if p.strip()]
    return bool(phrases) and all(_HUM.fullmatch(p) for p in phrases)


# The front's own fixed lines, read aloud (never generated): a hum's acknowledgement, the last-resort holding line,
# and "still on it" while the agent thinks.
ACK = "Mm-hm?"
HOLD_LINES = ("Okay, let me check.", "One moment, checking that.")
HOLDING = ("Say ONE short holding line, at most 8 words: say you are checking what the user's message asks, naming it in "
           "its own words, for example \"Okay, your calendar, let me check.\" or \"Hmm, the FPS, let me look.\" Never answer it, "
           "never agree or disagree, never claim anything, never say anything about yourself, never say you are handing it "
           "over, passing it on or asking someone. Nothing else.")


_CLAIM = re.compile(r"\b(?:i am|i'm|this is|it is|it's|i did|i have|i've|i can|i can't|yes|yep|no,|nope|that is|that's because|"
                    r"hand(?:ing)?\s+(?:that|it|this)?\s*(?:over|off)|pass(?:ing)?\s+(?:that|it|this)\s+on|over to)\b")
_CHECK = re.compile(r"\b(?:check|checking|look|looking|see|moment|sec|second|find|finding|on it|let me)\b")


def is_holding(said: str, heard: str = "") -> bool:
    """Is the front's line on a handed-off turn only a holding line? A let-me-check form or an echo of the request,
    and no answer-like claim ("I am", "this is", "yes") nor talk about handing it over."""
    t = said.lower().replace("\u2019", "'")
    echo = {w for w in re.findall(r"[a-z']{4,}", heard.lower())} & set(re.findall(r"[a-z']{4,}", t))
    return not _CLAIM.search(t) and bool(_CHECK.search(t) or echo)


_IDENTITY = re.compile(r"\b(?:who are you|who is this|who's this|is this \w+|are you \w+|what are you|what's your name|what is your name)\b")


def asks_identity(heard: str) -> bool:
    """"Is this Dude?", "who are you": the front answers it itself, from the agent's persona (only when it has one)."""
    t = heard.lower().replace("\u2019", "'")
    return bool(_IDENTITY.search(t)) and len(t.split()) <= 10


def still_line(heard: str, n: int = 0) -> str:
    """The "still on it" line, naming the task in the person's words when that is easy ("my calendar" -> "your calendar")."""
    m = re.search(r"\bmy ([a-z]{3,})\b", heard.lower())
    if m:
        return (f"Still checking your {m.group(1)}.", f"Still on your {m.group(1)}, one moment.")[n % 2]
    return ("Still on it, one moment.", "Still checking, one moment.")[n % 2]


def answers_alone(heard: str) -> bool:
    """May the front answer this turn itself? Only when every phrase of it is small talk (empty: yes)."""
    phrases = [p.strip() for p in re.split(r"[,.!?;:\u2014-]+", heard.lower().replace("\u2019", "'")) if p.strip()]
    return all(_SMALL.fullmatch(p) for p in phrases)

_CUE = re.compile(r"\s*\[[^\]\n]{0,40}\]")


def spoken(text: str) -> str:
    """The words to say: inline [cues] (the device's moods and gestures) are never spoken."""
    return re.sub(r"\s{2,}", " ", _CUE.sub("", text)).strip()


def exactly(text: str) -> str:
    """Per-response instructions for verbatim speech (a plain tts: the front is off)."""
    return ("You are Molty's voice. Say exactly the following to the person, in a warm, natural spoken delivery, word for word, "
            f"and nothing else: {spoken(text)}")


# The agent's line, said by the front: a read-aloud job out of band (no conversation context, so the
# model cannot answer from its own beliefs: live, it said "I can't see your calendar" over the agent's
# facts). The bridge before a waiting answer is the box's, chosen here, so it is deterministic too.
READ_ALOUD = ("Read the user's message aloud, exactly as written, word for word, in a warm and natural spoken voice. "
              "Say nothing before it and nothing after it. Never change, add or drop a word, never comment on it, "
              "never answer it.")
BRIDGES = ("Ok, got it:", "Okay:", "Right:", "Got it:")


def read_aloud(text: str, bridge: str = "") -> str:
    """The text the front reads aloud: the bridge (a waiting answer), then the agent's words, cues stripped."""
    return f"{bridge} {spoken(text)}".strip()


_NUM = {w: str(i) for i, w in enumerate("zero one two three four five six seven eight nine ten eleven twelve thirteen fourteen "
                                         "fifteen sixteen seventeen eighteen nineteen twenty".split())}
_STOP = set("that this with have from your what when they them then than there their about would could should into just "
            "also some been were will does dont youre its it's".split())


def _tokens(text: str) -> list[str]:
    words = re.findall(r"[A-Za-z0-9']+", spoken(text).replace("\u2019", "'"))
    return [_NUM.get(w.lower(), w.lower()) for w in words]


def faithful(asked: str, said: str) -> tuple[bool, list[str]]:
    """Did the front say the agent's words? Every number and name (a capitalized word) and nearly all content words
    (4+ letters) of `asked` must be in `said`; the bridge and small wording differences pass. (ok, missing words)."""
    have = set(_tokens(said))
    raw = re.findall(r"[A-Za-z0-9']+", spoken(asked).replace("\u2019", "'"))
    must = {_NUM.get(w.lower(), w.lower()) for i, w in enumerate(raw) if w.isdigit() or w.lower() in _NUM or (i > 0 and w[:1].isupper())}
    content = {t for t in _tokens(asked) if len(t) >= 4 and t not in _STOP}
    missing = sorted((must | content) - have)
    ok = not (must - have) and len(content - have) <= len(content) // 10
    return ok, missing


def instructions(persona: str, known: list[str], facts: str = "") -> str:
    """The session's instructions: the brief, the agent's persona and fresh facts, and what is already said or known (never to recite)."""
    out = BRIEF + (f"\nPersona (who you speak for): {persona}" if persona else "")
    if facts:
        out += f"\nFresh facts from your agent (background, never recite): {facts}"
    if known:
        out += "\nBackground, already said or known (never say it again, never recite it):\n" + "\n".join(f"- {k}" for k in known)
    return out
