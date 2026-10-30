// Inline [mood] / [action] cues in say text (parseCues, validation, the device's `marks`)
// and the agent gesture command `act`.
import { describe, expect, it } from "vitest";
import type { RelayBackend } from "../src/core/api.js";
import { ACTIONS, MAX_SAY_MARKS, validateAsk, validateCommand, validateTalk } from "../src/core/commands.js";
import { decodeAudioFrame, encodeAudioFrame } from "../src/core/protocol.js";
import type { DeviceRoom } from "../src/core/room.js";
import { parseCues, speechWords } from "../src/core/speech.js";
import type { TtsOptions } from "../src/voice/types.js";
import { handleMcp } from "../src/surfaces/mcp.js";
import { handleRest } from "../src/surfaces/rest.js";
import { DEVICE, type FakeSocket, FakeVoice, LINK, connect, makeRoom } from "./helpers.js";

const FAST = { cmdTimeoutMs: 300, pairWindowMs: 400, sayChunkSamples: 1600, sayLeadFrames: 2, listenGraceMs: 200, sayDoneGraceMs: 300 };
const CAPS = ["say", "say_cues", "say_marks", "listen", "act", "ask"];

/** A device that answers cmds, "plays" say streams (say.done on the end frame), streams mic audio for listen and taps option 0 of an ask. */
function device(room: DeviceRoom, sock: FakeSocket) {
  const log: Array<{ at: number; kind: string; name?: string; args?: any; head?: any; bytes?: number }> = [];
  const origText = sock.sendText.bind(sock);
  const origBin = sock.sendBinary.bind(sock);
  sock.sendText = (t: string) => {
    origText(t);
    const f = JSON.parse(t);
    if (f.t === "pair.request") setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "pair.result", req: f.req, ok: true, link_code: "NEWC2345" })), 2);
    if (f.t !== "cmd") return;
    log.push({ at: Date.now(), kind: "cmd", name: f.name, args: f.args });
    setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "res", id: f.id, ok: true, result: {} })), 1);
    if (f.name === "listen") {
      setTimeout(() => {
        const pcm = new Uint8Array(new Int16Array(3200).fill(8000).buffer);
        void room.socketMessage(sock, encodeAudioFrame({ s: f.args.job, seq: 0, kind: "pcm16", rate: 16000, end: false }, pcm));
        void room.socketMessage(sock, new TextEncoder().encode(JSON.stringify({ s: f.args.job, seq: 1, kind: "pcm16", rate: 16000, end: true, reason: "silence", speech_ms: 150 })));
      }, 20);
    }
    if (f.name === "ask") setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "evt", name: "answer", data: { job: f.args.job, index: 0 } })), 10);
  };
  sock.sendBinary = (b: Uint8Array) => {
    origBin(b);
    const fr = decodeAudioFrame(b)!;
    const nl = b.indexOf(10);
    const head = JSON.parse(new TextDecoder().decode(nl < 0 ? b : b.subarray(0, nl)));
    log.push({ at: Date.now(), kind: "frame", head, bytes: fr.payload.byteLength });
    if (fr.header.end) setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "evt", name: "say.done", data: { job: fr.header.s } })), 5);
  };
  const cmds = (name: string) => log.filter((l) => l.kind === "cmd" && l.name === name).map((l) => l.args);
  /** word -> sample, from every frame's word cues */
  const cues = () => new Map(log.filter((l) => l.kind === "frame").flatMap((l) => (l.head.w ?? []) as Array<[number, number]>));
  return { log, cmds, cues };
}

async function setup(opts: { caps?: string[]; fw?: string; voice?: FakeVoice } = {}) {
  const ctx = makeRoom({ timings: FAST, voice: opts.voice });
  const sock = await connect(ctx.room, ctx.host, undefined, LINK, { caps: opts.caps ?? CAPS, ...(opts.fw ? { fw: opts.fw } : {}) });
  const dev = device(ctx.room, sock);
  const out = await ctx.room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "cuer" });
  if (!out.ok || out.result.status !== "approved") throw new Error(JSON.stringify(out));
  const backend: RelayBackend = { device: () => ctx.room };
  return { ...ctx, sock, dev, token: out.result.agent_token, backend };
}

/** A provider with its own word timing (like the voice box): word i of a segment starts at 0.1·i s. */
class TimedVoice extends FakeVoice {
  async *ttsStream(text: string, opts: TtsOptions) {
    this.ttsCalls.push({ text, voice: opts.voice });
    speechWords(text).forEach((_, i) => opts.onWord?.(i, 0.1 * i));
    yield new Int16Array(this.ttsSamples).fill(1000);
  }
}

let rid = 0;
const mcpRaw = async (b: RelayBackend, token: string, method: string, params: unknown) =>
  (await (
    await handleMcp(
      new Request("https://relay.test/mcp", {
        method: "POST",
        headers: { "content-type": "application/json", authorization: `Bearer ${token}` },
        body: JSON.stringify({ jsonrpc: "2.0", id: ++rid, method, params }),
      }),
      b,
    )
  ).json()) as any;
const mcp = async (b: RelayBackend, token: string, name: string, args: unknown) => (await mcpRaw(b, token, "tools/call", { name, arguments: args })).result;

const say = async (w: Awaited<ReturnType<typeof setup>>, text: string) => {
  const out = await w.room.command(w.token, "say", { text });
  if (!out.ok) throw new Error(JSON.stringify(out));
  return w.room.job(w.token, (out.result as any).job, 3000);
};
const screenRev = (w: Awaited<ReturnType<typeof setup>>) => (w.room as any).screenRev as number;

describe("parseCues", () => {
  it("strips cues from the text and marks the word that follows each one", () => {
    expect(parseCues("[happy] Build passed! [thinking] Three tests were flaky… [surprised] wait, one is new.")).toEqual({
      text: "Build passed! Three tests were flaky… wait, one is new.",
      marks: [[0, "happy"], [2, "thinking"], [6, "surprised"]],
    });
  });

  it("word index: start, middle, mid-word (next word), trailing (= word count), several on one word", () => {
    expect(parseCues("[nod] a b c").marks).toEqual([[0, "nod"]]);
    expect(parseCues("a b [nod] c").marks).toEqual([[2, "nod"]]);
    expect(parseCues("great[happy]! go")).toEqual({ text: "great! go", marks: [[1, "happy"]] });
    expect(parseCues("gr[happy]eat go")).toEqual({ text: "great go", marks: [[1, "happy"]] });
    expect(parseCues("a b c [yawn]")).toEqual({ text: "a b c", marks: [[3, "yawn"]] });
    expect(parseCues("a [happy] [nod] [wiggle]b")).toEqual({ text: "a b", marks: [[1, "happy"], [1, "nod"], [1, "wiggle"]] });
    expect(parseCues("[happy]Hi").marks).toEqual([[0, "happy"]]);
  });

  it("unknown cue names are stripped without a mark", () => {
    expect(parseCues("Hi [laughs] there [music]")).toEqual({ text: "Hi there", marks: [] });
    expect(parseCues("[ ] x")).toEqual({ text: "x", marks: [] });
  });

  it("malformed / lone brackets are dropped, the text after them stays", () => {
    expect(parseCues("a [b c")).toEqual({ text: "a b c", marks: [] });
    expect(parseCues("a ] b")).toEqual({ text: "a b", marks: [] });
    expect(parseCues("[]x")).toEqual({ text: "x", marks: [] });
    expect(parseCues("a [abc[nod] b")).toEqual({ text: "a abc b", marks: [[2, "nod"]] });
    // too long (25 chars inside) is no cue: both brackets dropped, the inside kept
    expect(parseCues(`x [${"a".repeat(25)}] y`)).toEqual({ text: `x ${"a".repeat(25)} y`, marks: [] });
    expect(parseCues(`x [${"a".repeat(24)}] y`)).toEqual({ text: "x y", marks: [] });
    // a line break ends a candidate
    expect(parseCues("a [nod\n] b")).toEqual({ text: "a nod\nb", marks: [] });
  });

  it("word indexes count the returned text: leading Unicode spaces trimmed away are no word", () => {
    expect(parseCues("\u00a0 [happy] Hello there")).toEqual({ text: "Hello there", marks: [[0, "happy"]] });
    expect(parseCues("\u2003a [nod] b")).toEqual({ text: "a b", marks: [[1, "nod"]] });
  });

  it("escapes: \\[ and \\] are literal brackets (spoken and shown); other backslashes are literal", () => {
    expect(parseCues("say \\[happy\\] literally")).toEqual({ text: "say [happy] literally", marks: [] });
    expect(parseCues("a\\b \\n [nod] c")).toEqual({ text: "a\\b \\n c", marks: [[2, "nod"]] });
    // an escaped closing bracket closes no cue: the opening one is dropped
    expect(parseCues("[nod\\] x")).toEqual({ text: "nod] x", marks: [] });
  });

  it("whitespace: no doubled spaces where cues were, trimmed, newlines kept", () => {
    expect(parseCues("passed! [thinking] Three").text).toBe("passed! Three");
    expect(parseCues("[happy] Hi").text).toBe("Hi");
    expect(parseCues("Hi [nod]").text).toBe("Hi");
    expect(parseCues("a  [nod]  b").text).toBe("a  b");
    expect(parseCues("one [nod]\ntwo\n\nthree").text).toBe("one\ntwo\n\nthree");
    expect(parseCues("one [nod]\ntwo").marks).toEqual([[1, "nod"]]);
    expect(parseCues("no cues  here\n ok").text).toBe("no cues  here\n ok");
  });

  it("names: trimmed, lowercased, spaces / hyphens → _ (moods and actions)", () => {
    expect(parseCues("[Shake Head] a [shake-head] b [ HAPPY ] c [glance - left] d").marks).toEqual([
      [0, "shake_head"],
      [1, "shake_head"],
      [2, "happy"],
      [3, "glance_left"],
    ]);
    for (const a of ACTIONS) expect(parseCues(`[${a}] x`).marks).toEqual([[0, a]]);
  });

  it(`at most ${MAX_SAY_MARKS} marks (the first ones); exact duplicates dropped`, () => {
    const many = Array.from({ length: 20 }, (_, i) => `[nod] w${i}`).join(" ");
    const p = parseCues(many);
    expect(p.marks).toHaveLength(MAX_SAY_MARKS);
    expect(p.marks.map((m) => m[0])).toEqual(Array.from({ length: MAX_SAY_MARKS }, (_, i) => i));
    expect(p.text).toBe(Array.from({ length: 20 }, (_, i) => `w${i}`).join(" "));
    expect(parseCues("[nod][nod] [Nod] x [nod] y").marks).toEqual([[0, "nod"], [1, "nod"]]);
  });
});

describe("cue validation (say / talk / ask)", () => {
  it("the forwarded text is stripped; marks ride relay-only; raw bytes still limited", () => {
    expect(validateCommand("say", { text: "[happy] Hi there [nod]" }).args).toEqual({ text: "Hi there", marks: [[0, "happy"], [2, "nod"]] });
    expect(validateCommand("say", { text: "Hi" }).args).toEqual({ text: "Hi" });
    expect(validateTalk({ say: "[happy] Hi" })).toMatchObject({ say: "Hi", marks: [[0, "happy"]] });
    expect(validateTalk({ say: "Hi" })).not.toHaveProperty("marks");
    expect(validateAsk({ say: "[thinking] Ship it?", options: ["yes"] })).toMatchObject({ say: "Ship it?", marks: [[0, "thinking"]] });
    // the byte limit applies to the raw text (cues included)
    expect(() => validateCommand("say", { text: `[happy] ${"a".repeat(4090)}` })).toThrow(/exceeds 4096 bytes/);
  });

  it("nothing left to speak after the cues: invalid_args pointing at mood / act", () => {
    for (const f of [() => validateCommand("say", { text: "[happy] [nod]" }), () => validateTalk({ say: "[happy]" }), () => validateAsk({ say: " [nod] ", options: ["ok"] })]) {
      expect(f).toThrow(/nothing to speak after removing its \[cues\]: use mood or act/);
      try {
        f();
      } catch (e: any) {
        expect(e.code).toBe("invalid_args");
      }
    }
  });
});

describe("say cues on the device", () => {
  it("say: TTS and the device text carry no cue; marks go out with their word indexes; each mark's word has a word cue; result text stripped", async () => {
    const w = await setup();
    w.voice.ttsSamples = 16000;
    const done = await say(w, "[happy] Build passed! [nod] All [wiggle]good. [yawn]");
    const text = "Build passed! All good.";
    expect(done).toMatchObject({ ok: true, result: { status: "done", result: { text } } });
    expect(w.voice.ttsCalls.map((c) => c.text).join(" ")).toBe(text);
    expect(w.voice.ttsCalls.some((c) => /[[\]]|happy|nod|wiggle|yawn/.test(c.text))).toBe(false);
    const cmd = w.dev.cmds("say")[0];
    expect(cmd.text).toBe(text);
    expect(cmd.cues).toBe(true);
    expect(cmd.marks).toEqual([[0, "happy"], [2, "nod"], [3, "wiggle"], [4, "yawn"]]);
    const cues = w.dev.cues();
    const words = speechWords(text).length;
    expect([...cues.keys()].sort()).toEqual([0, 1, 2, 3]);
    for (const [word] of cmd.marks as Array<[number, string]>) {
      if (word < words) expect(cues.has(word)).toBe(true);
      else expect(word).toBe(words); // trailing: fires when playback ends
    }
    // no separate mood / act cmds for cues
    expect(w.dev.cmds("mood")).toEqual([]);
    expect(w.dev.cmds("act")).toEqual([]);
  });

  it("estimated cues: a mark fires at its word's cue, in proportion to characters", async () => {
    const w = await setup();
    w.voice.ttsSamples = 16000;
    await say(w, "hi there [surprised] wonderful world");
    const mark = w.dev.cmds("say")[0].marks[0];
    expect(mark).toEqual([2, "surprised"]);
    // chars before "wonderful" (+1 per word): 9 of 25 → of 16000 samples (talk.test.ts word cues)
    expect(w.dev.cues().get(mark[0])).toBe(5760);
  });

  it("a provider's own word timing: a mark's word cue is that word's timed start", async () => {
    const voice = new TimedVoice();
    voice.ttsSamples = 16000;
    const w = await setup({ voice });
    await say(w, "one two [nod] three four");
    const [[word, name]] = w.dev.cmds("say")[0].marks;
    expect([word, name]).toEqual([2, "nod"]);
    expect(w.dev.cues().get(word)).toBe(Math.round(0.1 * 2 * 16000));
  });

  it("firmware without say_marks (or without say_cues): no marks arg, cues still stripped", async () => {
    for (const caps of [["say", "say_cues", "listen"], ["say", "say_marks", "listen"], ["say", "listen"]]) {
      const w = await setup({ caps });
      await say(w, "[happy] Hi [nod] there");
      const cmd = w.dev.cmds("say")[0];
      expect(cmd).not.toHaveProperty("marks");
      expect(cmd.text).toBe("Hi there");
      expect(w.voice.ttsCalls.map((c) => c.text)).toEqual(["Hi there"]);
    }
  });

  it("a say without cues sends no marks", async () => {
    const w = await setup();
    await say(w, "Hi there");
    expect(w.dev.cmds("say")[0]).not.toHaveProperty("marks");
  });

  it("talk: stripped say spoken + shown, marks sent, said stripped; the listen's automatic moods still happen; no screenRev bump", async () => {
    const w = await setup();
    w.voice.ttsSamples = 1600;
    const rev = screenRev(w);
    const r = await mcp(w.backend, w.token, "molty_talk", { say: "[happy] Hi [nod] there." });
    expect(r.isError).toBe(false);
    expect(r.structuredContent).toMatchObject({ status: "done", said: "Hi there.", heard: "hello molty" });
    expect(w.voice.ttsCalls.map((c) => c.text)).toEqual(["Hi there."]);
    const cmd = w.dev.cmds("say")[0];
    expect(cmd.text).toBe("Hi there.");
    expect(cmd.marks).toEqual([[0, "happy"], [1, "nod"]]);
    await new Promise((r) => setTimeout(r, 20));
    // automatic thinking at end of speech and after STT: the cues did not count as an explicit mood
    expect(w.dev.cmds("mood")).toEqual([{ mood: "thinking", hold_s: 15 }, { mood: "thinking", hold_s: 15 }]);
    expect(w.dev.cmds("act")).toEqual([]);
    expect(screenRev(w)).toBe(rev);
  });

  it("ask: stripped question spoken + shown, marks sent, said stripped", async () => {
    const w = await setup();
    w.voice.ttsSamples = 1600;
    const r = await mcp(w.backend, w.token, "molty_ask", { say: "[thinking] Ship [nod] it?", options: ["yes", "no"] });
    expect(r.structuredContent).toMatchObject({ status: "done", said: "Ship it?", answer: "yes" });
    expect(w.voice.ttsCalls.map((c) => c.text)).toEqual(["Ship it?"]);
    expect(w.dev.cmds("say")[0]).toMatchObject({ text: "Ship it?", marks: [[0, "thinking"], [1, "nod"]] });
  });

  it("say does not bump screenRev for its cues", async () => {
    const w = await setup();
    const rev = screenRev(w);
    await say(w, "[happy] Hi [nod] there");
    expect(screenRev(w)).toBe(rev);
  });
});

describe("act", () => {
  it("validation: only {action}, one of ACTIONS", () => {
    expect(validateCommand("act", { action: "nod" })).toEqual({ name: "act", args: { action: "nod" }, job: false });
    expect(() => validateCommand("act", { action: "dance" })).toThrow(/action must be one of wiggle, nod/);
    expect(() => validateCommand("act", {})).toThrow(/action must be one of/);
    expect(() => validateCommand("act", { action: "nod", hold_s: 1 })).toThrow(/unknown argument hold_s/);
  });

  it("forwarded as cmd act {action}; never touches screenRev; a listen's automatic thinking still lands after it", async () => {
    const w = await setup();
    const rev = screenRev(w);
    expect(await w.room.command(w.token, "act", { action: "wiggle" })).toEqual({ ok: true, result: {} });
    expect(w.dev.cmds("act")).toEqual([{ action: "wiggle" }]);
    expect(screenRev(w)).toBe(rev);
    // during STT: an act is not an explicit mood, the automatic thinking is re-asserted
    const stt = w.voice.stt.bind(w.voice);
    w.voice.stt = async (pcm) => {
      await new Promise((r) => setTimeout(r, 80));
      return stt(pcm);
    };
    const turn = w.room.talk(w.token, {});
    const end = Date.now() + 1000;
    while (w.dev.cmds("mood").length === 0 && Date.now() < end) await new Promise((r) => setTimeout(r, 5));
    expect(await w.room.command(w.token, "act", { action: "nod" })).toMatchObject({ ok: true });
    await turn;
    await new Promise((r) => setTimeout(r, 20));
    expect(w.dev.cmds("mood")).toEqual([{ mood: "thinking", hold_s: 15 }, { mood: "thinking", hold_s: 15 }]);
  });

  it("REST /cmd {name:'act'} and MCP molty_act go through", async () => {
    const w = await setup();
    const r = await handleRest(
      new Request(`https://relay.test/v1/devices/${DEVICE}/cmd`, { method: "POST", headers: { authorization: `Bearer ${w.token}`, "content-type": "application/json" }, body: JSON.stringify({ name: "act", args: { action: "blink" } }) }),
      w.backend,
    );
    expect(r.status).toBe(200);
    expect(((await r.json()) as any).ok).toBe(true);
    const m = await mcp(w.backend, w.token, "molty_act", { action: "look_up" });
    expect(m.isError).toBe(false);
    expect(w.dev.cmds("act")).toEqual([{ action: "blink" }, { action: "look_up" }]);
    const bad = await mcp(w.backend, w.token, "molty_act", { action: "dance" });
    expect(bad.structuredContent).toMatchObject({ error: "invalid_args" });
  });

  it("caps without act: unsupported; older firmware: firmware_too_old; nothing sent", async () => {
    const w = await setup({ caps: ["say", "listen"] });
    expect(await w.room.command(w.token, "act", { action: "nod" })).toMatchObject({ ok: false, error: "unsupported" });
    const old = await setup({ caps: ["say", "listen"], fw: "0.1.0" });
    expect(await old.room.command(old.token, "act", { action: "nod" })).toMatchObject({ ok: false, error: "firmware_too_old", details: { feature: "molty_act", needed: "0.2.0" } });
    expect([...w.dev.cmds("act"), ...old.dev.cmds("act")]).toEqual([]);
  });

  it("MCP tools/list has molty_act with the action list; say / talk / ask describe inline cues", async () => {
    const w = await setup();
    const tools = (await mcpRaw(w.backend, w.token, "tools/list", {})).result.tools as any[];
    const act = tools.find((t) => t.name === "molty_act");
    expect(act.inputSchema.required).toEqual(["action"]);
    expect(act.inputSchema.properties.action.enum).toEqual([...ACTIONS]);
    for (const a of ACTIONS) expect(act.description).toContain(a);
    for (const n of ["molty_say", "molty_talk", "molty_ask"]) expect(tools.find((t) => t.name === n).description).toMatch(/\[happy\].*\\\[/);
  });
});
