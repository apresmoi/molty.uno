// Molty's reflex (core/reflex.ts, docs/AGENT-LINK.md "Reflex"): chunking, the keywords heuristic,
// the box's verdicts, engine choice and fallback, and the room: gating (owner screens, agents' reacts
// win, rate limit), the trace, the owner setting.
import { describe, expect, it } from "vitest";
import {
  LIVELINESS,
  REFLEX,
  REFLEX_LIVELINESS,
  REFLEX_PICKS,
  REFLEX_SHOW,
  REFLEX_SOFT,
  type ReflexNote,
  ReflexSession,
  type ReflexVerdict,
  UTTERANCE_CATEGORIES,
  keywordCategory,
  keywordReflex,
  parseBoxReflex,
  reflexDue,
  reflexGate,
  reflexInput,
  resolveEngine,
} from "../src/core/reflex.js";
import { realClock } from "../src/core/store.js";
import { DEVICE } from "./helpers.js";
import { readUntil, rest, setup, until } from "./turn-harness.js";

// ---------------------------------------------------------------- pure

describe("reflex chunks", () => {
  const { calm, normal, lively } = REFLEX_LIVELINESS;
  it("the first at REFLEX.firstWords words (no sentence end needed), then every few new words, a sentence end or a little speech time", () => {
    expect(REFLEX.firstWords).toBe(3);
    expect(reflexDue(0, "hi.", 0, normal)).toBeUndefined();
    expect(reflexDue(0, "hi there.", 0, normal)).toBeUndefined(); // under three words, even at a sentence end
    expect(reflexDue(0, "what's up, Molty", 0, normal)).toBe(3); // the first chunk: three words, mid-sentence
    expect(reflexDue(0, "what's up, Molty", 0, calm)).toBe(3); // every liveliness
    expect(reflexDue(3, "what's up, Molty", 9000, normal)).toBeUndefined(); // nothing new, however long
    expect(reflexDue(3, "what's up, Molty what are you", 100, normal)).toBeUndefined(); // 3 new words: normal waits for 4
    expect(reflexDue(3, "what's up, Molty what are you", 100, lively)).toBe(6); // lively: 3
    expect(reflexDue(3, "what's up, Molty what are you doing", 100, normal)).toBe(7);
    expect(reflexDue(3, "what's up, Molty what are you doing", 100, calm)).toBeUndefined(); // calm: 6
    expect(reflexDue(3, "what's up, Molty what", normal.everyMs, normal)).toBe(4); // a little speech time with a new word
    expect(reflexDue(3, "what's up, Molty what", normal.everyMs - 1, normal)).toBeUndefined();
    expect(reflexDue(3, "what's up, Molty what", lively.everyMs, lively)).toBe(4);
    expect(reflexDue(3, "what's up, Molty? you know?", 100, normal)).toBe(5); // a sentence end
    expect(reflexDue(3, "what's up, Molty so I was...", 100, normal)).toBeUndefined(); // an ellipsis ends nothing
  });

  it("the livelinesses are ordered: livelier = sooner, more often, lower gates, mild picks", () => {
    expect(LIVELINESS).toEqual(["calm", "normal", "lively"]);
    for (const [a, b] of [
      [calm, normal],
      [normal, lively],
    ]) {
      expect(b.everyWords).toBeLessThanOrEqual(a.everyWords);
      expect(b.everyMs).toBeLessThanOrEqual(a.everyMs);
      expect(b.gapMs).toBeLessThan(a.gapMs);
      for (const e of ["local", "hosted"] as const) expect(b.gate[e]).toBeLessThan(a.gate[e]);
      expect(b.mild?.none ?? 0).toBeGreaterThan(a.mild?.none ?? 0);
    }
    expect(calm.mild).toBeUndefined();
    expect(lively.gapMs).toBe(1500);
    expect(normal.gapMs).toBe(2000);
    // every soft gesture is a one-shot act, never a mood (the listening stays up)
    for (const v of Object.values(REFLEX_SOFT)) expect(typeof v).toBe("string");
    expect(Object.keys(REFLEX_SOFT).sort()).toEqual(Object.keys(REFLEX_SHOW).sort());
  });

  it("an engine sees the last maxChars characters, cut at a word", () => {
    const long = "word ".repeat(200) + "the end";
    const t = reflexInput(long);
    expect(t.length).toBeLessThanOrEqual(REFLEX.maxChars);
    expect(t.endsWith("the end")).toBe(true);
    expect(t.startsWith("word")).toBe(true);
  });
});

describe("keywords engine", () => {
  const pick = (t: string) => keywordReflex(t).pick;
  it("picks the obvious ones, none otherwise", () => {
    expect(pick("haha that was hilarious")).toBe("laugh");
    expect(pick("my grandma passed away last week")).toBe("sad");
    expect(pick("oh my god you won't believe this")).toBe("surprised");
    expect(pick("I got the job")).toBe("happy");
    expect(pick("why is the sky blue")).toBe("curious");
    expect(pick("do you remember where I parked?")).toBe("curious");
    expect(pick("can you remind me tomorrow?")).toBe("none"); // a request, not curiosity
    expect(pick("I need to buy milk and eggs")).toBe("none");
  });
  it("a negated feeling is no reaction", () => {
    expect(pick("I'm not happy with the plan")).toBe("none");
    expect(pick("honestly I am not sad about it")).toBe("none");
  });
  it("is instant and keyless", () => {
    expect(keywordReflex("wow")).toMatchObject({ engine: "keywords", ms: 0, chars: 3 });
  });
});

describe("utterance category (context-picked fillers)", () => {
  it("keywords: hard > request > question > other, on the last sentence", () => {
    expect(keywordCategory("why do cats purr so much?")).toBe("hard");
    expect(keywordCategory("should I take the job or stay")).toBe("hard");
    expect(keywordCategory("remind me to call mom at five")).toBe("request");
    expect(keywordCategory("can you play some jazz?")).toBe("request");
    expect(keywordCategory("what time is it?")).toBe("question");
    expect(keywordCategory("is it raining")).toBe("question");
    expect(keywordCategory("I had a long day at work.")).toBe("other");
    expect(keywordCategory("what time is it? anyway I had a long day.")).toBe("other");
    expect(keywordReflex("set a timer")).toMatchObject({ cat: "request", catConf: 1 });
  });
  it("box verdicts carry it", () => {
    expect(parseBoxReflex({ n: 3, pick: "none", conf: 0.9, cat: "question", cat_conf: 0.8 })).toMatchObject({ cat: "question", catConf: 0.8 });
  });
});

describe("engine choice", () => {
  it("auto: local when the box offers it, else keywords", () => {
    expect(resolveEngine("auto", { local: true })).toEqual({ engine: "local" });
    expect(resolveEngine("auto", { local: false })).toEqual({ engine: "keywords" });
  });
  it("local without a box falls back to keywords, saying why", () => {
    expect(resolveEngine("local", { local: false })).toEqual({ engine: "keywords", fallback: "unavailable" });
    expect(resolveEngine("keywords", { local: true })).toEqual({ engine: "keywords" });
  });
});

describe("model engines: request and answer", () => {
  it("the box's stt.reflex is validated (ungated); a hosted model's verdict uses the stricter gate", () => {
    expect(parseBoxReflex({ n: 5, pick: "sad", conf: 0.35, hosted: true })).toMatchObject({ engine: "local", hosted: true });
    const gated = (hosted: boolean) => reflexGate({ engine: "local", ...(hosted ? { hosted } : {}), pick: "sad", conf: 0.35, ms: 0, chars: 0 }, REFLEX_LIVELINESS.normal).pick;
    expect([gated(false), gated(true)]).toEqual(["sad", "none"]);
    expect(parseBoxReflex({ n: 5, pick: "sad", conf: 0.9, p: { sad: 0.9 }, ms: 34.6, chars: 20, tokens: 7 })).toMatchObject({ n: 5, engine: "local", pick: "sad", ms: 35, tokens: 7 });
    expect(parseBoxReflex({ n: 5, pick: "sad", conf: 0.1 })).toMatchObject({ pick: "sad", conf: 0.1 });
    expect(parseBoxReflex({ n: 0, pick: "sad" })).toBeUndefined();
    expect(parseBoxReflex({ n: 3, pick: "angry" })).toBeUndefined();
  });

});

describe("the liveliness gate (reflexGate)", () => {
  const { calm, normal, lively } = REFLEX_LIVELINESS;
  const jev = (pick: string, conf: number, p: Record<string, number>, extra: Partial<ReflexVerdict> = {}): ReflexVerdict => ({ engine: "local", hosted: true, pick: pick as any, conf, p, ms: 1, chars: 1, ...extra });
  const g = (v: ReflexVerdict, prof = normal) => {
    const o = reflexGate(v, prof);
    return [o.pick, o.soft ?? false, o.raw ?? null];
  };
  it("a confident pick shows in full at or above the engine's gate; below it, none (the raw choice kept)", () => {
    expect(g(jev("happy", 0.45, { happy: 0.5, none: 0.4 }), normal)).toEqual(["happy", false, null]); // normal hosted gate 0.4
    expect(g(jev("happy", 0.45, { happy: 0.5, none: 0.4 }), calm)).toEqual(["none", false, "happy"]); // calm 0.5, no mild
    expect(g(jev("happy", 0.4, {}), normal)).toEqual(["happy", false, null]); // at the gate
    expect(g(jev("happy", 0.39, {}), normal)).toEqual(["none", false, "happy"]); // no probabilities: no mild pick
    expect(g({ engine: "local", pick: "sad", conf: 0.28, ms: 1, chars: 1 }, normal)).toEqual(["none", false, "sad"]);
    expect(g({ engine: "local", pick: "sad", conf: 0.28, ms: 1, chars: 1 }, lively)).toEqual(["sad", false, null]);
  });
  it("mild: `none` unsure and a reaction plausible → that reaction's softer gesture (normal, lively; never calm)", () => {
    // "So I've been thinking" (Jev, real): none 0.58, curious 0.42
    const thinking = jev("none", 0.5, { curious: 0.42, none: 0.58 });
    expect(g(thinking, normal)).toEqual(["curious", true, "none"]);
    expect(g(thinking, lively)).toEqual(["curious", true, "none"]);
    expect(g(thinking, calm)).toEqual(["none", false, null]);
    // Juan's explanatory turns: none ≈ 0.7, curious ≈ 0.25 → only lively shows it
    const explain = jev("none", 0.68, { curious: 0.27, none: 0.73 });
    expect(g(explain, normal)).toEqual(["none", false, null]);
    expect(g(explain, lively)).toEqual(["curious", true, "none"]);
    // sure it is nothing: nothing, at every liveliness
    for (const prof of [calm, normal, lively]) expect(g(jev("none", 0.95, { curious: 0.04, none: 0.95 }), prof)).toEqual(["none", false, null]);
    // a below-gate pick that is also the most likely reaction: soft, no raw
    expect(g(jev("happy", 0.3, { happy: 0.35, none: 0.5 }), normal)).toEqual(["happy", true, null]);
    // under mild.min: nothing
    expect(g(jev("none", 0.5, { curious: 0.29, none: 0.55 }), normal)).toEqual(["none", false, null]);
  });
  it("a request is never curiosity, in full or mild", () => {
    expect(g(jev("none", 0.52, { curious: 0.48, none: 0.52 }, { cat: "request" }), lively)).toEqual(["none", false, "none"]);
    expect(g(jev("curious", 0.9, { curious: 0.9 }, { cat: "request" }), lively)).toEqual(["none", false, "curious"]);
    expect(g(jev("curious", 0.9, { curious: 0.9 }, { cat: "question" }), lively)).toEqual(["curious", false, null]);
  });
  it("keywords (no probabilities) are all-or-nothing at every liveliness", () => {
    for (const prof of [calm, normal, lively]) {
      expect(reflexGate(keywordReflex("haha that's hilarious"), prof)).toMatchObject({ pick: "laugh" });
      expect(reflexGate(keywordReflex("I need to buy milk"), prof).pick).toBe("none");
    }
  });
});

// ---------------------------------------------------------------- one listen's session (no room)

describe("reflex session", () => {
  function make(o: Partial<ConstructorParameters<typeof ReflexSession>[0]> = {}) {
    const notes: ReflexNote[] = [];
    const fired: string[] = [];
    const s = new ReflexSession({
      mode: "local",
      avail: { local: true },
      clock: realClock,
      fire: async (p, soft) => {
        fired.push(soft ? `${p}~` : p);
        return "ok";
      },
      note: (n) => notes.push(n),
      ...o,
    });
    return { s, notes, fired };
  }
  /** A hand-driven clock (no timers needed by the keywords engine). */
  const manual = () => {
    const c = { t: 1000, now: () => c.t, setTimeout: (f: () => void, ms: number) => setTimeout(f, ms), clearTimeout: (h: unknown) => clearTimeout(h as ReturnType<typeof setTimeout>) };
    return c;
  };
  const say = (s: ReflexSession, text: string, c?: { t: number }, perWordMs = 0) => {
    const w = text.split(" ");
    for (let i = 1; i <= w.length; i++) {
      if (c) c.t += perWordMs;
      s.text(w.slice(0, i).join(" "));
    }
  };

  it("cadence: the first decision at three words, then every few words by liveliness (one word per partial)", async () => {
    const text = "so I was walking home and the street was empty and quiet tonight";
    const chunks: Record<string, number[]> = {};
    for (const lv of LIVELINESS) {
      const { s, notes } = make({ mode: "keywords", liveliness: lv });
      expect(s.liveliness).toBe(lv);
      say(s, text);
      await s.idle();
      chunks[lv] = notes.map((n) => n.n);
    }
    expect(chunks).toEqual({ calm: [3, 9], normal: [3, 7, 11], lively: [3, 6, 9, 12] });
    expect(make({ mode: "keywords" }).s.liveliness).toBe("normal"); // the default
    expect(make({ mode: "keywords", liveliness: "loud" as any }).s.liveliness).toBe("normal");
  });

  it("cadence: a slow speaker gets a decision once everyMs of speech passed, with a new word", async () => {
    const c = manual();
    const { s, notes } = make({ mode: "keywords", clock: c, liveliness: "normal" });
    say(s, "well I think", c, 100); // 3 words: the first chunk
    c.t += REFLEX_LIVELINESS.normal.everyMs - 1;
    s.text("well I think that"); // one new word, not yet everyMs: nothing
    c.t += 1;
    s.text("well I think that"); // everyMs since the last chunk, a word it has not seen: due
    c.t += 5000;
    s.text("well I think that"); // nothing new: nothing, however long
    await s.idle();
    expect(notes.map((n) => n.n)).toEqual([3, 4]);
  });

  it("a mild pick fires its soft gesture; the trace notes it soft with the engine's raw choice", async () => {
    const { s, notes, fired } = make();
    s.text("So I've been thinking");
    s.onLocal({ n: 4, engine: "local", hosted: true, pick: "none", conf: 0.5, p: { curious: 0.42, none: 0.58 }, ms: 90, chars: 21 });
    await s.idle();
    expect(fired).toEqual(["curious~"]);
    expect(notes[0]).toMatchObject({ n: 4, pick: "curious", soft: true, raw: "none", c: 0.5, fired: "ok" });
  });

  it("an older chunk's verdict landing after a newer one's is stale: noted, never shown", async () => {
    const { s, notes, fired } = make();
    s.text("I just got");
    s.text("I just got the job.");
    s.onLocal({ n: 5, engine: "local", pick: "happy", conf: 0.9, ms: 10, chars: 19 });
    await new Promise((r) => setTimeout(r, 5));
    s.onLocal({ n: 3, engine: "local", pick: "sad", conf: 0.9, ms: 120, chars: 10 });
    await s.idle();
    expect(fired).toEqual(["happy"]);
    expect(notes.map((n) => [n.n, n.pick, n.fired])).toEqual([
      [5, "happy", "ok"],
      [3, "sad", "stale"],
    ]);
  });

  it("fires the box's pick, noted with latency, size, tokens and probabilities", async () => {
    const { s, notes, fired } = make();
    s.text("I just");
    s.text("I just got the job.");
    s.onLocal({ n: 5, engine: "local", pick: "happy", conf: 0.9, p: { happy: 0.9 }, ms: 34, chars: 19, tokens: 400 });
    await s.idle();
    expect(fired).toEqual(["happy"]);
    expect(notes).toEqual([expect.objectContaining({ n: 5, e: "local", pick: "happy", c: 0.9, tk: 400, ch: 19, ms: 34, fired: "ok", p: [null, 0.9, null, null, null, null] })]);
  });

  it("no answer in time falls back to keywords, which fire (fallback reason noted); the listen is never blocked", async () => {
    const { s, notes, fired } = make({ timeoutMs: 50 });
    const t0 = Date.now();
    s.text("oh my god, no way.");
    expect(Date.now() - t0).toBeLessThan(20); // text() returns at once
    await s.idle();
    expect(fired).toEqual(["surprised"]);
    expect(notes.map((n) => [n.e, n.err ?? null, n.fb ?? null, n.fired ?? null])).toEqual([
      ["local", "timeout", null, null],
      ["keywords", null, "timeout", "ok"],
    ]);
  });

  it("no box: keywords from the start, the reason on every decision", async () => {
    const { s, notes, fired } = make({ avail: { local: false } });
    expect([s.engine, s.fallback]).toEqual(["keywords", "unavailable"]);
    s.text("haha that's hilarious.");
    await s.idle();
    expect(fired).toEqual(["laugh"]);
    expect(notes[0]).toMatchObject({ e: "keywords", fb: "unavailable", fired: "ok" });
  });

  it("local: waits for the box's verdict of that chunk; none in time → keywords; a late one is noted, never fired", async () => {
    const { s, notes, fired } = make({ timeoutMs: 40 });
    s.text("it is raining.");
    s.onLocal({ n: 3, engine: "local", pick: "sad", conf: 0.8, ms: 31, chars: 14, tokens: 7 });
    await s.idle();
    s.text("it is raining. oh my god a rainbow is out there");
    await s.idle();
    s.onLocal({ n: 11, engine: "local", pick: "happy", conf: 0.9, ms: 600, chars: 47 });
    expect(fired).toEqual(["sad", "surprised"]);
    expect(notes.map((n) => [n.n, n.e, n.err ?? null, n.fb ?? null, n.fired ?? null, n.late ?? false])).toEqual([
      [3, "local", null, null, "ok", false],
      [11, "local", "timeout", null, null, false],
      [11, "keywords", null, "timeout", "ok", false],
      [11, "local", null, null, null, true],
    ]);
    expect(notes[0]).toMatchObject({ tk: 7, ms: 31 });
  });
});

// ---------------------------------------------------------------- the room: a shake turn with the voice box

describe("reflex in the room", () => {
  const set = (w: Awaited<ReturnType<typeof setup>>, body: Record<string, unknown>) => w.room.voiceSettings(w.link, { action: "set", ...body });
  const cmds = (w: Awaited<ReturnType<typeof setup>>) => w.dev.log.filter((l) => l.name === "mood" || l.name === "act").map((l) => ({ [l.name!]: l.args }));
  const trace = async (w: Awaited<ReturnType<typeof setup>>, job: string) => {
    let entry: any;
    await until(async () => {
      entry = ((await w.room.ownerActivity(w.link, { action: "get", query: { job, kind: "job" } })) as any).result.entries[0];
      return !!entry;
    });
    return entry.data.trace;
  };
  const words = (t: string) => t.split(" ");

  it("off by default: Molty shows nothing by itself", async () => {
    const w = await setup();
    expect(((await w.room.voiceSettings(w.link, { action: "get" })) as any).result.reflex).toMatchObject({ mode: "off", engine: null, engines: { local: false, keywords: true } });
    const turn = await w.shake({ words: words("oh my god I just got the job."), chunkMs: 30 });
    await readUntil(w.room, w.a, (e) => e.type === "utterance");
    expect(cmds(w)).toEqual([]);
    expect((await trace(w, turn)).relay.reflex).toBeUndefined();
  });

  it("keywords: the pick shows screen-only through the react path (react mood, back to listening), once per gap; the trace keeps no words", async () => {
    const w = await setup();
    expect((await set(w, { reflex: "keywords" })).ok).toBe(true);
    const turn = await w.shake({ words: words("oh my god I just got the job. haha that is so funny."), chunkMs: 30 });
    await readUntil(w.room, w.a, (e) => e.type === "utterance");
    await until(() => cmds(w).length >= 2);
    expect(cmds(w)).toEqual([{ mood: { mood: "surprised", hold_s: 0, react: true } }, { mood: { mood: "listening", hold_s: 0, react: true } }]);
    expect(w.dev.log.filter((l) => l.name === "say" || l.kind === "frame")).toEqual([]);
    const t = await trace(w, turn);
    expect(t.relay.reflex_cfg).toEqual({ mode: "keywords", engine: "keywords", lv: "normal" });
    expect(t.relay.reflex.map((n: ReflexNote) => [n.n, n.e, n.pick, n.fired])).toEqual([
      [3, "keywords", "surprised", "ok"], // three words: "oh my god", mid-sentence
      [7, "keywords", "surprised", "rate_limited"], // four new words (normal)
      [8, "keywords", "surprised", "rate_limited"], // the sentence end
      [12, "keywords", "laugh", "rate_limited"],
      [13, "keywords", "laugh", "rate_limited"],
    ]);
    expect(t.relay.reflex[0]).toMatchObject({ ms: 0, ch: 9, at: expect.any(Number) });
    expect(JSON.stringify(t)).not.toMatch(/job|funny/);
  });

  it("an agent's react wins: once an agent reacted, the reflex yields for the rest of the turn", async () => {
    const w = await setup();
    await set(w, { reflex: "keywords" });
    const turn = await w.shake({ words: words("well hello there Molty. oh my god I won the lottery."), chunkMs: 70 });
    await readUntil(w.room, w.a, (e) => e.type === "utterance.partial");
    expect((await w.room.react(w.a, { turn_id: turn, action: "nod" })).ok).toBe(true);
    await readUntil(w.room, w.a, (e) => e.type === "utterance");
    const t = await trace(w, turn);
    expect(t.relay.reflex.map((n: ReflexNote) => [n.pick, n.fired])).toEqual([
      ["none", "none"],
      ["none", "none"],
      ["surprised", "agent"],
      ["surprised", "agent"],
    ]);
    expect(cmds(w)).toEqual([{ act: { action: "nod" } }]);
  });

  it("the yield lasts the whole turn, not just the react gap", async () => {
    const w = await setup();
    await set(w, { reflex: "keywords" });
    const turn = await w.shake({ words: words("well hello there Molty. and then we went on about it for a long time and oh my god I won."), chunkMs: 220 }, "shake-y");
    await readUntil(w.room, w.a, (e) => e.type === "utterance.partial");
    expect((await w.room.react(w.a, { turn_id: turn, action: "nod" })).ok).toBe(true);
    const reactedAt = Date.now();
    await readUntil(w.room, w.a, (e) => e.type === "utterance", [], 8000);
    expect(Date.now() - reactedAt).toBeGreaterThan(REFLEX_LIVELINESS.normal.gapMs); // the last chunk came after the gap
    const t = await trace(w, turn);
    expect(t.relay.reflex.at(-1)).toMatchObject({ pick: "surprised", fired: "agent" });
    expect(cmds(w)).toEqual([{ act: { action: "nod" } }]);
  }, 12000);

  it("never over an owner screen (a pairing code, an update question)", async () => {
    const w = await setup();
    await set(w, { reflex: "keywords" });
    await w.kv.put("paircur", "p1");
    await w.kv.put("pair:p1", { id: "p1", req: "r", code: "123456", agent_name: "x", created: Date.now(), expires: Date.now() + 60000, status: "pending" });
    let turn = await w.shake({ words: words("oh my god I just got the job."), chunkMs: 30 });
    await readUntil(w.room, w.a, (e) => e.type === "utterance");
    expect((await trace(w, turn)).relay.reflex.map((n: ReflexNote) => n.fired)).toEqual(["screen", "screen", "screen"]);
    await w.kv.delete("paircur");
    await w.kv.put("upd", { job: "u", from: "1", to: "2", phase: "confirm", deadline: Date.now() + 60000 });
    turn = await w.shake({ words: words("oh my god I just got the job."), chunkMs: 30 }, "shake-2");
    await readUntil(w.room, w.a, (e) => e.type === "utterance" && e.data.turn_id === turn);
    expect((await trace(w, turn)).relay.reflex.map((n: ReflexNote) => n.fired)).toEqual(["screen", "screen", "screen"]);
    expect(cmds(w)).toEqual([]);
  });

  it("firmware without react / act caps: unsupported, nothing sent", async () => {
    const w = await setup({ caps: ["say", "say_cues", "listen", "listen_stop", "listen_window", "ack", "shake_talk", "mood"] });
    await set(w, { reflex: "keywords" });
    const turn = await w.shake({ words: words("oh my god I just got the job."), chunkMs: 30 });
    await readUntil(w.room, w.a, (e) => e.type === "utterance");
    expect((await trace(w, turn)).relay.reflex.map((n: ReflexNote) => n.fired)).toEqual(["unsupported", "unsupported", "unsupported"]);
    expect(cmds(w)).toEqual([]);
  });


  it("local: the box classifies (stt.start reflex:true, its stt.reflex per chunk), no relay call", async () => {
    const calls: string[] = [];
    const w = await setup({
      fetch: async (r) => (calls.push(r.url), new Response("{}")),
      // the box classifies every word count from three (molty_voice/reflex.py): the relay picks its chunks among them
      box: { reflex: true, reflexFor: (text) => (text.split(" ").length < 3 ? undefined : text.includes("died") ? { n: text.split(" ").length, pick: "sad", conf: 0.88, p: { sad: 0.88, none: 0.1 }, cat: "other", cat_conf: 0.7, ms: 34, chars: text.length, tokens: 9 } : { n: text.split(" ").length, pick: "none", conf: 0.9, p: { none: 0.9 }, ms: 30, chars: text.length }) },
    });
    expect(((await set(w, { reflex: "auto" })) as any).result.reflex).toMatchObject({ mode: "auto", engine: "local", engines: { local: true, keywords: true } });
    const turn = await w.shake({ words: words("my old dog died yesterday."), chunkMs: 30 });
    await readUntil(w.room, w.a, (e) => e.type === "utterance");
    expect(w.box.starts.at(-1)).toMatchObject({ reflex: true });
    const t = await trace(w, turn);
    expect(t.relay.reflex_cfg).toEqual({ mode: "auto", engine: "local", lv: "normal" });
    expect(t.relay.reflex).toEqual([
      expect.objectContaining({ n: 3, e: "local", pick: "none", fired: "none" }),
      expect.objectContaining({ n: 5, e: "local", pick: "sad", c: 0.88, ms: 34, tk: 9, fired: "ok", p: [null, null, 0.88, null, null, 0.1], cat: "other", cc: 0.7 }),
    ]);
    expect(cmds(w)).toEqual([{ act: { action: "nod" } }]);
    expect(calls).toEqual([]);
    // the filler's seam: the reflex's own category for those words, no new call
    expect(await w.room.fillerCategory("my old dog died yesterday.")).toEqual({ category: "other", engine: "local", ms: 0 });
  });


  it("the owner setting: validated, logged, through REST with the link code", async () => {
    const w = await setup();
    expect(await set(w, { reflex: "loud" })).toMatchObject({ ok: false, error: "invalid_args" });
    expect(await set(w, { reflex: "jev" })).toMatchObject({ ok: false, error: "invalid_args" }); // hosted models live on the box
    const r = await rest(w.backend, "", "POST", `/v1/devices/${DEVICE}/voice`, { reflex: "local" }, { "x-molty-link-code": w.link });
    // no box here: keywords answer, saying why
    expect(r.body.result.reflex).toMatchObject({ mode: "local", engine: "keywords", fallback: "unavailable", timeout_ms: REFLEX.timeoutMs });
    // no listen yet: the filler's category comes from the keywords
    expect(await w.room.fillerCategory("could you turn on the lights")).toEqual({ category: "request", engine: "keywords", ms: 0 });
    expect((await rest(w.backend, "", "POST", `/v1/devices/${DEVICE}/voice`, { reflex: "off" }, { "x-molty-link-code": w.link })).body.result.reflex).toMatchObject({ mode: "off", engine: null });
    const settings = ((await w.room.ownerActivity(w.link, { action: "get", query: { kind: "settings" } })) as any).result.entries.map((e: any) => e.data);
    expect(settings).toEqual(expect.arrayContaining([{ mode: "auto", voice: null, reflex: "local" }, { mode: "auto", voice: null, reflex: "off" }]));
    expect((await rest(w.backend, "", "POST", `/v1/devices/${DEVICE}/voice`, { reflex: "local" }, { "x-molty-link-code": "WRONG234" })).status).toBe(401);
  });
});

describe("liveliness in the room", () => {
  const set = (w: Awaited<ReturnType<typeof setup>>, body: Record<string, unknown>) => w.room.voiceSettings(w.link, { action: "set", ...body });
  const cmds = (w: Awaited<ReturnType<typeof setup>>) => w.dev.log.filter((l) => l.name === "mood" || l.name === "act").map((l) => ({ [l.name!]: l.args }));
  const trace = async (w: Awaited<ReturnType<typeof setup>>, job: string) => {
    let entry: any;
    await until(async () => {
      entry = ((await w.room.ownerActivity(w.link, { action: "get", query: { job, kind: "job" } })) as any).result.entries[0];
      return !!entry;
    });
    return entry.data.trace;
  };
  // A hosted model on the box, unsure: none 0.58, curious 0.42 (a mild pick at normal and lively)
  const mild = { reflex: true, reflexFor: (text: string) => (text.split(" ").length < 3 ? undefined : { n: text.split(" ").length, hosted: true, pick: "none", conf: 0.5, p: { curious: 0.42, none: 0.58 }, cat: "other", cat_conf: 0.9, ms: 90, chars: text.length, tokens: 300 }) };
  const ramble = "so I was thinking about the bridge and the box and how the words".split(" "); // 14 words

  it("the owner setting: calm | normal | lively, default normal, validated, logged, through REST", async () => {
    const w = await setup();
    expect(((await w.room.voiceSettings(w.link, { action: "get" })) as any).result.reflex.liveliness).toBe("normal");
    expect(await set(w, { liveliness: "hyper" })).toMatchObject({ ok: false, error: "invalid_args" });
    expect(await set(w, { liveliness: 2 })).toMatchObject({ ok: false, error: "invalid_args" });
    const r = await rest(w.backend, "", "POST", `/v1/devices/${DEVICE}/voice`, { liveliness: "lively" }, { "x-molty-link-code": w.link });
    expect(r.body.result.reflex.liveliness).toBe("lively");
    expect(((await set(w, { liveliness: "calm" })) as any).result.reflex.liveliness).toBe("calm");
    expect(((await set(w, { liveliness: "normal" })) as any).result.reflex.liveliness).toBe("normal");
    const settings = ((await w.room.ownerActivity(w.link, { action: "get", query: { kind: "settings" } })) as any).result.entries.map((e: any) => e.data.liveliness).filter(Boolean);
    expect(settings.sort()).toEqual(["calm", "lively", "normal"]);
    expect((await rest(w.backend, "", "POST", `/v1/devices/${DEVICE}/voice`, { liveliness: "lively" }, { "x-molty-link-code": "WRONG234" })).status).toBe(401);
  });

  for (const [lv, expected] of [
    ["normal", [[3, "ok"], [7, "rate_limited"], [11, "rate_limited"]]],
    ["lively", [[3, "ok"], [6, "rate_limited"], [9, "rate_limited"], [12, "ok"]]],
  ] as const) {
    it(`${lv}: its cadence and gap; a mild pick shows as its soft gesture (a glance, sides alternating), never a mood`, async () => {
      const w = await setup({ box: mild as never });
      await set(w, { reflex: "local", liveliness: lv });
      const turn = await w.shake({ words: ramble, chunkMs: 200 }, `shake-${lv}`);
      await readUntil(w.room, w.a, (e) => e.type === "utterance", [], 8000);
      const t = await trace(w, turn);
      expect(t.relay.reflex_cfg).toMatchObject({ engine: "local", lv });
      expect(t.relay.reflex.slice(0, expected.length).map((n: ReflexNote) => [n.n, n.fired])).toEqual(expected);
      expect(t.relay.reflex[0]).toMatchObject({ pick: "curious", soft: true, raw: "none", c: 0.5 });
      expect(cmds(w)).toEqual(lv === "lively" ? [{ act: { action: "glance_left" } }, { act: { action: "glance_right" } }] : [{ act: { action: "glance_left" } }]);
    }, 12000);
  }

  it("calm: the same unsure verdicts show nothing", async () => {
    const w = await setup({ box: mild as never });
    await set(w, { reflex: "local", liveliness: "calm" });
    const turn = await w.shake({ words: ramble.slice(0, 9), chunkMs: 30 }, "shake-calm");
    await readUntil(w.room, w.a, (e) => e.type === "utterance");
    const t = await trace(w, turn);
    expect(t.relay.reflex.map((n: ReflexNote) => [n.n, n.pick, n.fired])).toEqual([
      [3, "none", "none"],
      [9, "none", "none"],
    ]);
    expect(cmds(w)).toEqual([]);
  });
});

// keep the verdict type in use for readers of this file
export type _V = ReflexVerdict;
