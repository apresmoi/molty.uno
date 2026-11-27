"""molty-sim front --script (molty_sim/front_script.py): the checks over a whole conversation, offline."""

from molty_sim import front_script as fs

SCRIPT = fs.Script(name="t", turns=[
    fs.Turn(heard="hey molty", agent=[(13, "Just lounging on your desk like a proper cat, Juan.")]),
    fs.Turn(heard="it seems that", agent=[(12, "Mrrp, I am listening.")]),
    fs.Turn(heard="what is on my calendar?", front="handoff", agent=[(9, "Standup at ten.")]),
])


def trace(ts, job, **d):
    return {"ts": ts, "kind": "trace", "ev": "front", "job": job, "data": {"reply_first_audio": 450, **d}}


def reply(ts, said):
    return {"ts": ts, "kind": "job", "op": "say", "ok": True, "data": {"said": said, "timings": {"front": {"created": 200}}}}


def agent(ts, said=None):
    return {"ts": ts, "kind": "job", "op": "talk", "agent": "Dude", "ok": True, "data": {"said": said} if said else {"as": "inner"}}


def good():
    return [
        {"ts": 0, "kind": "voice_box", "ev": "front.start", "data": {}},
        {"ts": 100, "kind": "voice_box", "ev": "front.session", "data": {"state": "open"}},
        trace(1000, "j1"), reply(1100, "Hey there!"),
        trace(6000, "j2"), reply(6100, "Go on?"),
        agent(14000),  # the turn-1 greeting: context only
        trace(15000, "j3", waiting=True), reply(15100, "Ok, let me see... your calendar, one sec."),
        agent(24000, "Ok, got it: Standup at ten."),
        {"ts": 40000, "kind": "voice_box", "ev": "front.end", "data": {"reason": "idle"}},
    ]


def test_a_good_conversation_passes():
    end = {"ts": 40000, "kind": "voice_box", "ev": "front.end", "data": {"reason": "idle"}}
    rows, bad = fs.check(SCRIPT, good(), [{"ev": "open"}, {"ev": "create", "kind": "reply", "speaking": False}, {"ev": "close", "active": False}], end)
    assert bad == []
    assert [r["state"] for r in rows] == ["answered", "answered", "handed off"]


def test_run5_fails_every_way_it_went_wrong():
    e = [
        {"ts": 0, "kind": "voice_box", "ev": "front.start", "data": {}},
        trace(1000, "j1"), reply(1100, "Hey there!"),
        trace(6000, "j2", reply_first_audio=None),  # no reply
        agent(7000, "So, Just lounging on your desk like a proper cat, Juan."),  # voiced for a turn not handed off
        {"ts": 9000, "kind": "voice_box", "ev": "front.end", "data": {"reason": "idle"}},  # under turn 2's (missing) reply
        {"ts": 12000, "kind": "voice_box", "ev": "front.session", "data": {"state": "open"}},  # a session for a context line
        agent(12500, "Just lounging on your desk like a proper cat, Juan. So, Mrrp, I am listening."),  # recited
    ]
    rows, bad = fs.check(SCRIPT, e, [{"ev": "create", "kind": "inner", "speaking": True}, {"ev": "close", "active": True}], e[4])
    kinds = {b.split(":")[0] for b in bad}
    assert kinds == {"one_reply", "first_audio", "agent_waiting", "never_twice", "not_cut", "no_session", "not_over_them", "all_turns"}
    assert rows[2]["state"] == "MISSING"  # turn 3 never happened


def test_a_hung_run_fails_long_or_unhanded_replies_too():
    e = [trace(1000, "j1"), reply(1100, "Hey! I'm doing well, thanks for asking. I'm here and ready to help with anything you want."),
         trace(6000, "j2"), reply(6100, "Go on?"), trace(15000, "j3"), reply(15100, "Your calendar has three things.")]
    _, bad = fs.check(SCRIPT, e, None, None)  # never ended
    assert any(b.startswith("all_turns: the conversation never ended") for b in bad)
    assert any(b.startswith("short: turn 1") for b in bad)
    assert any(b.startswith("handoff: turn 3") for b in bad)


def test_the_script_loads_and_its_clips():
    s = fs.load(fs.Path(__file__).parents[1] / "scenarios" / "front-run5.yaml")
    assert [t.front for t in s.turns] == ["answer", "handoff", "answer", "handoff", "handoff", "answer"]
    assert s.turns[1].auto_create_ms == 400 and not s.turns[2].voiced and s.turns[-1].ends
    c = fs.clip(s.turns[0], 0)
    assert abs(c.speech[0][0] - 0.5) < 1e-6 and len(c.pcm) > 16000
    assert fs.fake_script(s)[3]["handoff"] is True


def test_a_voiced_answer_that_lost_the_agents_facts_fails():
    # live 2026-11-27 (rig run 2, turn 4): the front said "I can't see your calendar. Sorry." over the agent's answer
    e = good()
    e[-2] = agent(24000, "I can't see your calendar. Sorry.")
    e[-2]["job"] = "jx"
    e.append({"ts": 23900, "kind": "call", "op": "talk", "job": "jx", "args": {"say": "Standup at ten.", "in_reply_to": "j3"}})
    _, bad = fs.check(SCRIPT, e, None, next(x for x in e if x.get("ev") == "front.end"))
    assert [b.split(":")[0] for b in bad] == ["faithful"]


def test_a_reply_cut_or_never_ended_fails():
    log = """INFO front f1 (reply): response created
INFO front f1 (reply): response completed, said 'Hey!'
INFO front f2 (reply): response created
WARNING front f2 (reply): response incomplete (max_output_tokens), said 'Checking your'
INFO front f3 (reply): response created
"""
    assert fs.reply_ends(log) == ["reply_end: reply f2 ended incomplete (max_output_tokens)", "reply_end: reply f3 never ended (no end in the box's log)"]
    # the live run 4 log, as it was written then: "response incomplete" with no reason
    assert fs.reply_ends("INFO front f9 (reply): response created\nINFO front f9 (reply): response incomplete, said 'x'\n") == ["reply_end: reply f9 ended incomplete"]
