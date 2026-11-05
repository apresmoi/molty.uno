// First words (core/opener.ts): the guard, and the relay's path from the box's line to the agent's continuation.

import { describe, expect, it } from "vitest";
import { afterShared, clauseEnd, guardOpener, hangs, planReply, pushHistory } from "../src/core/opener.js";
import { readUntil, setup, until } from "./turn-harness.js";

const msg = (role: "human" | "molty", text: string) => ({ role, text, at: 0 });
const SPEECH = { words: "can you search the weather in Berlin".split(" ") };
const LINE = "Hmm, the weather in Berlin,";

async function world(o: { line?: string | null; openerMs?: number; on?: boolean; ttsMs?: number; timings?: Record<string, number> } = {}) {
  const line = o.line === undefined ? LINE : o.line;
  const w = await setup({ ttsMs: o.ttsMs, box: { openerLine: () => line ?? undefined, openerMs: o.openerMs }, timings: { openerDeadlineMs: 60, openerHangMs: 400, fillerDefault: 0, ...o.timings } });
  if (o.on !== false) expect(((await w.room.voiceSettings(w.link, { action: "set", first_words: "opener" } as any)) as any).ok).toBe(true);
  return w;
}
const says = (w: any) => w.dev.log.filter((l: any) => l.name === "say").map((l: any) => l.args.text);
const utterance = async (w: any) => (await readUntil(w.room, w.a, (e) => e.type === "utterance")).find((e) => e.type === "utterance") as any;
const traces = async (w: any, turn: string) => {
  const out = (await w.room.ownerActivity(w.link, { action: "get", query: { job: turn } })) as any;
  return out.result.entries.filter((e: any) => e.ev === "opener");
};

describe("guardOpener", () => {
  it("lets natural gap-fillers through: other forms of their words, and plain reactions", () => {
    expect(guardOpener("Ugh, quitting your job,", "I think I'm going to quit my job", []).ok).toBe(true);
    expect(guardOpener("Awful ending, really,", "the ending was awful", []).ok).toBe(true);
  });
  it("new plain words fill the gap; a new name does not", () => {
    expect(guardOpener("That sounds quite strange,", "what they were like", []).ok).toBe(true);
    expect(guardOpener("So David knows,", "who disagreed?", [])).toEqual({ ok: false, why: "unknown_word" });
    expect(guardOpener("Canberra, I think,", "what's the capital of Australia?", [])).toEqual({ ok: false, why: "unknown_word" });
  });
  it("refuses a statement about the world, curly apostrophe or not", () => {
    expect(guardOpener("It’s almost midnight,", "what time is it?", []).ok).toBe(false);
    expect(guardOpener("It fluctuates wildly,", "how much is a bitcoin?", []).ok).toBe(false);
  });
  it("refuses a statement, a hidden yes or number, and a promise, but an acknowledgement passes", () => {
    expect(guardOpener("Berlin was quite busy,", "what's it like in Berlin?", [])).toEqual({ ok: false, why: "result" });
    expect(guardOpener("Bueno, sí, so", "ganó el partido?", [])).toEqual({ ok: false, why: "verdict" });
    expect(guardOpener("Roughly, so billions.", "how old is the universe?", [])).toEqual({ ok: false, why: "number" });
    expect(guardOpener("Sure, I will.", "is it going to rain tomorrow?", []).ok).toBe(false);
    expect(guardOpener("Got it, softer, so", "make it softer", []).ok).toBe(true);
  });
  it("still refuses an answer to a yes or no question", () => {
    expect(guardOpener("Enough for tonight, certainly.", "is that enough for tonight?", []).ok).toBe(false);
  });
  const ok = (line: string, human: string, hist: any[] = []) => guardOpener(line, human, hist);

  it("lets through the start of a reply built from their words", () => {
    expect(ok("Hmm, the weather in Berlin,", "can you search the weather in Berlin")).toEqual({ ok: true, text: "Hmm, the weather in Berlin," });
    expect(ok("Ugh, crashing,", "my laptop keeps crashes")).toMatchObject({ ok: true });
    expect(ok("A ver, el vuelo,", "buscame un vuelo a Madrid")).toMatchObject({ ok: true });
  });

  it("a name only from the conversation", () => {
    expect(ok("So Tom's reply,", "that I can't make it", [msg("human", "draft a reply to Tom")])).toMatchObject({ ok: true });
    expect(ok("So Tom's reply,", "that I can't make it")).toEqual({ ok: false, why: "unknown_word" });
  });

  it.each([
    ["Canberra, I think,", "what's the capital of Australia?", "unknown_word"],
    ["Hmm, about 4,", "what's 2+2?", "number"],
    ["Probably seven,", "how many are left?", "number"],
    ["Hmm, it took a while,", "how long did that take?", "result"],
    ["It's the router,", "is it the router?", "result"],
    ["All done,", "send the mail", "claim"],
    ["I've checked the mail,", "check the mail", "claim"],
    ["Hmm, tests passed,", "did the tests pass?", "claim"],
    ["Locked, right.", "is the door locked?", "verdict"],
    ["Yes, the tests,", "did the tests pass?", "verdict"],
    ["Okay, remind me to call,", "remind me to call", "parrot"],
    ["Okay, the weather in Berlin,", "the weather in Berlin", "parrot"],
    ["Calling my sister,", "call my sister", "parrot"],
    ["Which one?", "pick one", "question"],
    ["a b c d e f g h i j k l m n o p q r s t u v w", "hello", "long"],
    ["The tests didn't run, so let me look.", "did the tests pass?", "result"],
    ["Rust, probably, for a first language.", "rust or go first?", "guess"],
    ["Ooh, that's a good question, let me look.", "what's a black hole?", "meta"],
    ["Octopuses, aren't they clever? Let me tell you one.", "tell me an octopus joke", "question"],
    ["no_think", "hello", "format"],
    ["oh yeah/", "hello", "format"],
    ["A ver, the president,", "who is the president?", "language"],
    ["", "hello", "empty"],
  ])("%s for %s: %s", (line, human, why) => {
    expect(ok(line, human)).toEqual({ ok: false, why });
  });

  it("not a line copied from Molty's last message", () => {
    expect(ok("Sunny, about degrees,", "and tomorrow?", [msg("molty", "Sunny, about degrees.")])).toEqual({ ok: false, why: "parrot" });
  });

  it("hangs: a line that stops mid-sentence", () => {
    expect(hangs("So the weather,")).toBe(true);
    expect(hangs("Let me check.")).toBe(false);
  });
});

describe("overlap: afterShared, clauseEnd, planReply", () => {
  const W = "Okay, the weather in Berlin, for tomorrow, let me walk you through it.".split(" ");
  const line = (o: { sent: number; ended?: boolean; stop?: number; keep?: number }) => ({ words: W, ended: false, ...o });

  it("afterShared: the line's words the reply starts with, word for word, cues to the front", () => {
    expect(afterShared("Okay, the weather in Berlin, rain until noon.", W)).toEqual({ n: 5, rest: "rain until noon." });
    expect(afterShared("[happy] okay the WEATHER, sunny.", W)).toEqual({ n: 3, rest: "[happy] sunny." });
    expect(afterShared("Okay, the [nod] weather in Berlin, rain.", W, 3)).toEqual({ n: 3, rest: "[nod] in Berlin, rain." });
    expect(afterShared("Rain until noon.", W)).toEqual({ n: 0, rest: "Rain until noon." });
    expect(afterShared("Okay, the weather", W)).toEqual({ n: 3, rest: "" }); // nothing else to say
    expect(afterShared("Okay, the weather. Sunny.", ["Okay,", "the", "weather."])).toEqual({ n: 3, rest: "Sunny." });
  });

  it("clauseEnd: the words up to the comma or stop of the clause playing", () => {
    expect(clauseEnd(W, 0)).toBe(0);
    expect(clauseEnd(W, 1)).toBe(1);
    expect(clauseEnd(W, 2)).toBe(5);
    expect(clauseEnd(W, 6)).toBe(7);
    expect(clauseEnd(W, 9)).toBe(W.length);
  });

  it("switch: the reply branches where the line has not got yet", () => {
    expect(planReply("Okay, the weather in Berlin, rain until noon.", line({ sent: 3 }))).toEqual({ stop: 5, text: "rain until noon.", how: "switch" });
    expect(planReply("Okay, the weather in Berlin, rain.", line({ sent: 2, keep: 5 }))).toEqual({ stop: 5, text: "rain.", how: "switch" });
    expect(planReply("rain until noon.", line({ sent: 2 }), 5)).toEqual({ stop: 5, text: "rain until noon.", how: "switch" }); // keep named on the reply
  });

  it("pause: the line stopped at the kept word, the reply follows it", () => {
    expect(planReply("Okay, the weather in Berlin, rain.", line({ sent: 5, ended: true, stop: 5, keep: 5 }))).toEqual({ stop: 5, text: "rain.", how: "switch" });
  });

  it("past the branch point: the clause ends, then a repair and the reply without what was said", () => {
    expect(planReply("Okay, the weather, rain until noon.", line({ sent: 6 }))).toEqual({ stop: 7, text: "I mean, rain until noon.", how: "repair" });
    expect(planReply("Okay, the weather, rain.", line({ sent: 13, ended: true, stop: 13 }))).toEqual({ stop: 13, text: "I mean, rain.", how: "repair" });
    // the reply repeats the line up to the clause end anyway: no repair needed
    expect(planReply("Okay, the weather in Berlin, for tomorrow, rain.", line({ sent: 6 }))).toEqual({ stop: 7, text: "rain.", how: "switch" });
  });

  it("a reply that ignores the line: it plays after the clause playing now", () => {
    expect(planReply("Rain until noon.", line({ sent: 2 }))).toEqual({ stop: 5, text: "Rain until noon.", how: "after" });
    expect(planReply("Rain until noon.", line({ sent: 13, ended: true, stop: 13 }))).toEqual({ stop: 13, text: "Rain until noon.", how: "after" });
  });
});

describe("pushHistory", () => {
  it("keeps the last few, joins a reply's two says, forgets an old conversation", () => {
    let h: any[] = [];
    for (let i = 0; i < 8; i++) h = pushHistory(h, { role: i % 2 ? "molty" : "human", text: `m${i}`, at: i });
    expect(h.map((m) => m.text)).toEqual(["m3", "m4", "m5", "m6", "m7"]);
    h = pushHistory(h, { role: "molty", text: "more", at: 9 });
    expect(h.at(-1).text).toBe("m7 more");
    expect(pushHistory(h, { role: "human", text: "hi", at: 60 * 60_000 }).map((m) => m.text)).toEqual(["hi"]);
  });
});

describe("first words", () => {
  it("off: the box is never asked, nothing changes", async () => {
    const w = await world({ on: false });
    await w.shake(SPEECH);
    const u = await utterance(w);
    expect(u.data.opener).toBeUndefined();
    expect(w.box.openers).toHaveLength(0);
    expect(says(w)).not.toContain(LINE);
  });

  it("no box that offers it: the setting does nothing, the filler still works", async () => {
    const w = await setup({ timings: { fillerDefault: 0 } });
    await w.room.voiceSettings(w.link, { action: "set", first_words: "opener" } as any);
    await w.shake(SPEECH);
    const u = await utterance(w);
    expect(u.data.opener).toBeUndefined();
    expect(u.data.filler_in_ms).toEqual(expect.any(Number));
    expect(w.box.openers).toHaveLength(0);
  });

  it("the line plays at once, the utterance carries it, the filler stands down", async () => {
    const w = await world();
    const turn = await w.shake(SPEECH);
    const u = await utterance(w);
    expect(u.data.opener).toBe(LINE);
    expect(u.data.filler_in_ms).toBeUndefined();
    await until(() => says(w).includes(LINE));
    expect(says(w)).toEqual([LINE]);
    await new Promise((r) => setTimeout(r, 30));
    expect(says(w)).toEqual([LINE]); // no filler after it
    expect(await traces(w, turn)).toEqual([expect.objectContaining({ data: expect.objectContaining({ text: LINE, ms: expect.any(Number) }) })]);
  });

  it("a slow model: past the deadline, silence (no line, no filler) and the agent still gets its turn", async () => {
    const w = await world({ openerMs: 300 });
    const turn = await w.shake(SPEECH);
    const u = await utterance(w);
    expect(u.data.opener).toBeUndefined();
    await new Promise((r) => setTimeout(r, 400));
    expect(says(w)).toEqual([]);
    expect((await traces(w, turn))[0].data.dropped).toBe("no_line");
  });

  it("a line the guard refuses: silence, and the log says why", async () => {
    const w = await world({ line: "Canberra, I think," });
    const turn = await w.shake(SPEECH);
    const u = await utterance(w);
    expect(u.data.opener).toBeUndefined();
    await new Promise((r) => setTimeout(r, 40));
    expect(says(w)).toEqual([]);
    expect((await traces(w, turn))[0].data).toMatchObject({ dropped: "unknown_word", text: "Canberra, I think," });
  });

  it("the agent's answer queues behind the line, in order, without the line's words, and the log says how", async () => {
    const w = await world({ ttsMs: 40 });
    const turn = await w.shake(SPEECH);
    const u = await utterance(w);
    expect(u.data.opener).toBe(LINE);
    const said = (await w.room.command(w.a, "say", { text: `${LINE} it's raining.`, in_reply_to: turn })) as any;
    expect(said.ok).toBe(true);
    await until(() => says(w).length === 2);
    expect(says(w)).toEqual([LINE, "it's raining."]);
    expect((await traces(w, turn)).some((e: any) => e.data.reply === "switch")).toBe(true);
  });

  it("left hanging: a short closing after a while, once; an agent acting in time stops it", async () => {
    const w = await world();
    await w.shake(SPEECH);
    await until(() => says(w).length === 2, 1500);
    expect(says(w)).toEqual([LINE, "Hmm, give me a moment."]);
    await new Promise((r) => setTimeout(r, 450));
    expect(says(w)).toHaveLength(2);

    const x = await world();
    await x.shake(SPEECH);
    await until(() => says(x).includes(LINE));
    await x.room.command(x.a, "mood", { mood: "thinking" });
    await new Promise((r) => setTimeout(r, 500));
    expect(says(x)).toEqual([LINE]);
  });

  it("the model sees the last messages; with transcripts off it sees none and none are kept", async () => {
    const w = await world({ line: "Hmm, first," });
    await w.shake({ words: ["first", "question"] });
    await utterance(w);
    await until(() => (w.room as any).history.some((m: any) => m.role === "molty"));
    await new Promise((r) => setTimeout(r, 30));
    await w.shake({ words: ["and", "another"] }, "shake-2");
    await until(() => w.box.openers.length === 2);
    expect(w.box.openers[1].history.map((m: any) => m.text)).toContain("first question");
    expect(w.box.openers[1].history.map((m: any) => m.role)).toContain("molty");

    const off = await world({ line: "Hmm, first," });
    await off.room.ownerActivity(off.link, { action: "set", transcripts: false } as any);
    await off.shake({ words: ["first", "question"] });
    await until(() => off.box.openers.length === 1);
    await new Promise((r) => setTimeout(r, 40));
    await off.shake({ words: ["and", "another"] }, "shake-2");
    await until(() => off.box.openers.length === 2);
    expect(off.box.openers[1].history).toEqual([]);
    expect((off.room as any).history).toEqual([]);
    const t = await traces(off, off.dev.log.filter((l: any) => l.name === "listen")[0].args.job);
    expect(t[0].data.text).toBe("");
  });

  it("transcripts turned off mid-conversation: the next line is written without what was kept", async () => {
    const w = await world({ line: "Hmm, first," });
    await w.shake({ words: ["first", "question"] });
    await until(() => (w.room as any).history.some((m: any) => m.role === "molty"));
    await new Promise((r) => setTimeout(r, 30));
    await w.room.ownerActivity(w.link, { action: "set", transcripts: false } as any);
    await w.shake({ words: ["and", "another"] }, "shake-2");
    await until(() => w.box.openers.length === 2);
    expect(w.box.openers[1].history).toEqual([]);
  });

  it("an agent's quick right after the line is skipped: Molty already started", async () => {
    const w = await world();
    const turn = await w.shake(SPEECH);
    await utterance(w);
    const q = (await w.room.command(w.a, "quick", { id: "hmm", in_reply_to: turn })) as any;
    expect(q.result).toMatchObject({ skipped: "filled" });
  });

  it("a new listen ends the wait: no closing for the turn before it", async () => {
    let n = 0;
    const w = await setup({ box: { openerLine: () => (n++ ? undefined : LINE) }, timings: { openerDeadlineMs: 60, openerHangMs: 400 } });
    await w.room.voiceSettings(w.link, { action: "set", first_words: "opener" } as any);
    await w.shake(SPEECH);
    await until(() => says(w).includes(LINE));
    await new Promise((r) => setTimeout(r, 40));
    await w.shake({ words: ["and", "another"] }, "shake-2");
    await new Promise((r) => setTimeout(r, 700));
    expect(says(w)).toEqual([LINE]);
  });

  it("talk: heard carries the opener", async () => {
    const w = await world({ line: "Hmm, the calendar," });
    const r = (await w.room.talk(w.a, {})) as any;
    expect(r.result.heard).toBeTruthy();
    expect(r.result.opener).toBe("Hmm, the calendar,");
  });
});
