// First words with overlap (core/opener.ts planReply, docs/AGENT-LINK.md "First words"): the agent
// reads the planned line, may keep its first m words, and its reply takes over at word m.

import { describe, expect, it } from "vitest";
import { guardOpener } from "../src/core/opener.js";
import { readUntil, setup, until } from "./turn-harness.js";

const SPEECH = { words: "what's the weather in Berlin tomorrow".split(" ") };
// 13 words; clauses end after words 1, 5, 7 and 13
const LINE = "Okay, the weather in Berlin, for tomorrow, let me walk you through it.";
const WORD = 4000; // samples per word of the fake box's voice (250 ms)

async function world(o: { line?: string; play?: { lagMs: number }; wordMs?: number; timings?: Record<string, number> } = {}) {
  const line = o.line ?? LINE;
  const w = await setup({ play: o.play, box: { openerLine: () => line, wordMs: o.wordMs ?? 250 }, timings: { openerDeadlineMs: 200, openerHangMs: 15000, fillerDefault: 0, ...o.timings } });
  expect(((await w.room.voiceSettings(w.link, { action: "set", first_words: "opener" } as any)) as any).ok).toBe(true);
  return w;
}
const says = (w: any) => w.dev.log.filter((l: any) => l.name === "say").map((l: any) => l.args.text);
const utterance = async (w: any) => (await readUntil(w.room, w.a, (e) => e.type === "utterance")).find((e) => e.type === "utterance") as any;
const lineOf = (w: any) => (w.room as any).line;
/** Samples of the line the device got. */
const lineSamples = (w: any) => w.dev.samples.get(lineOf(w).job);
const ended = (w: any) => until(() => lineOf(w)?.ended === true, 6000);

describe("first words: the agent takes over at a word of the line", () => {
  it("the line passes the guard (a real, long one)", () => {
    expect(guardOpener(LINE, SPEECH.words.join(" "), [])).toEqual({ ok: true, text: LINE });
  });

  it("the utterance carries the planned line and its rate; keep without words says where it is", async () => {
    const w = await world();
    const turn = await w.shake(SPEECH);
    const u = await utterance(w);
    expect(u.data).toMatchObject({ opener: LINE, opener_wpm: expect.any(Number) });
    const at = (await w.room.command(w.a, "keep", { in_reply_to: turn })) as any;
    expect(at.result).toMatchObject({ words: 13, playing: expect.any(Number), sent: expect.any(Number), wpm: expect.any(Number) });
    expect(at.result.keep).toBeUndefined();
  });

  it("switch before wm: keep 5, then the reply starting with those words: the line stops at word 5 and the rest follows", async () => {
    const w = await world();
    const turn = await w.shake(SPEECH);
    await utterance(w);
    const k = (await w.room.command(w.a, "keep", { words: 5, in_reply_to: turn })) as any;
    expect(k.result).toMatchObject({ keep: 5, said: "Okay, the weather in Berlin," });
    expect(k.result.late).toBeUndefined();
    // the other agent may not answer now: the keep claimed the turn
    expect(await w.room.command(w.b, "say", { text: "Hi.", in_reply_to: turn })).toMatchObject({ ok: false, error: "turn_claimed" });
    expect((await w.room.command(w.a, "say", { text: "Okay, the weather in Berlin, rain until noon.", in_reply_to: turn })).ok).toBe(true);
    await until(() => says(w).length === 2, 6000);
    await ended(w);
    expect(says(w)).toEqual([LINE, "rain until noon."]);
    expect(lineSamples(w)).toBe(5 * WORD);
  });

  it("switch without keep: a reply that starts with the line's words branches there", async () => {
    const w = await world();
    const turn = await w.shake(SPEECH);
    await utterance(w);
    expect((await w.room.talk(w.a, { say: "Okay, the weather in Berlin, rain until noon.", in_reply_to: turn, listen: false })).ok).toBe(true);
    expect(says(w)).toEqual([LINE, "rain until noon."]);
    expect(lineSamples(w)).toBe(5 * WORD);
  });

  it("keep named on the reply: its text is what follows those words", async () => {
    const w = await world();
    const turn = await w.shake(SPEECH);
    await utterance(w);
    expect((await w.room.talk(w.a, { say: "rain until noon.", keep: 5, in_reply_to: turn, listen: false })).ok).toBe(true);
    expect(says(w)).toEqual([LINE, "rain until noon."]);
    expect(lineSamples(w)).toBe(5 * WORD);
  });

  it("pause at wm: the line reached the kept word and the reply is not there: a thinking sound, thinking, then the reply", async () => {
    const w = await world();
    const turn = await w.shake(SPEECH);
    await utterance(w);
    expect(((await w.room.command(w.a, "keep", { words: 2, in_reply_to: turn })) as any).result).toMatchObject({ keep: 2 });
    await until(() => says(w).includes("Hmm…"), 4000);
    expect(lineSamples(w)).toBe(2 * WORD);
    await until(() => w.dev.log.some((l: any) => l.name === "mood" && l.args.mood === "thinking"), 3000);
    expect((await w.room.talk(w.a, { say: "Okay, the forecast says rain.", in_reply_to: turn, listen: false })).ok).toBe(true);
    expect(says(w)).toEqual([LINE, "Hmm…", "forecast says rain."]);
  });

  it("past wm (no keep, a slow agent): the line finishes its clause, then 'I mean,' and the reply without the words already said", async () => {
    const w = await world();
    const turn = await w.shake(SPEECH);
    await utterance(w);
    await until(() => (lineOf(w)?.samples ?? 0) > 5 * WORD, 6000); // word 6 is going out
    expect((await w.room.talk(w.a, { say: "Okay, the weather, rain until noon.", in_reply_to: turn, listen: false })).ok).toBe(true);
    expect(says(w)).toEqual([LINE, "I mean, rain until noon."]);
    expect(lineSamples(w)).toBe(7 * WORD);
  });

  it("a keep that comes too late: the clause playing now ends it, and the result says what was said", async () => {
    const w = await world();
    const turn = await w.shake(SPEECH);
    await utterance(w);
    await until(() => (lineOf(w)?.samples ?? 0) > 5 * WORD, 6000);
    const k = (await w.room.command(w.a, "keep", { words: 2, in_reply_to: turn })) as any;
    expect(k.result).toMatchObject({ keep: 7, late: true, said: "Okay, the weather in Berlin, for tomorrow," });
    expect((await w.room.talk(w.a, { say: "Okay, the weather in Berlin, for tomorrow, rain.", in_reply_to: turn, listen: false })).ok).toBe(true);
    expect(says(w).at(-1)).toBe("rain.");
    expect(lineSamples(w)).toBe(7 * WORD);
  });

  it("a reply that ignores the line plays after the clause playing now", async () => {
    const w = await world();
    const turn = await w.shake(SPEECH);
    await utterance(w);
    await until(() => (lineOf(w)?.samples ?? 0) > WORD, 6000); // word 2 is going out: its clause ends at word 5
    expect((await w.room.talk(w.a, { say: "Rain until noon.", in_reply_to: turn, listen: false })).ok).toBe(true);
    expect(says(w)).toEqual([LINE, "Rain until noon."]);
    expect(lineSamples(w)).toBe(5 * WORD);
  });

  it("no first words: keep says so, and a reply is untouched", async () => {
    const w = await setup({ timings: { fillerDefault: 0 } });
    const turn = await w.shake(SPEECH);
    await utterance(w);
    expect(((await w.room.command(w.a, "keep", { words: 3, in_reply_to: turn })) as any).result).toMatchObject({ keep: null });
    expect(await w.room.command(w.a, "say", { text: "Hi.", keep: 99 })).toMatchObject({ ok: false, error: "invalid_args" });
  });
});

describe("a reply to the turn takes over from lead-in audio, never busy", () => {
  // the device plays in real time and refuses a say while one plays, like the real one
  it("behind the line and behind the closing, the reply waits for the device's say.done", async () => {
    const w = await world({ play: { lagMs: 150 } });
    const turn = await w.shake(SPEECH);
    await utterance(w);
    const r = (await w.room.talk(w.a, { say: "Okay, the weather in Berlin, for tomorrow, let me walk you through it. Rain.", in_reply_to: turn, listen: false })) as any;
    expect(r).toMatchObject({ ok: true, result: { status: "done" } });
    expect(says(w)).toEqual([LINE, "Rain."]);

    const x = await world({ line: "Hmm, the weather in Berlin,", wordMs: 100, play: { lagMs: 150 }, timings: { openerHangMs: 900 } });
    const t2 = await x.shake(SPEECH);
    await utterance(x);
    await until(() => says(x).includes("Hmm, give me a moment."), 4000);
    const late = (await x.room.talk(x.b, { say: "It's raining.", in_reply_to: t2, listen: false })) as any;
    expect(late).toMatchObject({ ok: true, result: { status: "done" } });
    expect(says(x).at(-1)).toBe("It's raining.");
    for (const d of [w, x]) expect(d.dev.log.some((l: any) => l.kind === "busy")).toBe(false);
  }, 15000);
});
