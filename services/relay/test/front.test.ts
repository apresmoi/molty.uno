// The voice front (core/front.ts, docs/AGENT-LINK.md "Voice front"): a voice box whose engine is a
// realtime voice model hears the person, ends their turn, answers at once; the agent stays the inner
// voice (utterance with `front`, its say / talk spoken by the front), hands-free turns, the owner's
// switch, and every way a conversation ends. The box here is a fake speaking the box side of the wire.
import { describe, expect, it } from "vitest";
import type { AgentEvent } from "../src/core/events.js";
import { decodeAudioFrame, encodeAudioFrame } from "../src/core/protocol.js";
import type { DeviceRoom } from "../src/core/room.js";
import { FRONT_ROLE, FrontTalk, frontTurnSummary, stopPhrase } from "../src/core/front.js";
import { DEVICE, type FakeHost, type FakeSocket, LINK, connect, makeRoom } from "./helpers.js";

const FAST = { cmdTimeoutMs: 300, pairWindowMs: 400, sayChunkSamples: 1600, sayLeadFrames: 2, listenGraceMs: 200, sayDoneGraceMs: 300, boxSttFinalMs: 400, frontWaitMs: 150, frontSaidWaitMs: 300 };
const CAPS = ["say", "say_cues", "say_marks", "listen", "listen_stop", "listen_window", "ack", "shake_talk", "dnd"];
const NEXT = ["NEWC2345", "NEWD2345", "NEWE2345", "NEWF2345"];

/** A device: answers cmds, streams 100 ms mic chunks every 15 ms for a listen (`chunks`, then its own end), stops on listen_stop, says say.done. */
function device(room: DeviceRoom, sock: FakeSocket, o: { chunks?: number } = {}) {
  const cmds: Array<{ name: string; args: any; at: number }> = [];
  const says: Record<string, number> = {};
  const ends: Record<string, number> = {}; // when each say's end frame reached the device
  let timers: ReturnType<typeof setTimeout>[] = [];
  let codes = 0;
  const text = sock.sendText.bind(sock);
  const bin = sock.sendBinary.bind(sock);
  sock.sendText = (t: string) => {
    text(t);
    const f = JSON.parse(t);
    if (f.t === "pair.request") setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "pair.result", req: f.req, ok: true, link_code: NEXT[codes++] })), 2);
    if (f.t !== "cmd") return;
    cmds.push({ name: f.name, args: f.args, at: Date.now() });
    const result = f.name === "listen" ? { job: f.args.job, ms: 30000, ack: "chirp" } : f.name === "listen_stop" ? { job: f.args.job, stopping: true } : {};
    setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "res", id: f.id, ok: true, result })), 1);
    if (f.name === "listen") {
      const job = f.args.job;
      let seq = 0;
      timers = [];
      const n = o.chunks ?? 6;
      for (let i = 0; i < n; i++) timers.push(setTimeout(() => void room.socketMessage(sock, encodeAudioFrame({ s: job, seq: seq++, kind: "pcm16", rate: 16000, end: false }, new Uint8Array(new Int16Array(1600).fill(6000).buffer))), 10 + 15 * i));
      timers.push(setTimeout(() => void room.socketMessage(sock, new TextEncoder().encode(JSON.stringify({ s: job, seq: seq++, kind: "pcm16", rate: 16000, end: true, reason: "silence", speech_ms: 400 }))), 20 + 15 * n));
    }
    if (f.name === "listen_stop") {
      for (const x of timers) clearTimeout(x);
      timers = [];
      setTimeout(() => void room.socketMessage(sock, new TextEncoder().encode(JSON.stringify({ s: f.args.job, seq: 90, kind: "pcm16", rate: 16000, end: true, reason: f.args.reason ?? "turn_end" }))), 3);
    }
  };
  sock.sendBinary = (b: Uint8Array) => {
    bin(b);
    const fr = decodeAudioFrame(b)!;
    says[fr.header.s] = (says[fr.header.s] ?? 0) + fr.payload.byteLength / 2;
    if (fr.header.end) ends[fr.header.s] = Date.now();
    if (fr.header.end) setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "evt", name: "say.done", data: { job: fr.header.s } })), 5);
  };
  return { cmds, says, ends, named: (n: string) => cmds.filter((c) => c.name === n) };
}

interface BoxOpts {
  /** offer the front in the hello (a realtime box); false: an older or kyutai box */
  front?: boolean;
  /** the person's words for each listen, in order ("" = nobody spoke: no_speech; default: a question, then nobody) */
  heard?: string[];
  /** the front's own reply to a turn: its words and whether it hands the turn to the agent */
  reply?: (heard: string) => { said: string; wait: boolean };
  /** turn end after this many mic chunks */
  turnEndAfter?: number;
  /** the front keeps an inner voice thought to itself (it repeats what was said): no audio, tts.end absorbed */
  absorb?: (thought: string) => boolean;
}

/** A realtime voice box on the voice socket, as molty-voice's realtime engine speaks it. */
function frontBox(room: DeviceRoom, sock: FakeSocket, o: BoxOpts = {}) {
  const got: any[] = [];
  const listens: Record<string, { n: number; front: boolean; hold: boolean; heard: string; turned: boolean }> = {};
  let k = 0;
  const say = (m: unknown) => void room.socketMessage(sock, JSON.stringify(m));
  const speak = (id: string, words: string, end: Record<string, unknown> = {}, delay = 5) => {
    setTimeout(() => {
      if (sock.closed) return;
      say({ t: "tts.text", id, text: words.split(" ").slice(0, 2).join(" ") });
      void room.socketMessage(sock, encodeAudioFrame({ s: id, seq: 0, kind: "pcm16", rate: 16000, end: false }, new Uint8Array(new Int16Array(3200).fill(700).buffer)));
      words.split(" ").forEach((_, i) => say({ t: "tts.word", id, i, text: "w", start_s: 0.1 * i }));
      say({ t: "tts.text", id, text: words, final: true });
      say({ t: "tts.end", id, ms: { first_audio: 300 }, ...end });
    }, delay);
  };
  const text = sock.sendText.bind(sock);
  sock.sendText = (t: string) => {
    text(t);
    const f = JSON.parse(t);
    got.push(f);
    if (f.t === "stt.start") listens[f.id] = { n: 0, front: f.front === true, hold: f.hold === true, heard: o.heard?.[Object.keys(listens).length] ?? (Object.keys(listens).length ? "" : "what is on my calendar today?"), turned: false };
    // Molty ended the turn itself (held: the release): the box commits it and its front answers now
    const l = f.t === "stt.end" ? listens[f.id] : undefined;
    if (l && l.front && l.heard && !l.turned) {
      l.turned = true;
      say({ t: "front.reply", id: `fr${++k}`, stt: f.id });
      speak(`fr${k}`, (o.reply ?? (() => ({ said: "Hi there!", wait: false })))(l.heard).said, {}, 20);
    }
    if (f.t === "stt.end") setTimeout(() => !sock.closed && listens[f.id] && say({ t: "stt.final", id: f.id, text: listens[f.id].heard, ms: { final: 2 } }), 3);
    if (f.t === "tts" && f.front === "inner" && o.absorb?.(f.text)) setTimeout(() => say({ t: "tts.end", id: f.id, absorbed: true }), 5);
    else if (f.t === "tts") {
      const words = f.front === "inner" && !f.exact ? `So, ${f.text}` : f.front === "wait" ? "Still checking, one moment." : f.text;
      speak(f.id, words);
    }
  };
  sock.sendBinary = (b: Uint8Array) => {
    const fr = decodeAudioFrame(b)!;
    const l = listens[fr.header.s];
    if (!l || l.turned) return;
    l.n++;
    if (l.hold) return; // held to talk: no turn detection, the release ends it
    if (l.n === 1 && l.heard) say({ t: "stt.hint", id: fr.header.s, k: "speech" });
    if (!l.heard && l.n === 3) {
      l.turned = true;
      say({ t: "stt.turn_end", id: fr.header.s, reason: "no_speech" });
    } else if (l.heard && l.n === (o.turnEndAfter ?? 3)) {
      l.turned = true;
      say({ t: "stt.turn_end", id: fr.header.s });
      if (l.front) {
        const r = (o.reply ?? ((h: string) => (h.includes("?") ? { said: "Let me check on that.", wait: true } : { said: "Hi there!", wait: false })))(l.heard);
        const id = `fr${++k}`;
        say({ t: "front.reply", id, stt: fr.header.s, ms: { created: 180 } });
        speak(id, r.said, r.wait ? { wait: true } : {}, 20);
      }
    }
  };
  const hello = (front = o.front ?? true) =>
    room.socketMessage(sock, JSON.stringify({ t: "voice.hello", name: "laptop", version: "0.2.0", voices: ["marin"], stt: true, tts: true, engine: "realtime", ...(front ? { front: 1 } : {}) }));
  return { got, listens, hello, of: (t: string) => got.filter((m) => m.t === t) };
}

async function setup(o: { box?: BoxOpts; front?: "on" | "off"; chunks?: number; caps?: string[] } = {}) {
  const ctx = makeRoom({ timings: FAST, log: process.env.FRONT_DEBUG ? (m, d) => console.log(m, JSON.stringify(d)) : undefined });
  const sock = await connect(ctx.room, ctx.host, undefined, LINK, { caps: o.caps ?? CAPS });
  const dev = device(ctx.room, sock, { chunks: o.chunks });
  const pair = async (link: string, name: string, role?: "voice") => {
    const out = await ctx.room.pair({ device_id: DEVICE, link_code: link, agent_name: name, ...(role ? { role } : {}) });
    if (!out.ok || out.result.status !== "approved") throw new Error(JSON.stringify(out));
    return out.result.agent_token as string;
  };
  const token = await pair(LINK, "agent");
  const voiceToken = await pair(NEXT[0], "laptop", "voice");
  const box = await openBox(ctx.room, ctx.host, voiceToken, o.box);
  const owner = NEXT[1];
  if (o.front !== "off") expect((await ctx.room.voiceSettings(owner, { action: "set", front: "on" })).ok).toBe(true);
  const shake = (n = 1) => ctx.room.socketMessage(sock, JSON.stringify({ t: "evt", name: "shake", data: { peak_g: 2.4, reversals: 5, ms: 900, listen: `shake-${n}` } }));
  // every event read so far (a read moves the agent's cursor)
  const seen: AgentEvent[] = [];
  const inbox = async () => {
    seen.push(...(((await ctx.room.events(token, { wait_ms: 0 })) as any).result.events as AgentEvent[]));
    return seen;
  };
  return { ...ctx, sock, dev, token, voiceToken, box, owner, shake, inbox };
}

async function openBox(room: DeviceRoom, host: FakeHost, token: string, o: BoxOpts = {}) {
  const auth = await room.voiceAuth(token);
  if (!auth.ok) throw new Error(JSON.stringify(auth));
  const sock = host.open();
  room.voiceOpen(sock, auth.result.hash);
  const b = frontBox(room, sock, o);
  await b.hello();
  return { sock, ...b };
}

const until = async (cond: () => boolean | Promise<boolean>, ms = 2000) => {
  const end = Date.now() + ms;
  while (!(await cond()) && Date.now() < end) await new Promise((r) => setTimeout(r, 5));
};
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
const sayTexts = (dev: ReturnType<typeof device>) => dev.named("say").map((c) => c.args.text);

describe("voice front: the owner's switch", () => {
  it("is off by default: the box serves STT / TTS as today, the view says why the front is not serving", async () => {
    const ctx = await setup({ front: "off" });
    let v = ((await ctx.room.voiceSettings(ctx.owner, { action: "get" })) as any).result;
    expect(v.front).toEqual({ mode: "off", available: true, serving: false, conversation: false, engine: "realtime" });
    await ctx.shake();
    await until(() => ctx.box.of("stt.final").length > 0 || ctx.box.of("stt.start").length > 0);
    await until(async () => (await ctx.inbox()).some((e) => e.type === "utterance"));
    expect(ctx.box.of("stt.start")[0].front).toBeUndefined();
    expect(ctx.box.of("front.reply").length + ctx.box.of("tts").filter((t) => t.front).length).toBe(0);
    // turned on without a box that offers it: unavailable, in plain words; nothing is sent to any box
    expect((await ctx.room.voiceSettings(ctx.owner, { action: "set", front: "maybe" })).ok).toBe(false);
    await ctx.box.hello(false);
    v = ((await ctx.room.voiceSettings(ctx.owner, { action: "set", front: "on" })) as any).result;
    expect(v.front).toMatchObject({ mode: "on", available: false, serving: false });
    expect(v.front.reason).toMatch(/offers no voice front/);
  });

  it("the kill switch: turning it off ends the conversation and closes the box's session at once", async () => {
    const ctx = await setup({ box: { reply: () => ({ said: "Hi!", wait: false }) } });
    await ctx.shake();
    await until(() => ctx.dev.named("say").length > 0);
    expect(((await ctx.room.voiceSettings(ctx.owner, { action: "get" })) as any).result.front.conversation).toBe(true);
    await ctx.room.voiceSettings(ctx.owner, { action: "set", front: "off" });
    expect(ctx.box.of("front.end").map((m) => m.reason)).toContain("front_off");
    expect(((await ctx.room.voiceSettings(ctx.owner, { action: "get" })) as any).result.front.conversation).toBe(false);
    // the hands-free listen that may still start uses no front
    await sleep(150);
    const starts = ctx.box.of("stt.start");
    expect(starts.slice(1).every((s) => s.front === undefined)).toBe(true);
  });
});

describe("voice front: a turn", () => {
  it("the model ends the turn, the front answers at once, the agent gets the utterance with what it said, then speaks through it", async () => {
    const ctx = await setup();
    await ctx.shake();
    await until(() => ctx.dev.named("say").length > 0);
    // the box got the listen as a front listen; the device's own VAD is off: the model ends the turn
    const start = ctx.box.of("stt.start")[0];
    expect(start.front).toBe(true);
    const adopt = ctx.dev.named("listen")[0];
    expect(adopt.args.vad).toBe(false);
    expect(ctx.dev.named("listen_stop")[0].args.reason).toBe("turn_end");
    // the front's reply plays; the device shows what the front says (its transcript), not a TTS of anything
    expect(sayTexts(ctx.dev)[0]).toBe("Let me check on that.");
    expect(ctx.dev.named("listen_stop")[0].at).toBeLessThanOrEqual(ctx.dev.named("say")[0].at);
    await until(async () => (await ctx.inbox()).some((e) => e.type === "utterance"));
    const u = (await ctx.inbox()).find((e) => e.type === "utterance")!;
    expect(u.data).toMatchObject({ text: "what is on my calendar today?", trigger: "shake", front: { said: "Let me check on that.", waiting: true, role: FRONT_ROLE } });
    expect(u.data.front.role).toMatch(/^you are the inner voice/);
    expect(u.data.filler_in_ms).toBeUndefined(); // no auto filler: the front answered
    // the inner voice: no TTS request, the box's front says it in its own words
    const r = (await ctx.room.talk(ctx.token, { say: "Standup at 10 and a dentist at 3.", listen: false, in_reply_to: u.data.turn_id })) as any;
    expect(r.ok).toBe(true);
    // one voice: the waiting turn's answer, said at once by the front; the agent is told it is not the speaker
    expect(r.result).toMatchObject({ spoken: true, as: "inner", how: "said", said: "So, Standup at 10 and a dentist at 3." });
    expect(r.result.hint).toMatch(/inner voice, not the speaker/);
    expect(ctx.box.of("tts").map((t) => [t.text, t.front, t.exact])).toEqual([["Standup at 10 and a dentist at 3.", "inner", undefined]]);
    expect(sayTexts(ctx.dev)[1]).toBe("So, Standup at 10 and a dentist at 3.");
    // word cues follow the front's words
    const head = (b: Uint8Array) => JSON.parse(new TextDecoder().decode(b.indexOf(10) < 0 ? b : b.subarray(0, b.indexOf(10))));
    const cued = ctx.sock.bins.map(head).filter((h) => h.s === ctx.dev.named("say")[1].args.job).flatMap((h) => h.w ?? []);
    expect(cued.map((c) => c[0])).toEqual([0, 1, 2, 3, 4, 5, 6, 7, 8]);
    // hands-free: Molty listens again; nobody speaks -> the conversation ends and the box closes its session
    await until(() => ctx.box.of("front.end").length > 0, 3000);
    expect(ctx.box.of("stt.start").length).toBe(2);
    expect(ctx.box.of("front.end")[0].reason).toBe("idle");
    // the turn's line in the activity log
    const act = ((await ctx.room.ownerActivity(ctx.owner, { action: "get", query: {} })) as any).result.entries;
    const line = act.find((e: any) => e.kind === "trace" && e.ev === "front");
    expect(line.data.line).toMatch(/^front turn: Molty answered [\d.]+ s after the person stopped \(model 0.48 s\), utterance [\d.]+ s, handed to the agent, agent answered [\d.]+ s, spoken [\d.]+ s$/);
    expect(Object.keys(line.data)).toEqual(expect.arrayContaining(["reply", "box", "reply_first_audio", "utterance", "agent_answer", "agent_first_audio"]));
    expect(act.some((e: any) => e.ev === "front.end" && e.data.reason === "idle")).toBe(true);
  });

  it("exact goes through the front too (it keeps the wording); no marks with the front", async () => {
    const ctx = await setup({ box: { reply: () => ({ said: "One sec.", wait: true }) } });
    await ctx.shake();
    await until(async () => (await ctx.inbox()).some((e) => e.type === "utterance"));
    const out = (await ctx.room.command(ctx.token, "say", { text: "Build [happy] 42 passed.", exact: true })) as any;
    expect(out.ok).toBe(true);
    await until(() => ctx.dev.named("say").length >= 2);
    expect(ctx.box.of("tts").at(-1)).toMatchObject({ text: "Build 42 passed.", front: "inner", exact: true });
    expect(ctx.dev.named("say")[1].args).toMatchObject({ text: "Build 42 passed." });
    expect(ctx.dev.named("say")[1].args.marks).toBeUndefined();
    expect(ctx.dev.named("say")[1].args.exact).toBeUndefined(); // relay-only
  });

  it("the inner voice arriving while the front still speaks queues behind it: never cut off, never audio_busy", async () => {
    const ctx = await setup({ box: { reply: () => ({ said: "Let me look that up for you right now.", wait: true }) } });
    await ctx.shake();
    await until(() => ctx.dev.named("say").length > 0);
    const r = (await ctx.room.command(ctx.token, "say", { text: "It is sunny." })) as any;
    expect(r.ok).toBe(true);
    await until(() => ctx.dev.named("say").length >= 2);
    const [first, second] = ctx.dev.named("say");
    expect(first.args.text).toBe("Let me look that up for you right now.");
    expect(second.args.text).toBe("So, It is sunny.");
    expect(second.at).toBeGreaterThan(first.at);
  });

  it("while the person is still talking the agent cannot cut in (the shake's open turn)", async () => {
    const ctx = await setup({ box: { turnEndAfter: 4 } });
    await ctx.shake();
    await until(() => ctx.box.of("stt.start").length > 0);
    await sleep(25);
    // the shake's turn is open (human_speaking): an agent cannot cut in; it can as soon as the turn ended
    const early = (await ctx.room.command(ctx.token, "say", { text: "Hello." })) as any;
    expect(early.ok ? "ok" : early.error).toBe("human_speaking");
  });

  it("the agent never answers: the front says it is still waiting, once, then listens", async () => {
    const ctx = await setup();
    await ctx.shake();
    await until(() => ctx.box.of("tts").some((t) => t.front === "wait"), 3000);
    expect(ctx.box.of("tts").filter((t) => t.front === "wait").length).toBe(1);
    await until(() => sayTexts(ctx.dev).includes("Still checking, one moment."));
    expect(sayTexts(ctx.dev)).toContain("Still checking, one moment.");
    // thinking while it waited
    expect(ctx.dev.named("mood").some((c) => c.args.mood === "thinking")).toBe(true);
    await until(() => ctx.box.of("stt.start").length >= 2, 3000);
    await until(() => ctx.box.of("front.end").length > 0, 3000);
    expect(ctx.box.of("tts").filter((t) => t.front === "wait").length).toBe(1);
  });

  it("small talk: the front answers itself and Molty listens again, hands-free, for several turns; the agent may stay quiet", async () => {
    const ctx = await setup({ box: { heard: ["hi molty", "how are you", "bye"], reply: (h) => ({ said: h === "bye" ? "Bye!" : "Hi there!", wait: false }) } });
    await ctx.shake();
    await until(() => ctx.box.of("front.end").length > 0, 4000);
    expect(ctx.box.of("stt.start").length).toBe(3);
    expect(sayTexts(ctx.dev)).toEqual(["Hi there!", "Hi there!", "Bye!"]);
    // a stop phrase ends it after the goodbye
    expect(ctx.box.of("front.end")[0].reason).toBe("stop_phrase");
    const us = (await ctx.inbox()).filter((e) => e.type === "utterance");
    expect(us.map((e) => [e.data.text, e.data.trigger])).toEqual([["hi molty", "shake"], ["how are you", "front"], ["bye", "front"]]);
  });

  it("a held listen (the button): the box is told, the release ends the turn and the front answers it then", async () => {
    // live 2026-11-27: held, spoke, let go; the box was never told it was held (its model could cut a pause)
    const ctx = await setup({ box: { heard: ["what's up, is this fast?"], reply: () => ({ said: "Pretty fast!", wait: false }) } });
    await ctx.room.socketMessage(ctx.sock, JSON.stringify({ t: "evt", name: "shake", data: { peak_g: 0, reversals: 0, ms: 0, listen: "shake-1", input: "button_accept", hold: true } }));
    await until(() => sayTexts(ctx.dev).length > 0);
    expect(ctx.box.of("stt.start")[0]).toMatchObject({ front: true, hold: true });
    expect(ctx.dev.named("listen_stop")).toEqual([]); // the device's own end (the release), not the model's
    expect(sayTexts(ctx.dev)[0]).toBe("Pretty fast!");
    await until(async () => (await ctx.inbox()).some((e) => e.type === "utterance"));
    const u = (await ctx.inbox()).find((e) => e.type === "utterance")!;
    expect(u.data).toMatchObject({ text: "what's up, is this fast?", front: { said: "Pretty fast!", waiting: false } });
    // a hands-free listen after it is not held
    await until(() => ctx.box.of("stt.start").length >= 2, 3000);
    expect(ctx.box.of("stt.start")[1].hold).toBeUndefined();
  });

  it("an agent answer during a hands-free listen nobody spoke into yet: the listen yields to it, the conversation goes on", async () => {
    const ctx = await setup({ chunks: 40, box: { heard: ["hi molty", "", ""], reply: () => ({ said: "Hey!", wait: false }), turnEndAfter: 3 } });
    // the follow-up listen's person never speaks: the box would call no_speech at chunk 3; answer before that
    ctx.box.listens; // (typed access)
    await ctx.shake();
    await until(() => ctx.box.of("stt.start").length >= 2);
    const r = (await ctx.room.command(ctx.token, "say", { text: "By the way, your build passed." })) as any;
    expect(r.ok).toBe(true);
    await until(() => sayTexts(ctx.dev).includes("So, By the way, your build passed."));
    const stops = ctx.dev.named("listen_stop").map((c) => c.args.reason);
    expect(stops).toContain("no_speech");
    // not "idle": Molty listens once more after the answer, then ends when nobody speaks
    await until(() => ctx.box.of("front.end").length > 0, 3000);
    expect(ctx.box.of("stt.start").length).toBe(3);
  });
});

describe("voice front: talk and modes", () => {
  it("an agent's talk: it speaks through the front and returns once said; the front does the listen, the person's words come as an utterance", async () => {
    // live 2026-11-27: a talk held its webhook agent through the front's speech and the listen after it (17 s)
    const ctx = await setup({ box: { heard: ["is it raining?"] } });
    const r = (await ctx.room.talk(ctx.token, { say: "Hi, what can I do?", exact: true })) as any;
    expect(r.ok).toBe(true);
    expect(r.result).toMatchObject({ status: "done", said: "Hi, what can I do?", reason: "spoken", spoken: true, how: "said" });
    expect(r.result.hint).toMatch(/listens next/);
    expect(r.result.heard).toBeUndefined();
    expect(ctx.box.of("tts")[0]).toMatchObject({ front: "inner", exact: true });
    await until(async () => (await ctx.inbox()).some((e) => e.type === "utterance"));
    expect(ctx.box.of("stt.start")[0].front).toBe(true);
    const u = (await ctx.inbox()).find((e) => e.type === "utterance")!;
    expect(u.data).toMatchObject({ text: "is it raining?", trigger: "front", front: { said: "Let me check on that.", waiting: true } });
  });

  it("voice mode cloud: no front at all, whatever the switch says", async () => {
    const ctx = await setup();
    await ctx.room.voiceSettings(ctx.owner, { action: "set", mode: "cloud" });
    const v = ((await ctx.room.voiceSettings(ctx.owner, { action: "get" })) as any).result;
    expect(v.front).toMatchObject({ mode: "on", serving: false });
    expect(v.front.reason).toMatch(/voice mode is cloud/);
    const r = (await ctx.room.talk(ctx.token, { say: "Hello." })) as any;
    expect(r.ok).toBe(true);
    expect(ctx.box.of("tts").length + ctx.box.of("stt.start").length).toBe(0);
    expect(ctx.voice.ttsCalls.map((c) => c.text)).toContain("Hello.");
  });
});

describe("voice front: every way out", () => {
  it("the agent ends it; context is passed silently", async () => {
    const ctx = await setup();
    await ctx.shake();
    await until(() => ctx.dev.named("say").length > 0);
    const c = (await ctx.room.command(ctx.token, "front", { context: "The deploy finished at 14:02." })) as any;
    expect(c.result).toMatchObject({ conversation: true, context: true });
    expect(ctx.box.of("front.context")[0].text).toBe("The deploy finished at 14:02.");
    const e = (await ctx.room.command(ctx.token, "front", { end: true })) as any;
    expect(e.result).toMatchObject({ ended: true });
    expect(ctx.box.of("front.end")[0].reason).toBe("agent");
    expect(((await ctx.room.command(ctx.token, "front", {})) as any).error).toBe("invalid_args");
  });

  it("a persona line set through front {context, persona} is kept per device and given to every box that connects", async () => {
    const ctx = await setup();
    const c = (await ctx.room.command(ctx.token, "front", { context: "You are Dude, a cat lounging on Juan's desk; dry, warm humour.", persona: true })) as any;
    expect(c.result).toMatchObject({ context: true });
    expect(ctx.box.of("front.context").at(-1)).toMatchObject({ text: "You are Dude, a cat lounging on Juan's desk; dry, warm humour.", persona: true });
    expect(await ctx.kv.get("front:persona")).toBe("You are Dude, a cat lounging on Juan's desk; dry, warm humour.");
    await ctx.box.hello(); // the box restarts (or reconnects): it is told again
    await until(() => ctx.box.of("front.context").filter((m) => m.persona).length === 2);
    expect(ctx.box.of("front.context").filter((m) => m.persona).length).toBe(2);
    expect(((await ctx.room.command(ctx.token, "front", { context: "x".repeat(501), persona: true })) as any).error).toBe("invalid_args");
  });

  it("after a front reply the device gets the say's end and the hands-free listen at once, with or without an agent answer in flight", async () => {
    // live 2026-11-27 (turn 4): the reply's say stayed open 12.8 s for 4.5 s of audio; the next listen came 8 s late
    for (const inFlight of [false, true]) {
      const ctx = await setup({ chunks: 30, box: { heard: ["hi molty", ""], reply: () => ({ said: "Hey there, all good here!", wait: false }), absorb: (t) => t.startsWith("Hey") } });
      await ctx.shake();
      await until(() => ctx.dev.named("say").length > 0);
      const reply = ctx.dev.named("say")[0].args.job as string;
      const talk = inFlight ? (ctx.room.talk(ctx.token, { say: "Hey Juan! Just lounging on your desk." }) as Promise<any>) : undefined;
      await until(() => ctx.dev.named("listen").length >= 2, 3000);
      const sent = ctx.sock.bins.filter((b) => JSON.parse(new TextDecoder().decode(b.subarray(0, Math.max(0, b.indexOf(10)) || b.length))).s === reply);
      expect(ctx.dev.ends[reply]).toBeDefined();
      expect(sent.length).toBeGreaterThan(0);
      // the end frame within the audio's own length of the say; the listen right after the say.done
      expect(ctx.dev.ends[reply] - ctx.dev.named("say")[0].at).toBeLessThan(400);
      expect(ctx.dev.named("listen")[1].at - ctx.dev.ends[reply]).toBeLessThan(300);
      if (talk) expect((await talk).result).toMatchObject({ how: "absorbed" });
      expect(sayTexts(ctx.dev)).toEqual(["Hey there, all good here!"]);
    }
  });

  it("do not disturb, a tap, the box going away and the device going away all end it", async () => {
    for (const how of ["dnd", "tap", "box", "device"] as const) {
      const ctx = await setup({ box: { reply: () => ({ said: "One moment.", wait: true }) } });
      await ctx.shake();
      await until(() => ctx.dev.named("mood").some((m) => m.args.mood === "thinking"));
      if (how === "dnd") await ctx.room.socketMessage(ctx.sock, JSON.stringify({ t: "evt", name: "dnd", data: { on: true } }));
      if (how === "tap") await ctx.room.socketMessage(ctx.sock, JSON.stringify({ t: "evt", name: "tap", data: {} }));
      if (how === "box") ctx.box.sock.close(1000, "bye");
      if (how === "device") ctx.sock.close(1000, "bye");
      await until(async () => !((await ctx.room.voiceSettings(ctx.owner, { action: "get" })) as any).result.front.conversation);
      expect(((await ctx.room.voiceSettings(ctx.owner, { action: "get" })) as any).result.front.conversation).toBe(false);
      const act = ((await ctx.room.ownerActivity(ctx.owner, { action: "get", query: {} })) as any).result.entries;
      expect(act.find((e: any) => e.ev === "front.end").data.reason).toBe({ dnd: "do_not_disturb", tap: "tap", box: "box_gone", device: "device_offline" }[how]);
      // nothing is spoken for the waiting turn afterwards
      await sleep(250);
      expect(ctx.box.of("tts").filter((t) => t.front === "wait").length).toBe(0);
    }
  });

  it("the box at its daily limit stops offering the front: today's path serves the next turn", async () => {
    const ctx = await setup({ box: { reply: () => ({ said: "Hi!", wait: false }) } });
    await ctx.shake();
    await until(() => ctx.dev.named("say").length > 0);
    await ctx.room.socketMessage(ctx.box.sock, JSON.stringify({ t: "front.session", state: "closed", reason: "daily_cap", s: 60, usage: { audio_in: 600, audio_out: 1200 } }));
    await ctx.room.socketMessage(ctx.box.sock, JSON.stringify({ t: "voice.hello", name: "laptop", version: "0.2.0", voices: [], stt: false, tts: false, engine: "realtime" }));
    const v = ((await ctx.room.voiceSettings(ctx.owner, { action: "get" })) as any).result;
    expect(v.front).toMatchObject({ mode: "on", available: false, serving: false, conversation: false });
    expect(v.serving).toEqual({ tts: "fake", stt: "fake" }); // auto: the cloud
    const act = ((await ctx.room.ownerActivity(ctx.owner, { action: "get", query: {} })) as any).result.entries;
    expect(act.find((e: any) => e.ev === "front.session").data).toMatchObject({ state: "closed", reason: "daily_cap", usage: { audio_in: 600, audio_out: 1200 } });
  });

  it("a front.reply for a listen the relay never ran, or with the front off, is cancelled: nothing plays", async () => {
    const ctx = await setup({ front: "off" });
    await ctx.room.socketMessage(ctx.box.sock, JSON.stringify({ t: "front.reply", id: "rogue", stt: "s-unknown" }));
    expect(ctx.box.of("tts.cancel").map((m) => m.id)).toEqual(["rogue"]);
    expect(ctx.dev.named("say").length).toBe(0);
  });
});

describe("voice front: an agent's answer belongs to the moment (live 2026-11-27)", () => {
  it("one voice: the agent's greeting after the front's own greeting is absorbed, a new fact is said once in the front's turn", async () => {
    // live 2026-11-27: the front greeted; 11 s later the agent's own greeting for that turn was said as a second answer
    const ctx = await setup({ box: { heard: ["hi molty", ""], reply: () => ({ said: "Hey there!", wait: false }), absorb: (t) => t.startsWith("Hey") } });
    await ctx.shake();
    await until(async () => (await ctx.inbox()).some((e) => e.type === "utterance"));
    const t1 = (await ctx.inbox()).find((e) => e.type === "utterance")!.data.turn_id as string;
    const g = (await ctx.room.talk(ctx.token, { say: "Hey Juan! Just lounging on your desk like a proper cat.", in_reply_to: t1 })) as any;
    expect(g).toMatchObject({ ok: true, result: { status: "done", spoken: false, as: "inner", how: "absorbed" } });
    expect(g.result.hint).toMatch(/said nothing/);
    const f = (await ctx.room.talk(ctx.token, { say: "By the way, your build passed." })) as any;
    expect(f.result).toMatchObject({ status: "done", spoken: true, as: "inner", how: "said", said: "So, By the way, your build passed." });
    expect(sayTexts(ctx.dev)).toEqual(["Hey there!", "So, By the way, your build passed."]);
    expect(ctx.box.of("tts").map((t) => t.front)).toEqual(["inner", "inner"]); // never text to speech
    const act = ((await ctx.room.ownerActivity(ctx.owner, { action: "get", query: {} })) as any).result.entries;
    expect(act.find((e: any) => e.kind === "job" && e.job === g.result.job).data).toMatchObject({ status: "done", as: "inner" });
  });

  it("nothing is said while the person speaks: an answer to the turn before waits for their turn, then the front decides", async () => {
    // live: Dude's greeting for turn 1 came while the person spoke turn 2 and played after the front's reply to it
    const ctx = await setup({ chunks: 10, box: { heard: ["hi molty", "tell me something about my screen", ""], reply: () => ({ said: "Hi there!", wait: false }), turnEndAfter: 6, absorb: (t) => t.startsWith("Hey") } });
    await ctx.shake();
    await until(async () => (await ctx.inbox()).some((e) => e.type === "utterance"));
    const t1 = (await ctx.inbox()).find((e) => e.type === "utterance")!.data.turn_id as string;
    await until(() => Object.values(ctx.box.listens).filter((l) => l.heard).length >= 2 && Object.values(ctx.box.listens)[1].n >= 2);
    const r = (await ctx.room.talk(ctx.token, { say: "Hey Juan! Purring along nicely.", in_reply_to: t1 })) as any;
    expect(r.result).toMatchObject({ spoken: false, how: "absorbed" });
    const sayAt = ctx.dev.named("say").map((c) => c.at);
    expect(ctx.dev.named("listen_stop").length).toBeGreaterThan(0);
    expect(sayAt.every((at) => at < ctx.dev.named("listen")[1].at || at > ctx.dev.named("listen_stop")[0].at)).toBe(true);
    expect(sayTexts(ctx.dev).some((t) => /Purring/.test(t))).toBe(false);
    const again = (await ctx.room.talk(ctx.token, { say: "Hey Juan! Purring along nicely on your desk.", in_reply_to: t1 })) as any;
    expect(again.result).toMatchObject({ job: r.result.job, how: "absorbed", repeated: true });
  });

  it("the same agent answering the same turn again while the first is queued, playing or done: never said twice, the first one's job", async () => {
    // live: Dude retried its talk for one in_reply_to two or three times; the second got audio_busy, a third was said again
    const ctx = await setup({ box: { reply: () => ({ said: "Let me look that up for you right now.", wait: true }) } });
    await ctx.shake();
    await until(async () => (await ctx.inbox()).some((e) => e.type === "utterance"));
    const t1 = (await ctx.inbox()).find((e) => e.type === "utterance")!.data.turn_id as string;
    const first = ctx.room.talk(ctx.token, { say: "Standup at 10.", in_reply_to: t1 }) as Promise<any>;
    await sleep(5);
    const second = (await ctx.room.talk(ctx.token, { say: "Your standup is at 10.", in_reply_to: t1 })) as any;
    const one = await first;
    expect(one.result).toMatchObject({ status: "done", said: "So, Standup at 10." });
    expect(second).toMatchObject({ ok: true, result: { job: one.result.job, status: "done", repeated: true, said: "So, Standup at 10." } });
    const third = (await ctx.room.command(ctx.token, "say", { text: "Standup is at 10 am.", in_reply_to: t1 })) as any;
    expect(third.result).toMatchObject({ job: one.result.job, repeated: true });
    expect(ctx.box.of("tts").filter((t) => t.front === "inner").length).toBe(1);
    expect(sayTexts(ctx.dev).filter((t) => /tandup/.test(t)).length).toBe(1);
  });

  it("after the conversation ended, an answer to a turn the front already answered is dropped: no session, nothing said", async () => {
    // live: a retry after the conversation ended idle opened a new provider session to say a stale greeting
    const ctx = await setup({ box: { heard: ["hi molty"], reply: () => ({ said: "Hi there!", wait: false }) } });
    await ctx.shake();
    await until(() => ctx.box.of("front.end").length > 0, 3000);
    const t1 = (await ctx.inbox()).find((e) => e.type === "utterance")!.data.turn_id as string;
    const tts = ctx.box.of("tts").length;
    const r = (await ctx.room.command(ctx.token, "say", { text: "Hey! Going pretty smooth.", in_reply_to: t1 })) as any;
    expect(r.ok).toBe(true);
    const job = (await ctx.room.job(ctx.token, r.result.job, 0)) as any;
    expect(job.result).toMatchObject({ status: "done", result: { spoken: false, as: "inner", how: "absorbed", reason: "conversation_over" } });
    await sleep(50);
    expect(ctx.box.of("tts").length).toBe(tts);
    expect(ctx.box.of("front.context").at(-1).text).toMatch(/Hey! Going pretty smooth\.$/); // kept, never voiced (no session opened for it)
    expect(sayTexts(ctx.dev)).toEqual(["Hi there!"]);
  });

  it("which answers are said: the one the front waits for, but none to a turn over a minute old", () => {
    let now = 0;
    const clock = { now: () => now, setTimeout: () => 0, clearTimeout: () => {} } as any;
    const ft = new FrontTalk({ clock, openTurn: () => undefined } as any);
    ft.spoken("t1");
    ft.turnEnded("t1", 0);
    const latest = { id: "t1", at: 500 };
    expect(ft.fate("t1", latest)).toEqual({ speak: true });
    ft.spoken("t2"); // the person speaks again: t1 is not the moment any more
    expect(ft.fate("t1", latest)).toEqual({ speak: false, reason: "conversation_over" }); // (no conversation live here)
    ft.listened({ ok: true, text: "" }, false, "t2"); // it was nothing: t1 is the moment again
    expect(ft.fate("t1", latest)).toEqual({ speak: true });
    ft.spoke("t1", { wait: true } as any); // the front handed t1 to the agent: its answer is said even after the person spoke again
    ft.spoken("t3");
    expect(ft.fate("t1", latest)).toEqual({ speak: true });
    now = 60001;
    expect(ft.fate("t1", latest)).toEqual({ speak: false, reason: "old_turn" });
  });
});

describe("voice front: helpers", () => {
  it("stop phrases are only the whole utterance", () => {
    for (const t of ["bye", "Bye bye, Molty!", "that's all", "Okay, stop.", "thanks, bye"]) expect(stopPhrase(t)).toBe(true);
    for (const t of ["stop the build", "say bye to Ana", "what's all this"]) expect(stopPhrase(t)).toBe(false);
  });

  it("the turn's line", () => {
    const s = frontTurnSummary({ endAt: 1000, replyAt: 1250, boxMs: 210, replyOut: 1480, utteranceAt: 1600, waiting: true, agentAt: 5000, agentOut: 5400 });
    expect(s.ms).toEqual({ reply: 250, box: 210, reply_first_audio: 480, utterance: 600, agent_answer: 4000, agent_first_audio: 4400 });
    expect(s.line).toBe("front turn: Molty answered 0.48 s after the person stopped (model 0.21 s), utterance 0.60 s, handed to the agent, agent answered 4.00 s, spoken 4.40 s");
    // live: a turn with no utterance (a talk's listen) printed "utterance -"
    expect(frontTurnSummary({ endAt: 1000, replyAt: 1001, replyOut: 2059 }).line).toBe("front turn: Molty answered 1.06 s after the person stopped");
  });
});
