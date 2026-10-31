// Molty's reflex (core/reflex.ts, docs/AGENT-LINK.md "Reflex"): chunking, the keywords heuristic,
// the model engines' requests / parsing / timeout, engine choice and fallback, compare mode, and the
// room: gating (owner screens, agents' reacts win, rate limit), the trace, the owner setting, keys.
import { describe, expect, it } from "vitest";
import {
  EngineBench,
  REFLEX,
  REFLEX_PICKS,
  type ReflexNote,
  ReflexSession,
  type ReflexVerdict,
  UTTERANCE_CATEGORIES,
  categoryOf,
  jevBody,
  keywordCategory,
  keywordReflex,
  openaiBody,
  parseBoxReflex,
  parseJev,
  parseOpenAI,
  reflexDue,
  reflexInput,
  remoteReflex,
  resolveEngine,
} from "../src/core/reflex.js";
import { realClock } from "../src/core/store.js";
import { DEVICE } from "./helpers.js";
import { readUntil, rest, setup, until } from "./turn-harness.js";

const KEY_JEV = "ts-secret-key-0123456789";
const KEY_OAI = "sk-secret-openai-0123456789";

// ---------------------------------------------------------------- pure

describe("reflex chunks", () => {
  it("a sentence end with a new word, or everyWords new words; never under minWords", () => {
    expect(reflexDue(0, "hi.")).toBeUndefined();
    expect(reflexDue(0, "hi there")).toBeUndefined();
    expect(reflexDue(0, "hi there.")).toBe(2);
    expect(reflexDue(2, "hi there.")).toBeUndefined(); // nothing new
    expect(reflexDue(2, "hi there. so I was")).toBeUndefined();
    expect(reflexDue(2, "hi there. so I was thinking about the")).toBe(8);
    expect(reflexDue(2, "hi there. so I was...")).toBeUndefined(); // an ellipsis ends nothing
    expect(reflexDue(2, "hi there. you know what?")).toBe(5);
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
  it("box verdicts carry it; categoryOf asks a model engine and falls back to keywords", async () => {
    expect(parseBoxReflex({ n: 3, pick: "none", conf: 0.9, cat: "question", cat_conf: 0.8 })).toMatchObject({ cat: "question", catConf: 0.8 });
    const ok = async () => Response.json({ model: "jev", answers: { reaction: { type: "choice", choice: "none", confidence: 0.9, probabilities: {} }, category: { type: "choice", choice: "hard", confidence: 0.8, probabilities: {} } } });
    expect(await categoryOf("is it raining", { engine: "jev", keys: { jev: KEY_JEV }, fetch: ok, clock: realClock })).toMatchObject({ category: "hard", engine: "jev" });
    const down = async () => new Response("", { status: 503 });
    expect(await categoryOf("is it raining", { engine: "jev", keys: { jev: KEY_JEV }, fetch: down, clock: realClock })).toEqual({ category: "question", engine: "keywords", ms: 0 });
    expect(await categoryOf("is it raining", { engine: "openai", keys: {}, fetch: ok, clock: realClock })).toMatchObject({ category: "question", engine: "keywords" });
  });
});

describe("engine choice", () => {
  const none = { local: false, jev: false, openai: false };
  it("auto: local when the box serves the listen, else jev with a key, else keywords", () => {
    expect(resolveEngine("auto", { ...none, local: true, jev: true })).toEqual({ engine: "local" });
    expect(resolveEngine("auto", { ...none, jev: true, openai: true })).toEqual({ engine: "jev" });
    expect(resolveEngine("auto", { ...none, openai: true })).toEqual({ engine: "keywords" });
  });
  it("a chosen engine that is unavailable falls back to keywords, saying why", () => {
    expect(resolveEngine("local", none)).toEqual({ engine: "keywords", fallback: "unavailable" });
    expect(resolveEngine("jev", none)).toEqual({ engine: "keywords", fallback: "no_key" });
    expect(resolveEngine("openai", { ...none, openai: true })).toEqual({ engine: "openai" });
    expect(resolveEngine("keywords", { local: true, jev: true, openai: true })).toEqual({ engine: "keywords" });
  });
  it("an engine refused for good (401/403/404/400) is benched for a while; a timeout or 5xx is not", () => {
    const b = new EngineBench();
    b.note({ engine: "jev", pick: "none", ms: 1, chars: 1, error: "timeout" }, 0);
    b.note({ engine: "openai", pick: "none", ms: 1, chars: 1, error: "http_503" }, 0);
    expect([b.ok("jev", 1), b.ok("openai", 1)]).toEqual([true, true]);
    b.note({ engine: "jev", pick: "none", ms: 1, chars: 1, error: "http_401" }, 0);
    expect([b.ok("jev", 1), b.ok("jev", REFLEX.benchMs)]).toEqual([false, true]);
  });
});

describe("model engines: request and answer", () => {
  it("Jev: one Choice over the reactions, with the text so far as state", () => {
    const b = jevBody("Oh my god, I got the job!") as any;
    expect(b.model).toBe("jev-latest");
    expect(b.state).toEqual({ human_said_so_far: "Oh my god, I got the job!" });
    expect(b.questions.reaction.type).toBe("choice");
    expect(Object.keys(b.questions.reaction.criteria)).toEqual([...REFLEX_PICKS]);
    expect(b.questions.category.type).toBe("choice");
    expect(Object.keys(b.questions.category.criteria)).toEqual([...UTTERANCE_CATEGORIES]);
  });
  it("OpenAI Decisions: one choice question, gpt-6-luna", () => {
    const b = openaiBody("why?") as any;
    expect(b.model).toBe("gpt-6-luna");
    expect(b.questions).toHaveLength(2);
    expect(b.questions[0]).toMatchObject({ type: "choice", name: "reaction" });
    expect(b.questions[0].choices.map((c: any) => c.value)).toEqual([...REFLEX_PICKS]);
    // the utterance category rides in the same request
    expect(b.questions[1]).toMatchObject({ type: "choice", name: "category" });
    expect(b.questions[1].choices.map((c: any) => c.value)).toEqual([...UTTERANCE_CATEGORIES]);
  });
  it("parses answers, gates low confidence to none (keeping the raw pick), flags junk and refusals", () => {
    const jev = { model: "jev-1.14.0", answers: { reaction: { type: "choice", choice: "happy", confidence: 0.98, probabilities: { happy: 0.98, surprised: 0.02, bogus: 3 } } }, usage: { input_tokens: 398, output_tokens: 63 } };
    expect(parseJev(jev)).toEqual({ pick: "happy", conf: 0.98, p: { happy: 0.98, surprised: 0.02 }, tokens: 398, model: "jev-1.14.0" });
    expect(parseJev({ ...jev, answers: { reaction: { ...jev.answers.reaction, confidence: 0.2 } } })).toMatchObject({ pick: "none", raw: "happy", conf: 0.2 });
    expect(parseJev({ answers: {} })).toEqual({ pick: "none", error: "bad_response" });
    const oai = { model: "gpt-6-luna", answers: [{ type: "choice", name: "reaction", choice: "laugh", confidence: 0.7, probabilities: [{ value: "laugh", probability: 0.8 }, { value: "none", probability: 0.2 }] }], usage: { input_tokens: 120 } };
    expect(parseOpenAI(oai)).toEqual({ pick: "laugh", conf: 0.7, p: { laugh: 0.8, none: 0.2 }, tokens: 120, model: "gpt-6-luna" });
    expect(parseOpenAI({ answers: [{ type: "refusal", name: "reaction" }] })).toEqual({ pick: "none", error: "refused" });
    // the category answer, in the same response
    const jc = { ...jev, answers: { ...jev.answers, category: { type: "choice", choice: "hard", confidence: 0.6, probabilities: { hard: 0.7, question: 0.3 } } } };
    expect(parseJev(jc)).toMatchObject({ pick: "happy", cat: "hard", catConf: 0.6, catP: { hard: 0.7, question: 0.3 } });
    const oc = { ...oai, answers: [...oai.answers, { type: "choice", name: "category", choice: "request", confidence: 0.9, probabilities: [{ value: "request", probability: 0.95 }] }] };
    expect(parseOpenAI(oc)).toMatchObject({ pick: "laugh", cat: "request", catConf: 0.9, catP: { request: 0.95 } });
    expect(parseJev({ ...jev, answers: { ...jev.answers, category: { type: "choice", choice: "bogus" } } }).cat).toBeUndefined();
  });
  it("the box's stt.reflex is validated and gated like the others", () => {
    expect(parseBoxReflex({ n: 5, pick: "sad", conf: 0.9, p: { sad: 0.9 }, ms: 34.6, chars: 20, tokens: 7 })).toMatchObject({ n: 5, engine: "local", pick: "sad", ms: 35, tokens: 7 });
    expect(parseBoxReflex({ n: 5, pick: "sad", conf: 0.1 })).toMatchObject({ pick: "none", raw: "sad" });
    expect(parseBoxReflex({ n: 0, pick: "sad" })).toBeUndefined();
    expect(parseBoxReflex({ n: 3, pick: "angry" })).toBeUndefined();
  });

  it("a call: the key only in its Authorization header; a timeout aborts it; HTTP errors are codes (the body never read)", async () => {
    let seen: Request | undefined;
    const ok = await remoteReflex("jev", "wow", {
      key: KEY_JEV,
      clock: realClock,
      fetch: async (r) => {
        seen = r;
        return Response.json({ model: "jev-1.14.0", answers: { reaction: { type: "choice", choice: "surprised", confidence: 0.9, probabilities: { surprised: 0.95 } } }, usage: { input_tokens: 300 } });
      },
    });
    expect(ok).toMatchObject({ engine: "jev", pick: "surprised", tokens: 300, chars: 3 });
    expect(seen!.url).toBe("https://api.typesafe.ai/v1/systemone");
    expect(seen!.headers.get("authorization")).toBe(`Bearer ${KEY_JEV}`);
    expect(await seen!.clone().text()).not.toContain(KEY_JEV);

    let aborted = false;
    const t0 = Date.now();
    const slow = await remoteReflex("openai", "wow", {
      key: KEY_OAI,
      clock: realClock,
      timeoutMs: 80,
      fetch: (r) =>
        new Promise((_, rej) =>
          r.signal.addEventListener("abort", () => {
            aborted = true;
            rej(new Error("aborted"));
          }),
        ),
    });
    expect(slow).toMatchObject({ engine: "openai", pick: "none", error: "timeout" });
    expect(aborted).toBe(true);
    expect(Date.now() - t0).toBeLessThan(500);

    const denied = await remoteReflex("openai", "wow", { key: KEY_OAI, clock: realClock, fetch: async () => new Response(`bad key ${KEY_OAI}`, { status: 401 }) });
    expect(denied).toMatchObject({ error: "http_401", pick: "none" });
    expect(JSON.stringify(denied)).not.toContain(KEY_OAI);
    expect(await remoteReflex("jev", "wow", { key: undefined, clock: realClock, fetch: async () => new Response("") })).toMatchObject({ error: "no_key" });
  });
});

// ---------------------------------------------------------------- one listen's session (no room)

describe("reflex session", () => {
  const jevAnswer = (choice: string, conf = 0.9) => Response.json({ model: "jev-1.14.0", answers: { reaction: { type: "choice", choice, confidence: conf, probabilities: { [choice]: conf } } }, usage: { input_tokens: 400 } });
  const oaiAnswer = (choice: string) => Response.json({ model: "gpt-6-luna", answers: [{ type: "choice", name: "reaction", choice, confidence: 0.8, probabilities: [{ value: choice, probability: 0.9 }] }], usage: { input_tokens: 150 } });

  function make(o: Partial<ConstructorParameters<typeof ReflexSession>[0]> = {}) {
    const notes: ReflexNote[] = [];
    const fired: string[] = [];
    const s = new ReflexSession({
      mode: "jev",
      compare: false,
      avail: { local: false, jev: true, openai: false },
      keys: { jev: KEY_JEV },
      fetch: async () => jevAnswer("happy"),
      clock: realClock,
      fire: async (p) => {
        fired.push(p);
        return "ok";
      },
      note: (n) => notes.push(n),
      ...o,
    });
    return { s, notes, fired };
  }

  it("fires the configured engine's pick, noted with latency, size, tokens and probabilities", async () => {
    const { s, notes, fired } = make();
    s.text("I just got");
    s.text("I just got the job.");
    await s.idle();
    expect(fired).toEqual(["happy"]);
    expect(notes).toEqual([expect.objectContaining({ n: 5, e: "jev", pick: "happy", c: 0.9, tk: 400, ch: 19, fired: "ok", p: [null, 0.9, null, null, null, null] })]);
    expect(notes[0].ms).toEqual(expect.any(Number));
  });

  it("a timeout or error falls back to keywords, which fire (fallback reason noted); the listen is never blocked", async () => {
    const { s, notes, fired } = make({ timeoutMs: 50, fetch: (r) => new Promise((_, rej) => r.signal.addEventListener("abort", () => rej(new Error("x")))) });
    const t0 = Date.now();
    s.text("oh my god, no way.");
    expect(Date.now() - t0).toBeLessThan(20); // text() returns at once
    await s.idle();
    expect(fired).toEqual(["surprised"]);
    expect(notes.map((n) => [n.e, n.err ?? null, n.fb ?? null, n.fired ?? null])).toEqual([
      ["jev", "timeout", null, null],
      ["keywords", null, "timeout", "ok"],
    ]);
  });

  it("no key: keywords from the start, the reason on every decision", async () => {
    const { s, notes, fired } = make({ keys: {}, avail: { local: false, jev: false, openai: false } });
    expect([s.engine, s.fallback]).toEqual(["keywords", "no_key"]);
    s.text("haha that's hilarious.");
    await s.idle();
    expect(fired).toEqual(["laugh"]);
    expect(notes[0]).toMatchObject({ e: "keywords", fb: "no_key", fired: "ok" });
  });

  it("compare: every available engine runs on each chunk, only the configured one fires", async () => {
    let calls = 0;
    const { s, notes, fired } = make({
      mode: "openai",
      compare: true,
      avail: { local: true, jev: true, openai: true },
      keys: { jev: KEY_JEV, openai: KEY_OAI },
      fetch: async (r) => {
        calls++;
        return r.url.includes("openai") ? oaiAnswer("sad") : jevAnswer("happy");
      },
    });
    expect(s.engines).toEqual(["openai", "local", "jev", "keywords"]);
    expect(s.wantsLocal).toBe(true);
    s.text("my old dog died.");
    s.onLocal({ n: 4, engine: "local", pick: "sad", conf: 0.9, ms: 30, chars: 16 });
    await s.idle();
    expect(calls).toBe(2);
    expect(fired).toEqual(["sad"]);
    expect(notes.map((n) => n.e).sort()).toEqual(["jev", "keywords", "local", "openai"]);
    expect(notes.filter((n) => n.fired !== undefined)).toEqual([expect.objectContaining({ e: "openai", fired: "ok" })]);
  });

  it("local: waits for the box's verdict of that chunk; none in time → keywords; a late one is noted, never fired", async () => {
    const { s, notes, fired } = make({ mode: "local", avail: { local: true, jev: false, openai: false }, keys: {}, timeoutMs: 40 });
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
    expect(((await w.room.voiceSettings(w.link, { action: "get" })) as any).result.reflex).toMatchObject({ mode: "off", engine: null, engines: { local: false, jev: false, openai: false, keywords: true } });
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
    expect(t.relay.reflex_cfg).toEqual({ mode: "keywords", engine: "keywords" });
    expect(t.relay.reflex.map((n: ReflexNote) => [n.n, n.e, n.pick, n.fired])).toEqual([
      [6, "keywords", "surprised", "ok"], // six words
      [8, "keywords", "surprised", "rate_limited"], // the sentence end
      [13, "keywords", "laugh", "rate_limited"],
    ]);
    expect(t.relay.reflex[0]).toMatchObject({ ms: 0, ch: 20, at: expect.any(Number) });
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
    expect(Date.now() - reactedAt).toBeGreaterThan(REFLEX.gapMs); // the last chunk came after the gap
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
    expect((await trace(w, turn)).relay.reflex.map((n: ReflexNote) => n.fired)).toEqual(["screen", "screen"]);
    await w.kv.delete("paircur");
    await w.kv.put("upd", { job: "u", from: "1", to: "2", phase: "confirm", deadline: Date.now() + 60000 });
    turn = await w.shake({ words: words("oh my god I just got the job."), chunkMs: 30 }, "shake-2");
    await readUntil(w.room, w.a, (e) => e.type === "utterance" && e.data.turn_id === turn);
    expect((await trace(w, turn)).relay.reflex.map((n: ReflexNote) => n.fired)).toEqual(["screen", "screen"]);
    expect(cmds(w)).toEqual([]);
  });

  it("firmware without react / act caps: unsupported, nothing sent", async () => {
    const w = await setup({ caps: ["say", "say_cues", "listen", "listen_stop", "listen_window", "ack", "shake_talk", "mood"] });
    await set(w, { reflex: "keywords" });
    const turn = await w.shake({ words: words("oh my god I just got the job."), chunkMs: 30 });
    await readUntil(w.room, w.a, (e) => e.type === "utterance");
    expect((await trace(w, turn)).relay.reflex.map((n: ReflexNote) => n.fired)).toEqual(["unsupported", "unsupported"]);
    expect(cmds(w)).toEqual([]);
  });

  it("jev: one call per chunk with the relay's key; a slow call falls back to keywords at the timeout; the key is never logged or traced", async () => {
    const logs: string[] = [];
    const calls: Request[] = [];
    let slow = false;
    const w = await setup({
      reflexKeys: { jev: KEY_JEV },
      log: (m, d) => logs.push(m + JSON.stringify(d ?? {})),
      fetch: async (r) => {
        calls.push(r.clone());
        if (slow) return new Promise((_, rej) => r.signal.addEventListener("abort", () => rej(new Error("aborted"))));
        return Response.json({ model: "jev-1.14.0", answers: { reaction: { type: "choice", choice: "happy", confidence: 0.97, probabilities: { happy: 0.97 } } }, usage: { input_tokens: 398 } });
      },
    });
    expect(((await set(w, { reflex: "jev" })) as any).result.reflex).toMatchObject({ mode: "jev", engine: "jev", engines: { jev: true, openai: false } });
    let turn = await w.shake({ words: words("I just got the job."), chunkMs: 30 });
    await readUntil(w.room, w.a, (e) => e.type === "utterance");
    expect(calls.map((c) => [c.url, c.headers.get("authorization")])).toEqual([["https://api.typesafe.ai/v1/systemone", `Bearer ${KEY_JEV}`]]);
    expect(JSON.parse(await calls[0].text()).state).toEqual({ human_said_so_far: "I just got the job." });
    let t = await trace(w, turn);
    expect(t.relay.reflex).toEqual([expect.objectContaining({ e: "jev", pick: "happy", c: 0.97, tk: 398, fired: "ok" })]);
    await until(() => cmds(w).length === 2);
    expect(cmds(w)[0]).toEqual({ mood: { mood: "happy", hold_s: 0, react: true } });

    // the API hangs: the keywords' pick fires at the timeout, while the human still talks
    slow = true;
    await new Promise((r) => setTimeout(r, REFLEX.gapMs)); // past the reflex gap
    turn = await w.shake({ words: words("oh my god no way. " + "and then we went on and on about it for a long while more".repeat(1)), chunkMs: 60 }, "shake-2");
    await readUntil(w.room, w.a, (e) => e.type === "utterance" && e.data.turn_id === turn, [], 6000);
    t = await trace(w, turn);
    expect(t.relay.reflex.slice(0, 2).map((n: ReflexNote) => [n.e, n.err ?? null, n.fb ?? null, n.fired ?? null])).toEqual([
      ["jev", "timeout", null, null],
      ["keywords", null, "timeout", "ok"],
    ]);
    expect(t.relay.reflex[0].ms).toBeGreaterThanOrEqual(REFLEX.timeoutMs - 5);
    const all = JSON.stringify({ logs, act: await w.room.ownerActivity(w.link, { action: "get", query: {} }), view: await w.room.voiceSettings(w.link, { action: "get" }) });
    expect(all).not.toContain(KEY_JEV);
  }, 15000);

  it("local: the box classifies (stt.start reflex:true, its stt.reflex per chunk), no relay call", async () => {
    const calls: string[] = [];
    const w = await setup({
      reflexKeys: { jev: KEY_JEV },
      fetch: async (r) => (calls.push(r.url), new Response("{}")),
      box: { reflex: true, reflexFor: (text) => (text.endsWith(".") ? { n: text.split(" ").length, pick: "sad", conf: 0.88, p: { sad: 0.88, none: 0.1 }, cat: "other", cat_conf: 0.7, ms: 34, chars: text.length, tokens: 9 } : undefined) },
    });
    expect(((await set(w, { reflex: "auto" })) as any).result.reflex).toMatchObject({ mode: "auto", engine: "local", engines: { local: true, jev: true } });
    const turn = await w.shake({ words: words("my old dog died yesterday."), chunkMs: 30 });
    await readUntil(w.room, w.a, (e) => e.type === "utterance");
    expect(w.box.starts.at(-1)).toMatchObject({ reflex: true });
    const t = await trace(w, turn);
    expect(t.relay.reflex_cfg).toEqual({ mode: "auto", engine: "local" });
    expect(t.relay.reflex).toEqual([expect.objectContaining({ n: 5, e: "local", pick: "sad", c: 0.88, ms: 34, tk: 9, fired: "ok", p: [null, null, 0.88, null, null, 0.1], cat: "other", cc: 0.7 })]);
    expect(cmds(w)).toEqual([{ act: { action: "nod" } }]);
    expect(calls).toEqual([]);
    // the filler's seam: the reflex's own category for those words, no new call
    expect(await w.room.fillerCategory("my old dog died yesterday.")).toEqual({ category: "other", engine: "local", ms: 0 });
  });

  it("compare: all available engines on each chunk, the configured one fires exactly once", async () => {
    const urls: string[] = [];
    const w = await setup({
      reflexKeys: { jev: KEY_JEV, openai: KEY_OAI },
      fetch: async (r) => {
        urls.push(r.url);
        return r.url.includes("openai")
          ? Response.json({ model: "gpt-6-luna", answers: [{ type: "choice", name: "reaction", choice: "surprised", confidence: 0.8, probabilities: [{ value: "surprised", probability: 0.9 }] }], usage: { input_tokens: 150 } })
          : Response.json({ model: "jev-1.14.0", answers: { reaction: { type: "choice", choice: "happy", confidence: 0.9, probabilities: { happy: 0.9 } } }, usage: { input_tokens: 400 } });
      },
      box: { reflex: true, reflexFor: (text) => (text.endsWith(".") ? { n: text.split(" ").length, pick: "happy", conf: 0.7, ms: 30, chars: text.length } : undefined) },
    });
    const v = ((await set(w, { reflex: "openai", reflex_compare: true })) as any).result.reflex;
    expect(v).toMatchObject({ mode: "openai", compare: true, engine: "openai" });
    const turn = await w.shake({ words: words("guess what I got the job."), chunkMs: 30 });
    await readUntil(w.room, w.a, (e) => e.type === "utterance");
    expect(w.box.starts.at(-1)).toMatchObject({ reflex: true });
    const t = await trace(w, turn);
    expect(t.relay.reflex_cfg).toEqual({ mode: "openai", engine: "openai", compare: ["openai", "local", "jev", "keywords"] });
    expect(t.relay.reflex.map((n: ReflexNote) => n.e).sort()).toEqual(["jev", "keywords", "local", "openai"]);
    expect(t.relay.reflex.filter((n: ReflexNote) => n.fired !== undefined)).toEqual([expect.objectContaining({ e: "openai", pick: "surprised", fired: "ok" })]);
    expect(urls.sort()).toEqual(["https://api.openai.com/v1/decisions", "https://api.typesafe.ai/v1/systemone"]);
    await until(() => cmds(w).length === 2);
    expect(cmds(w)[0]).toEqual({ mood: { mood: "surprised", hold_s: 0, react: true } });
  });

  it("the owner setting: validated, logged, through REST with the link code", async () => {
    const w = await setup();
    expect(await set(w, { reflex: "loud" })).toMatchObject({ ok: false, error: "invalid_args" });
    expect(await set(w, { reflex_compare: "yes" })).toMatchObject({ ok: false, error: "invalid_args" });
    const r = await rest(w.backend, "", "POST", `/v1/devices/${DEVICE}/voice`, { reflex: "openai", reflex_compare: true }, { "x-molty-link-code": w.link });
    // no OpenAI key on this relay: keywords answer, saying why
    expect(r.body.result.reflex).toMatchObject({ mode: "openai", compare: true, engine: "keywords", fallback: "no_key", timeout_ms: REFLEX.timeoutMs });
    // no listen yet: the filler's category comes from the keywords (no key for the chosen engine)
    expect(await w.room.fillerCategory("could you turn on the lights")).toEqual({ category: "request", engine: "keywords", ms: 0 });
    expect((await rest(w.backend, "", "POST", `/v1/devices/${DEVICE}/voice`, { reflex: "off" }, { "x-molty-link-code": w.link })).body.result.reflex).toMatchObject({ mode: "off", compare: true, engine: null });
    const settings = ((await w.room.ownerActivity(w.link, { action: "get", query: { kind: "settings" } })) as any).result.entries.map((e: any) => e.data);
    expect(settings).toEqual(expect.arrayContaining([{ mode: "auto", voice: null, reflex: "openai", reflex_compare: true }, { mode: "auto", voice: null, reflex: "off", reflex_compare: true }]));
    expect((await rest(w.backend, "", "POST", `/v1/devices/${DEVICE}/voice`, { reflex: "jev" }, { "x-molty-link-code": "WRONG234" })).status).toBe(401);
  });
});

// keep the verdict type in use for readers of this file
export type _V = ReflexVerdict;
