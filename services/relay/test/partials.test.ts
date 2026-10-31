// While the human speaks (core/partials.ts, core/calibration.ts, core/notes.ts; docs/AGENT-LINK.md
// "While the human speaks"): partial transcripts chunked by meaning, human_speaking for every agent
// say / talk / ask / mood / show during an open turn, drafts held + pre-synthesised then played at
// once or dropped, reacts (screen only, rate-limited), the speech stats + calibration, per-agent notes,
// the activity entries and the webhook opt-in.
import { describe, expect, it } from "vitest";
import type { RelayBackend } from "../src/core/api.js";
import { CALIBRATION, addOutcome, addTurn, emptyStats, markMisfire, policyFor, speechSummary, tuneThreshold } from "../src/core/calibration.js";
import type { AgentEvent } from "../src/core/events.js";
import { NOTES_LIMITS } from "../src/core/notes.js";
import { DEFAULT_PARTIAL_POLICY, PartialChunker, PauseTracker, judgeDraft, sentenceEnd } from "../src/core/partials.js";
import { decodeAudioFrame, encodeAudioFrame } from "../src/core/protocol.js";
import type { DeviceRoom } from "../src/core/room.js";
import { handleMcp } from "../src/surfaces/mcp.js";
import { handleRest } from "../src/surfaces/rest.js";
import { DEVICE, type FakeSocket, LINK, connect, makeRoom } from "./helpers.js";

// ---------------------------------------------------------------- pure: chunking, judging, calibration

describe("partial chunking (by meaning, never token by token)", () => {
  const P = { ...DEFAULT_PARTIAL_POLICY, lead_ms: 0, every_words: 5, every_ms: 4000 };

  it("a sentence end, N new words or T seconds — whichever first; nothing per word", () => {
    const c = new PartialChunker(P, 1000);
    expect(c.update("so", 1100)).toBeUndefined();
    expect(c.update("so I was", 1300)).toBeUndefined();
    // a sentence end (Kyutai punctuation) sends at once, even with < N words
    expect(c.update("so I was late.", 1500)).toEqual({ seq: 1, text_so_far: "so I was late.", new_text: "so I was late.", elapsed_ms: 500 });
    expect(c.update("so I was late. the", 1600)).toBeUndefined();
    // five new words
    expect(c.update("so I was late. the train was stuck again", 2000)).toEqual({ seq: 2, text_so_far: "so I was late. the train was stuck again", new_text: "the train was stuck again", elapsed_ms: 1000 });
    // T seconds after the last partial with a new word waiting: the timer's check sends it
    expect(c.update("so I was late. the train was stuck again and", 2100)).toBeUndefined();
    expect(c.dueAt()).toBe(6000);
    expect(c.check(5999)).toBeUndefined();
    expect(c.check(6000)).toMatchObject({ seq: 3, new_text: "and" });
    // no new words: nothing, however long
    expect(c.check(60000)).toBeUndefined();
    expect(c.dueAt()).toBeUndefined();
    expect(c.textOf(2)).toBe("so I was late. the train was stuck again");
  });

  it("no partial before lead_ms (a short turn gets none), an ellipsis is no sentence end, at most max_per_turn", () => {
    const c = new PartialChunker({ ...P, lead_ms: 3000, max_per_turn: 2 }, 0);
    expect(c.update("hi there.", 500)).toBeUndefined();
    expect(c.update("hi there. what's on my calendar today?", 2900)).toBeUndefined();
    expect(c.dueAt()).toBe(4000);
    expect(c.check(3000)?.seq).toBe(1); // at the lead: the waiting sentence goes
    expect(sentenceEnd("well...")).toBe(false);
    expect(sentenceEnd("well…")).toBe(false);
    expect(sentenceEnd('he said "yes."')).toBe(true);
    expect(c.update("hi there. what's on my calendar today? well...", 3100)).toBeUndefined();
    expect(c.update("hi there. what's on my calendar today? well... one two three four five", 3200)?.seq).toBe(2);
    expect(c.update("hi there. what's on my calendar today? well... one two three four five six seven eight nine ten.", 9000)).toBeUndefined();
    expect(c.dueAt()).toBeUndefined();
  });

  it("an STT revision counts new words from where the texts differ", () => {
    const c = new PartialChunker(P, 0);
    c.update("set an alarm for seven.", 10);
    const p = c.update("set an alarm for eleven tomorrow morning at the latest", 20);
    expect(p?.new_text).toBe("eleven tomorrow morning at the latest");
  });

  it("pauses: gaps of at least 600 ms between word arrivals", () => {
    const t = new PauseTracker();
    t.update("so", 0);
    t.update("so I", 200);
    t.update("so I was", 1100);
    t.update("so I was", 3000); // no new word: no pause
    t.update("so I was thinking", 3100);
    expect([t.pauses, t.max_pause_ms]).toEqual([2, 2000]);
  });
});

describe("draft judging", () => {
  it("holds when the final adds few words that keep the meaning", () => {
    expect(judgeDraft("what's on my calendar", "What's on my calendar today?", 4)).toEqual({ ok: true, new_words: 1 });
    expect(judgeDraft("what's on my calendar", "what's on my calendar", 0)).toEqual({ ok: true, new_words: 0 });
  });
  it("drops on too many new words, on a flip word, on a revised transcript", () => {
    expect(judgeDraft("what's on my calendar", "what's on my calendar today and tomorrow and also next week", 4)).toMatchObject({ ok: false, reason: "new_words", new_words: 7 });
    expect(judgeDraft("can you set an alarm for seven", "can you set an alarm for seven, but not today", 6)).toMatchObject({ ok: false, reason: "meaning", new_words: 3 });
    expect(judgeDraft("remind me to call mom", "remind me to call mom. Actually, never mind.", 6)).toMatchObject({ ok: false, reason: "meaning" });
    expect(judgeDraft("remind me to call mom", "remind me to text dad later", 6)).toMatchObject({ ok: false, reason: "revised" });
    // one revised last word is tolerated (the STT's last word often firms up)
    expect(judgeDraft("remind me to call mo", "remind me to call mom tonight", 4)).toEqual({ ok: true, new_words: 2 });
  });
});

describe("calibration", () => {
  const turn = (words: number, ms: number, at = 1) => ({ at, words, ms, pauses: 1, max_pause_ms: 800 });

  it("defaults until enough turns, then partial points at ~1/3 of the median turn, within bounds", () => {
    let s = emptyStats();
    for (let i = 0; i < CALIBRATION.minTurns - 1; i++) s = addTurn(s, turn(24, 9000));
    expect(policyFor(s)).toMatchObject({ source: "defaults", every_words: DEFAULT_PARTIAL_POLICY.every_words, every_ms: DEFAULT_PARTIAL_POLICY.every_ms });
    s = addTurn(s, turn(24, 9000));
    expect(policyFor(s)).toMatchObject({ source: "calibrated", every_words: 8, every_ms: 3000, lead_ms: 3000 });
    // a terse human: clamped to the bounds; a rambler likewise
    let terse = emptyStats();
    for (let i = 0; i < 10; i++) terse = addTurn(terse, turn(3, 1200));
    expect(policyFor(terse)).toMatchObject({ every_words: CALIBRATION.everyWords[0], every_ms: CALIBRATION.everyMs[0] });
    let ramble = emptyStats();
    for (let i = 0; i < 10; i++) ramble = addTurn(ramble, turn(200, 90000));
    expect(policyFor(ramble)).toMatchObject({ every_words: CALIBRATION.everyWords[1], every_ms: CALIBRATION.everyMs[1] });
    // empty turns are no samples; only the last keepTurns are kept
    expect(addTurn(s, turn(0, 900))).toBe(s);
    for (let i = 0; i < 80; i++) s = addTurn(s, turn(10, 3000));
    expect(s.turns).toHaveLength(CALIBRATION.keepTurns);
  });

  it("the draft threshold tunes itself one step at a time, within its bounds", () => {
    const { min, max, initial } = CALIBRATION.threshold;
    // drafts keep dying of late words while openers land: looser
    let s = emptyStats();
    for (let i = 0; i < CALIBRATION.tuneEvery; i++) s = addOutcome(s, i % 3 === 0 ? "used" : "new_words", i);
    expect(s.threshold).toBe(initial + 1);
    for (let k = 0; k < 20; k++) for (let i = 0; i < CALIBRATION.tuneEvery; i++) s = addOutcome(s, "new_words", i);
    expect(s.threshold).toBe(max);
    // openers that misfire: stricter, down to the floor
    let m = emptyStats();
    for (let k = 0; k < 20; k++) for (let i = 0; i < CALIBRATION.tuneEvery; i++) m = addOutcome(m, i % 2 ? "misfire" : "used", i);
    expect(m.threshold).toBe(min);
    // outcomes that judge nothing about the threshold never tune it
    let q = emptyStats();
    for (let i = 0; i < 30; i++) q = addOutcome(q, "turn_claimed", i);
    expect([q.threshold, q.since_tune, q.totals.dropped.turn_claimed]).toEqual([initial, 0, 30]);
    // balanced outcomes: unchanged
    let b = emptyStats();
    for (let i = 0; i < 12; i++) b = addOutcome(b, "used", i);
    expect(b.threshold).toBe(initial);
    expect(tuneThreshold({ ...b, threshold: 99 })).toBe(max);
  });

  it("a misfire turns that opener's used around; the summary shows the record", () => {
    let s = addOutcome(emptyStats(), "used", 100);
    s = addOutcome(s, "new_words", 200);
    s = markMisfire(s, 100, 300);
    expect(s.totals).toMatchObject({ used: 0, misfires: 1, dropped: { new_words: 1 } });
    expect(markMisfire(s, 100, 400)).toBe(s); // once
    const sum = speechSummary(addTurn(s, turn(12, 4000)));
    expect(sum).toMatchObject({ turns: { sampled: 1, needed: CALIBRATION.minTurns }, words: { median: 12 }, seconds: { median: 4 }, wpm: 180, drafts: { misfires: 1, use_rate: 0.5, misfire_rate: 1, threshold: CALIBRATION.threshold.initial } });
    expect(speechSummary(undefined)).toMatchObject({ turns: { sampled: 0 }, policy: { source: "defaults" } });
  });
});

// ---------------------------------------------------------------- the room: a shake turn with the voice box

const FAST = { cmdTimeoutMs: 400, pairWindowMs: 400, sayChunkSamples: 1600, sayLeadFrames: 2, listenGraceMs: 300, sayDoneGraceMs: 400, boxSttFinalMs: 400, reactShowMs: 60, partialLeadMs: 0 };
const CAPS = ["say", "say_cues", "listen", "listen_stop", "listen_window", "ack", "shake_talk", "mood", "act", "react"];
const SHAKE = { peak_g: 2.4, reversals: 5, ms: 900 };
const NEXT = ["NEWC2345", "NEWD2345", "NEWE2345", "NEWF2345", "NEWG2345"];

/** What the simulated human says in a shake turn, as the box hears it. */
interface Speech {
  words: string[];
  /** The box's final text (default: the words). */
  final?: string;
  /** ms between mic chunks; one chunk = one more word heard. */
  chunkMs?: number;
}

type Log = Array<{ at: number; kind: string; name?: string; args?: any; job?: string }>;

/** A device: answers cmds, adopts a shake's listen streaming one chunk per word, plays say streams (say.done 5 ms after the end). */
function device(room: DeviceRoom, sock: FakeSocket, speech: () => Speech) {
  const log: Log = [];
  let timers: ReturnType<typeof setTimeout>[] = [];
  let codes = 0;
  const text = sock.sendText.bind(sock);
  const bin = sock.sendBinary.bind(sock);
  sock.sendText = (t: string) => {
    text(t);
    const f = JSON.parse(t);
    if (f.t === "pair.request") setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "pair.result", req: f.req, ok: true, link_code: NEXT[codes++] })), 2);
    if (f.t !== "cmd") return;
    log.push({ at: Date.now(), kind: "cmd", name: f.name, args: f.args });
    const result = f.name === "listen" ? { job: f.args.job, ms: 30000, ack: "chirp" } : {};
    setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "res", id: f.id, ok: true, result })), 1);
    if (f.name === "listen") {
      const job = f.args.job;
      const sp = speech();
      let seq = 0;
      timers = [];
      // the human talks one word per chunk, then goes quiet (the box calls the end of turn)
      for (let i = 0; i < sp.words.length + 3; i++) {
        timers.push(
          setTimeout(() => void room.socketMessage(sock, encodeAudioFrame({ s: job, seq: seq++, kind: "pcm16", rate: 16000, end: false }, new Uint8Array(new Int16Array(800).fill(3000).buffer))), 5 + (sp.chunkMs ?? 20) * i),
        );
      }
    }
    if (f.name === "listen_stop") {
      for (const x of timers) clearTimeout(x);
      timers = [];
      log.push({ at: Date.now(), kind: "listen-end", job: f.args.job });
      setTimeout(() => void room.socketMessage(sock, new TextEncoder().encode(JSON.stringify({ s: f.args.job, seq: 999, kind: "pcm16", rate: 16000, end: true, reason: f.args.reason ?? "turn_end" }))), 2);
    }
  };
  const firstFrame = new Map<string, number>();
  sock.sendBinary = (b: Uint8Array) => {
    bin(b);
    const fr = decodeAudioFrame(b)!;
    if (!firstFrame.has(fr.header.s)) firstFrame.set(fr.header.s, Date.now());
    log.push({ at: Date.now(), kind: "frame", job: fr.header.s });
    if (fr.header.end) setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "evt", name: "say.done", data: { job: fr.header.s } })), 5);
  };
  return { log, firstFrame };
}

/** The voice box: one more word per audio chunk as stt.partial, stt.turn_end after the last, the final on stt.end; TTS after `ttsMs`. */
function box(room: DeviceRoom, sock: FakeSocket, speech: () => Speech, o: { ttsMs?: number } = {}) {
  const tts: Array<{ at: number; text: string }> = [];
  const got: Record<string, number> = {};
  const turned = new Set<string>();
  const text = sock.sendText.bind(sock);
  const say = (m: unknown) => void room.socketMessage(sock, JSON.stringify(m));
  sock.sendText = (t: string) => {
    text(t);
    const f = JSON.parse(t);
    if (f.t === "tts") {
      tts.push({ at: Date.now(), text: f.text });
      setTimeout(() => {
        if (sock.closed) return;
        void room.socketMessage(sock, encodeAudioFrame({ s: f.id, seq: 0, kind: "pcm16", rate: 16000, end: false }, new Uint8Array(new Int16Array(3200).fill(500).buffer)));
        say({ t: "tts.end", id: f.id });
      }, o.ttsMs ?? 5);
    }
    if (f.t === "stt.start") got[f.id] = 0;
    if (f.t === "stt.end") setTimeout(() => !sock.closed && say({ t: "stt.final", id: f.id, text: speech().final ?? speech().words.join(" "), ms: { final: 3 } }), 3);
  };
  sock.sendBinary = (b: Uint8Array) => {
    const fr = decodeAudioFrame(b)!;
    const id = fr.header.s;
    if (got[id] === undefined) return;
    const w = speech().words;
    const n = Math.min(w.length, ++got[id]);
    if (n <= w.length && got[id] <= w.length) say({ t: "stt.partial", id, text: w.slice(0, n).join(" ") });
    if (got[id] === w.length + 2 && !turned.has(id)) {
      turned.add(id);
      say({ t: "stt.turn_end", id });
    }
  };
  return { tts, hello: () => room.socketMessage(sock, JSON.stringify({ t: "voice.hello", name: "gpu box", version: "0.1.0", voices: ["a.wav"], stt: true, tts: true })) };
}

async function setup(o: { caps?: string[]; ttsMs?: number; timings?: Record<string, number>; agents?: number } = {}) {
  const sent: Array<{ url: string; body: string }> = [];
  const ctx = makeRoom({
    timings: { ...FAST, ...o.timings },
    scheduler: { setAlarm: async () => {} },
    fetch: async (r) => {
      sent.push({ url: r.url, body: await r.text() });
      return new Response("ok");
    },
    resolveHost: async () => ["93.184.216.34"],
  });
  let speech: Speech = { words: "what's on my calendar today?".split(" ") };
  const sock = await connect(ctx.room, ctx.host, undefined, LINK, { caps: o.caps ?? CAPS });
  const dev = device(ctx.room, sock, () => speech);
  const pair = async (name: string, link: string, role?: "voice") => {
    const out = await ctx.room.pair({ device_id: DEVICE, link_code: link, agent_name: name, ...(role ? { role } : {}) });
    if (!out.ok || out.result.status !== "approved") throw new Error(JSON.stringify(out));
    return out.result.agent_token;
  };
  const a = await pair("drafter", LINK);
  const b = await pair("other", NEXT[0]);
  const v = await pair("gpu box", NEXT[1], "voice");
  const auth = await ctx.room.voiceAuth(v);
  if (!auth.ok) throw new Error("voice auth");
  const bsock = ctx.host.open();
  ctx.room.voiceOpen(bsock, auth.result.hash);
  const bx = box(ctx.room, bsock, () => speech, { ttsMs: o.ttsMs });
  await bx.hello();
  const backend: RelayBackend = { device: () => ctx.room };
  const shake = async (sp: Speech, local = "shake-1") => {
    speech = sp;
    await ctx.room.socketMessage(sock, JSON.stringify({ t: "evt", name: "shake", data: { ...SHAKE, listen: local } }));
    const listen = dev.log.filter((l) => l.name === "listen").at(-1)!;
    return listen.args.job as string;
  };
  return { ...ctx, sock, dev, box: bx, a, b, backend, shake, sent, link: NEXT[2] };
}

const until = async (cond: () => boolean | Promise<boolean>, ms = 2000) => {
  const end = Date.now() + ms;
  while (!(await cond()) && Date.now() < end) await new Promise((r) => setTimeout(r, 4));
};
const evs = async (room: DeviceRoom, token: string) => ((await room.events(token, { wait_ms: 0 })) as any).result.events as AgentEvent[];
/** Reads a token's inbox until `type` (or ms), keeping everything read. */
async function readUntil(room: DeviceRoom, token: string, pred: (e: AgentEvent) => boolean, seen: AgentEvent[] = [], ms = 2500) {
  await until(async () => {
    seen.push(...(await evs(room, token)));
    return seen.some(pred);
  }, ms);
  return seen;
}
const jobRec = (kv: { get<T>(k: string): Promise<T | undefined> }, id: string) => kv.get<any>(`job:${id}`);
const rest = async (b: RelayBackend, token: string, method: string, path: string, body?: unknown, headers: Record<string, string> = {}) => {
  const r = await handleRest(new Request(`https://relay.test${path}`, { method, headers: { ...(token ? { authorization: `Bearer ${token}` } : {}), "content-type": "application/json", ...headers }, body: body === undefined ? undefined : JSON.stringify(body) }), b);
  return { status: r.status, body: r.status === 204 ? null : ((await r.json()) as any) };
};
let rid = 0;
const mcp = async (b: RelayBackend, token: string, name: string, args: unknown) =>
  ((await (
    await handleMcp(new Request("https://relay.test/mcp", { method: "POST", headers: { "content-type": "application/json", authorization: `Bearer ${token}` }, body: JSON.stringify({ jsonrpc: "2.0", id: ++rid, method: "tools/call", params: { name, arguments: args } }) }), b)
  ).json()) as any).result.structuredContent;

const SENT = "so I was thinking. we could move the router upstairs maybe";
const LONG = (SENT + " tomorrow").split(" ");

describe("partials during an open shake turn", () => {
  it("go out by meaning (sentence end / N words), to every agent, then ONE utterance", async () => {
    const w = await setup();
    const turn = await w.shake({ words: LONG });
    const seen = await readUntil(w.room, w.b, (e) => e.type === "utterance");
    const partials = seen.filter((e) => e.type === "utterance.partial");
    // never per word: 11 words → a sentence end at word 4, then every 8 new words
    expect(partials.length).toBeGreaterThanOrEqual(1);
    expect(partials.length).toBeLessThan(LONG.length / 2);
    expect(partials[0].data).toMatchObject({ turn_id: turn, seq: 1, text_so_far: "so I was thinking.", new_text: "so I was thinking.", elapsed_ms: expect.any(Number) });
    for (const [i, p] of partials.entries()) expect(p.data.seq).toBe(i + 1);
    // the drafter got the same partials (they are for every agent)
    const a = await readUntil(w.room, w.a, (e) => e.type === "utterance");
    expect(a.filter((e) => e.type === "utterance.partial").map((e) => e.id)).toEqual(partials.map((e) => e.id));
    const utt = seen.find((e) => e.type === "utterance")!;
    expect(utt.data).toMatchObject({ turn_id: turn, text: LONG.join(" ") });
    expect(utt.data.claimed).toBeUndefined();
    // and the activity log counts them on the utterance (no entry per partial)
    const act = (await w.room.ownerActivity(w.link, { action: "get", query: { kind: "device" } })) as any;
    const u = act.result.entries.find((e: any) => e.ev === "utterance");
    expect(u.data).toMatchObject({ partials: partials.length, drafts: 0, words: LONG.length });
    expect(act.result.entries.filter((e: any) => e.ev === "utterance.partial")).toEqual([]);
  });

  it("none for a turn shorter than the lead (calibration's ~3 s)", async () => {
    const w = await setup({ timings: { partialLeadMs: 5000 } });
    await w.shake({ words: LONG });
    const seen = await readUntil(w.room, w.b, (e) => e.type === "utterance");
    expect(seen.map((e) => e.type)).toEqual(["shake", "utterance"]);
  });
});

describe("human_speaking: agents never speak during an open turn", () => {
  it("say / talk / ask / mood / show / listen / state patches are refused 409 human_speaking; reads are not; after the utterance say works", async () => {
    const w = await setup();
    const turn = await w.shake({ words: LONG, chunkMs: 40 });
    await readUntil(w.room, w.b, (e) => e.type === "utterance.partial");
    const hush = { ok: false, error: "human_speaking", details: { turn_id: turn } };
    expect(await w.room.command(w.b, "say", { text: "hello!" })).toMatchObject(hush);
    expect(await w.room.command(w.a, "say", { text: "hello!", in_reply_to: turn })).toMatchObject(hush);
    expect(await w.room.talk(w.b, { say: "hi" })).toMatchObject(hush);
    expect(await w.room.talk(w.b, { listen: true })).toMatchObject(hush);
    expect(await w.room.ask(w.b, { say: "ok?", options: ["yes", "no"] })).toMatchObject(hush);
    expect(await w.room.ask(w.b, { options: ["yes"] })).toMatchObject(hush);
    expect(await w.room.command(w.b, "mood", { mood: "happy" })).toMatchObject(hush);
    expect(await w.room.command(w.b, "show", { text: "hi" })).toMatchObject(hush);
    expect(await w.room.command(w.b, "listen", {})).toMatchObject(hush);
    expect(await w.room.command(w.b, "state", { state: { palette: "green" } })).toMatchObject(hush);
    // REST: 409 with the turn id; MCP: the same error
    const r = await rest(w.backend, w.b, "POST", `/v1/devices/${DEVICE}/talk`, { say: "hi" });
    expect([r.status, r.body.error, r.body.turn_id]).toEqual([409, "human_speaking", turn]);
    expect(await mcp(w.backend, w.b, "molty_say", { text: "hi" })).toMatchObject({ ok: false, error: "human_speaking", turn_id: turn });
    // nothing reached the device
    expect(w.dev.log.filter((l) => l.kind === "cmd" && (l.name === "say" || l.name === "mood" || l.name === "show"))).toEqual([]);
    // reads go through
    expect((await w.room.command(w.b, "status", {})).ok).toBe(true);
    expect((await w.room.command(w.b, "state", {})).ok).toBe(true);
    await readUntil(w.room, w.b, (e) => e.type === "utterance");
    const ok = await w.room.command(w.b, "say", { text: "Here's your day.", in_reply_to: turn });
    expect(ok.ok).toBe(true);
  });

  it("the turn closes however its listen ends (a box failure, the device dropping): never stuck in human_speaking", async () => {
    const w = await setup();
    const turn = await w.shake({ words: LONG, chunkMs: 40 });
    await readUntil(w.room, w.b, (e) => e.type === "utterance.partial");
    // the device goes away mid-turn: the listen fails (device_offline), the turn closes with it
    w.sock.close(1006, "gone");
    await until(async () => (await jobRec(w.kv, turn))?.status === "failed");
    const back = await connect(w.room, w.host, undefined, LINK, { caps: CAPS });
    device(w.room, back, () => ({ words: ["hi"] }));
    const out = await w.room.command(w.b, "say", { text: "anyone there?" });
    expect(out.ok).toBe(true);
  });
});

describe("drafts", () => {
  it("held + pre-synthesised while the human talks; at the end the opener plays at once, claims the turn, and the follow-up queues behind it with no gap", async () => {
    const w = await setup({ ttsMs: 30 });
    const words = "what's on my calendar. for today".split(" ");
    const turn = await w.shake({ words, chunkMs: 40 });
    const seen = await readUntil(w.room, w.a, (e) => e.type === "utterance.partial");
    const p = seen.find((e) => e.type === "utterance.partial")!;
    expect(p.data.text_so_far).toBe("what's on my calendar.");
    const held = await rest(w.backend, w.a, "POST", `/v1/devices/${DEVICE}/draft`, { turn_id: turn, based_on: p.data.seq, opener: "Let me look.", max_new_words: 5 });
    expect(held).toMatchObject({ status: 200, body: { ok: true, result: { turn_id: turn, based_on: 1, status: "held", max_new_words: CALIBRATION.threshold.initial } } });
    // pre-synthesis: the box got the opener while the human was still talking
    await until(() => w.box.tts.some((t) => t.text === "Let me look."));
    const synthAt = w.box.tts.find((t) => t.text === "Let me look.")!.at;
    // the end: draft.used to the drafter only, before the utterance (claimed)
    const mine = await readUntil(w.room, w.a, (e) => e.type === "utterance", seen);
    const used = mine.find((e) => e.type === "draft.used")!;
    expect(used.data).toMatchObject({ turn_id: turn, based_on: 1, opener: "Let me look.", new_words: 2, job: expect.any(String) });
    expect(mine.findIndex((e) => e.type === "draft.used")).toBeLessThan(mine.findIndex((e) => e.type === "utterance"));
    expect(mine.find((e) => e.type === "utterance")!.data.claimed).toBe(true);
    const theirs = await readUntil(w.room, w.b, (e) => e.type === "utterance");
    expect(theirs.map((e) => e.type)).not.toContain("draft.used");
    const listenEnd = w.dev.log.find((l) => l.kind === "listen-end")!.at;
    expect(synthAt).toBeLessThan(listenEnd);
    // the opener's say went to the device right after the turn, with nobody calling anything
    await until(() => w.dev.log.some((l) => l.name === "say"));
    const sayCmd = w.dev.log.find((l) => l.name === "say")!;
    expect(sayCmd.args.text).toBe("Let me look.");
    expect(sayCmd.at - listenEnd).toBeLessThan(150);
    // the turn is the drafter's: the other agent is refused; the drafter's follow-up queues behind the opener
    expect(await w.room.command(w.b, "say", { text: "Me too!" })).toMatchObject({ ok: false, error: "turn_claimed" });
    const follow = await w.room.command(w.a, "say", { text: "You have two meetings.", in_reply_to: turn });
    expect(follow.ok).toBe(true);
    const fj = (follow as any).result.job;
    expect((await jobRec(w.kv, fj)).status).toBe("queued");
    // its speech is synthesised while the opener plays, its say goes out the moment the opener is done
    await until(() => w.dev.log.filter((l) => l.name === "say").length === 2);
    const says = w.dev.log.filter((l) => l.name === "say");
    expect(says[1].args.text).toBe("You have two meetings.");
    const followSynth = w.box.tts.find((t) => t.text === "You have two meetings.")!.at;
    expect(followSynth).toBeLessThan(says[1].at);
    await until(async () => (await jobRec(w.kv, fj))?.status === "done");
    const done = await jobRec(w.kv, fj);
    expect(done.result.timings.turn).toMatchObject({ id: turn });
    // the drafter's own activity: the draft call and draft.used; the other agent sees none of it
    const actA = (await w.room.activityLog(w.a, {})) as any;
    expect(actA.result.entries.some((e: any) => e.ev === "draft.used" && e.data.opener === "Let me look.")).toBe(true);
    const actB = (await w.room.activityLog(w.b, {})) as any;
    expect(actB.result.entries.some((e: any) => String(e.ev ?? "").startsWith("draft"))).toBe(false);
  });

  it("dropped when late words exceed the threshold or flip the meaning: draft.dropped to the drafter, nothing played, the turn stays open to answer", async () => {
    for (const [words, reason] of [
      ["can you set an alarm. for seven and also one for eight and another at nine thirty please", "new_words"],
      ["can you set an alarm. for seven but not today", "meaning"],
    ] as const) {
      const w = await setup();
      const turn = await w.shake({ words: words.split(" "), chunkMs: 25 });
      const seen = await readUntil(w.room, w.a, (e) => e.type === "utterance.partial");
      const p = seen.find((e) => e.type === "utterance.partial")!;
      expect((await w.room.draft(w.a, { turn_id: turn, based_on: p.data.seq, opener: "Sure, setting it." })).ok).toBe(true);
      const mine = await readUntil(w.room, w.a, (e) => e.type === "utterance", seen);
      const dropped = mine.find((e) => e.type === "draft.dropped")!;
      expect(dropped.data).toMatchObject({ turn_id: turn, based_on: p.data.seq, reason });
      expect(mine.find((e) => e.type === "utterance")!.data.claimed).toBeUndefined();
      expect((await readUntil(w.room, w.b, (e) => e.type === "utterance")).map((e) => e.type)).not.toContain("draft.dropped");
      await new Promise((r) => setTimeout(r, 60));
      expect(w.dev.log.filter((l) => l.name === "say")).toEqual([]);
      // anyone may answer it (the first claims it)
      expect((await w.room.command(w.b, "say", { text: "Okay.", in_reply_to: turn })).ok).toBe(true);
      const stats = (await w.room.speechStats(w.link, { action: "get" })) as any;
      expect(stats.result.drafts).toMatchObject({ made: 1, used: 0, dropped: { [reason]: 1 } });
    }
  });

  it("validation: only the open turn, a partial it sent, a short opener; replaced by a newer one; capped per turn; the first holding draft wins", async () => {
    const w = await setup();
    const turn = await w.shake({ words: "what's on my calendar. for today".split(" "), chunkMs: 40 });
    const seen = await readUntil(w.room, w.a, (e) => e.type === "utterance.partial");
    expect(await w.room.draft(w.a, { turn_id: turn, based_on: 9, opener: "Hm." })).toMatchObject({ ok: false, error: "invalid_args" });
    expect(await w.room.draft(w.a, { turn_id: `${DEVICE}.j_notthisturn00`, based_on: 1, opener: "Hm." })).toMatchObject({ ok: false, error: "turn_closed" });
    expect(await w.room.draft(w.a, { turn_id: turn, based_on: 1, opener: "word ".repeat(30) })).toMatchObject({ ok: false, error: "invalid_args" });
    expect(await w.room.draft(w.a, { turn_id: turn, based_on: 1, opener: "Hm.", max_new_words: 99 })).toMatchObject({ ok: false, error: "invalid_args" });
    expect(await w.room.draft(w.a, { turn_id: turn, based_on: 1, opener: "Hm.", say: "x" })).toMatchObject({ ok: false, error: "invalid_args" });
    expect((await w.room.draft(w.a, { turn_id: turn, based_on: 1, opener: "Hmm, let me see." })).ok).toBe(true);
    // re-based with the same words: the audio already synthesised is kept (no second TTS request)
    expect((await w.room.draft(w.a, { turn_id: turn, based_on: 1, opener: "Hmm, let me see." })).ok).toBe(true);
    expect(w.box.tts.filter((t) => t.text === "Hmm, let me see.")).toHaveLength(1);
    expect((await w.room.draft(w.a, { turn_id: turn, based_on: 1, opener: "One sec." })).ok).toBe(true);
    expect(await w.room.draft(w.a, { turn_id: turn, based_on: 1, opener: "Again." })).toMatchObject({ ok: false, error: "rate_limited" });
    // the other agent drafts later: the drafter's (earlier) one wins, the other's is turn_claimed
    expect((await w.room.draft(w.b, { turn_id: turn, based_on: 1, opener: "Checking!" })).ok).toBe(true);
    await readUntil(w.room, w.a, (e) => e.type === "utterance", seen);
    // everything the drafter got (ride-alongs of its own draft calls included): re-read from the start
    const mine = ((await w.room.events(w.a, { since: 0, wait_ms: 0 })) as any).result.events as AgentEvent[];
    expect(mine.filter((e) => e.type === "draft.dropped").map((e) => e.data.reason)).toEqual(["replaced", "replaced"]);
    expect(mine.find((e) => e.type === "draft.used")!.data.opener).toBe("One sec.");
    const theirs = await readUntil(w.room, w.b, (e) => e.type === "utterance");
    expect(theirs.find((e) => e.type === "draft.dropped")!.data.reason).toBe("turn_claimed");
    // after the turn: closed
    expect(await w.room.draft(w.a, { turn_id: turn, based_on: 1, opener: "Late." })).toMatchObject({ ok: false, error: "turn_closed" });
    expect((await rest(w.backend, w.a, "POST", `/v1/devices/${DEVICE}/draft`, { turn_id: turn, based_on: 1, opener: "Late." })).status).toBe(409);
  });

  it("nobody spoke: drafts dropped no_speech, no utterance, no opener", async () => {
    const w = await setup();
    const turn = await w.shake({ words: "uh well. hmm".split(" "), final: "", chunkMs: 30 });
    const seen = await readUntil(w.room, w.a, (e) => e.type === "utterance.partial");
    expect((await w.room.draft(w.a, { turn_id: turn, based_on: 1, opener: "Yes?" })).ok).toBe(true);
    const mine = await readUntil(w.room, w.a, (e) => e.type === "draft.dropped", seen);
    expect(mine.find((e) => e.type === "draft.dropped")!.data.reason).toBe("no_speech");
    expect(mine.some((e) => e.type === "utterance")).toBe(false);
    await new Promise((r) => setTimeout(r, 60));
    expect(w.dev.log.filter((l) => l.name === "say")).toEqual([]);
  });
});

describe("react", () => {
  it("a mood over the listening (react:true: not an explicit mood, no sound), back to listening, at most one per 3 s", async () => {
    const w = await setup();
    const turn = await w.shake({ words: LONG, chunkMs: 40 });
    await readUntil(w.room, w.a, (e) => e.type === "utterance.partial");
    const r1 = await rest(w.backend, w.a, "POST", `/v1/devices/${DEVICE}/react`, { turn_id: turn, mood: "happy" });
    expect(r1).toMatchObject({ status: 200, body: { ok: true, result: { turn_id: turn, shown: "happy" } } });
    const moods = () => w.dev.log.filter((l) => l.name === "mood");
    expect(moods()[0].args).toEqual({ mood: "happy", hold_s: 0, react: true });
    // the second within 3 s (any agent: one screen): 429 with retry_after_ms
    const r2 = await rest(w.backend, w.b, "POST", `/v1/devices/${DEVICE}/react`, { turn_id: turn, mood: "surprised" });
    expect([r2.status, r2.body.error, r2.body.retry_after_ms > 0]).toEqual([429, "rate_limited", true]);
    // back to listening after the show time; never a say, never an audio frame
    await until(() => moods().length === 2);
    expect(moods()[1].args).toEqual({ mood: "listening", hold_s: 0, react: true });
    expect(w.dev.log.filter((l) => l.name === "say" || l.kind === "frame")).toEqual([]);
    // only listed moods; actions none yet; after the turn: closed
    expect(await w.room.react(w.a, { turn_id: turn, mood: "speaking" })).toMatchObject({ ok: false, error: "invalid_args" });
    expect(await w.room.react(w.a, { turn_id: turn, action: "dance" })).toMatchObject({ ok: false, error: "invalid_args" });
    expect(await w.room.react(w.a, { turn_id: turn, mood: "happy", action: "nod" })).toMatchObject({ ok: false, error: "invalid_args" });
    await readUntil(w.room, w.a, (e) => e.type === "utterance");
    expect(await w.room.react(w.a, { turn_id: turn, mood: "happy" })).toMatchObject({ ok: false, error: "turn_closed" });
    expect(((await w.room.speechStats(w.link, { action: "get" })) as any).result.reacts).toBe(1);
  });

  it("an action is the device's gesture (act, screen only), under the same rate limit; in the turn trace with partials and drafts", async () => {
    const w = await setup();
    const turn = await w.shake({ words: "what's on my calendar. for today and tomorrow".split(" "), chunkMs: 40 });
    const seen = await readUntil(w.room, w.a, (e) => e.type === "utterance.partial");
    const r = await rest(w.backend, w.a, "POST", `/v1/devices/${DEVICE}/react`, { turn_id: turn, action: "nod" });
    expect(r).toMatchObject({ status: 200, body: { ok: true, result: { turn_id: turn, shown: "nod" } } });
    expect(w.dev.log.filter((l) => l.name === "act").map((l) => l.args)).toEqual([{ action: "nod" }]);
    expect(w.dev.log.filter((l) => l.name === "mood" || l.name === "say")).toEqual([]); // no restore needed, no sound
    expect(await w.room.react(w.b, { turn_id: turn, mood: "happy" })).toMatchObject({ ok: false, error: "rate_limited" });
    // a plain molty_act meanwhile is the human's time too
    expect(await w.room.command(w.b, "act", { action: "wiggle" })).toMatchObject({ ok: false, error: "human_speaking" });
    expect((await w.room.draft(w.a, { turn_id: turn, based_on: seen.find((e) => e.type === "utterance.partial")!.data.seq, opener: "Let me look." })).ok).toBe(true);
    await readUntil(w.room, w.a, (e) => e.type === "utterance", seen);
    // the listen's turn trace (activity job entry): partials, the draft, both reacts and the outcome — counts, never words
    await until(async () => (((await w.room.ownerActivity(w.link, { action: "get", query: { job: turn, kind: "job" } })) as any).result.entries.length > 0));
    const job = ((await w.room.ownerActivity(w.link, { action: "get", query: { job: turn, kind: "job" } })) as any).result.entries[0];
    const relay = job.data.trace.relay;
    expect(relay.partials.length).toBeGreaterThan(0);
    expect(relay.partials[0]).toEqual([1, expect.any(Number), 4]);
    expect(relay.drafts).toEqual([[1, expect.any(Number)]]);
    expect(relay.reacts).toEqual([["nod", expect.any(Number), "ok"], ["happy", expect.any(Number), "rate_limited"]]);
    expect(relay.draft).toMatchObject({ used: 1, new_words: expect.any(Number), opener_at: expect.any(Number) });
    expect(relay.utterance).toEqual(expect.any(Number));
    expect(JSON.stringify(job.data.trace)).not.toContain("calendar");
  });

  it("firmware without the react cap: unsupported (a plain mood would cancel the turn's acknowledgement)", async () => {
    const w = await setup({ caps: CAPS.filter((c) => c !== "react" && c !== "act") });
    const turn = await w.shake({ words: LONG, chunkMs: 40 });
    await readUntil(w.room, w.a, (e) => e.type === "utterance.partial");
    expect(await w.room.react(w.a, { turn_id: turn, mood: "happy" })).toMatchObject({ ok: false, error: "unsupported" });
    expect(await w.room.react(w.a, { turn_id: turn, action: "nod" })).toMatchObject({ ok: false, error: "unsupported" });
    expect(w.dev.log.filter((l) => l.name === "mood")).toEqual([]);
  });
});

describe("speech stats + calibration", () => {
  it("every heard turn is a sample; after enough turns the partial policy is calibrated (visible to the owner and in the activity log)", async () => {
    const w = await setup();
    for (let i = 0; i < CALIBRATION.minTurns; i++) {
      const turn = await w.shake({ words: LONG, chunkMs: 10 }, `shake-${i}`);
      await until(async () => (await jobRec(w.kv, turn))?.status === "done");
      await until(() => !w.dev.log.some((l) => l.name === "say") || true);
    }
    const out = (await rest(w.backend, "", "GET", `/v1/devices/${DEVICE}/speech`, undefined, { "x-molty-link-code": w.link })).body;
    expect(out.ok).toBe(true);
    expect(out.result).toMatchObject({ turns: { sampled: CALIBRATION.minTurns }, words: { median: LONG.length }, policy: { source: "calibrated", every_words: 4 }, drafts: { threshold: CALIBRATION.threshold.initial } });
    const act = (await w.room.ownerActivity(w.link, { action: "get", query: { kind: "settings" } })) as any;
    expect(act.result.entries.find((e: any) => e.ev === "calibration").data).toMatchObject({ partials: { source: "calibrated", every_words: 4 } });
    // the owner route is the link code's only: wrong code 401; DELETE resets
    expect((await rest(w.backend, "", "GET", `/v1/devices/${DEVICE}/speech`, undefined, { "x-molty-link-code": "ZZZZ2345" })).status).toBe(401);
    expect((await rest(w.backend, "", "DELETE", `/v1/devices/${DEVICE}/speech`, undefined, { "x-molty-link-code": w.link })).body.result.turns.sampled).toBe(0);
  });
});

describe("notes", () => {
  it("private to each agent token (REST + MCP), size-capped, values never logged, gone with the token", async () => {
    const w = await setup();
    const put = await rest(w.backend, w.a, "PUT", "/v1/notes", { key: "ledger", value: [{ tried: "react happy", response: "smiled" }] });
    expect(put).toMatchObject({ status: 200, body: { ok: true, result: { key: "ledger" } } });
    expect((await rest(w.backend, w.a, "GET", "/v1/notes?key=ledger")).body.result.value).toEqual([{ tried: "react happy", response: "smiled" }]);
    // the other agent sees nothing of it, and cannot touch it
    expect((await rest(w.backend, w.b, "GET", "/v1/notes?key=ledger")).body.result).toEqual({ key: "ledger", value: null, updated: null });
    expect((await rest(w.backend, w.b, "GET", "/v1/notes")).body.result.keys).toEqual([]);
    expect((await rest(w.backend, w.b, "DELETE", "/v1/notes?key=ledger")).body.result.removed).toBe(false);
    expect((await mcp(w.backend, w.b, "molty_notes", { action: "put", key: "ledger", value: "mine" })).ok).toBe(true);
    expect((await mcp(w.backend, w.a, "molty_notes", { action: "get", key: "ledger" })).value).toEqual([{ tried: "react happy", response: "smiled" }]);
    expect((await mcp(w.backend, w.b, "molty_notes", { action: "get", key: "ledger" })).value).toBe("mine");
    // caps: one value, the key count, the total
    const big = "x".repeat(NOTES_LIMITS.valueBytes);
    const tooBig = await rest(w.backend, w.a, "PUT", "/v1/notes", { key: "big", value: big });
    expect([tooBig.status, tooBig.body.error]).toEqual([413, "too_large"]);
    const chunk = "y".repeat(NOTES_LIMITS.valueBytes - 10);
    for (let i = 0; i < 3; i++) expect((await w.room.notes(w.a, { action: "put", key: `k${i}`, value: chunk })).ok).toBe(true);
    expect(await w.room.notes(w.a, { action: "put", key: "k3", value: chunk })).toMatchObject({ ok: false, error: "too_large" });
    expect(await w.room.notes(w.b, { action: "put", key: "k3", value: chunk })).toMatchObject({ ok: true }); // b's own budget
    for (let i = 0; i < NOTES_LIMITS.keys - 1; i++) await w.room.notes(w.b, { action: "put", key: `n${i}`, value: 1 });
    expect(await w.room.notes(w.b, { action: "put", key: "one-too-many", value: 1 })).toMatchObject({ ok: false, error: "too_large" });
    expect(await w.room.notes(w.a, { action: "put", key: "bad key!", value: 1 })).toMatchObject({ ok: false, error: "invalid_args" });
    // never in the activity log (op + key only), for the owner or the agent
    const act = JSON.stringify(((await w.room.ownerActivity(w.link, { action: "get", query: {} })) as any).result.entries);
    expect(act).toContain('"op":"notes"');
    expect(act).not.toContain("smiled");
    expect(act).not.toContain("yyyy");
    // revoking the token deletes its notes; the other's stay
    await w.room.revoke(w.a);
    expect([...w.kv.data.keys()].filter((k) => k.startsWith("note:"))).toHaveLength(1);
  });
});

describe("webhooks: utterance.partial only on explicit opt-in", () => {
  it("default events: utterance, not partials; events [utterance.partial] gets them; draft.dropped only to the drafter's webhook", async () => {
    const w = await setup();
    const plain = await w.room.webhook(w.b, { action: "set", spec: { url: "https://hooks.example.org/plain" } });
    expect(plain.ok).toBe(true);
    expect((plain as any).result.webhook.events).not.toContain("utterance.partial");
    const opt = await w.room.webhook(w.a, { action: "set", spec: { url: "https://hooks.example.org/partials", events: ["utterance.partial", "utterance", "draft.dropped", "draft.used"] } });
    expect(opt.ok).toBe(true);
    const turn = await w.shake({ words: "can you set an alarm. for seven but not today".split(" "), chunkMs: 25 });
    const seen = await readUntil(w.room, w.a, (e) => e.type === "utterance.partial");
    await w.room.draft(w.a, { turn_id: turn, based_on: seen.find((e) => e.type === "utterance.partial")!.data.seq, opener: "Sure." });
    await readUntil(w.room, w.a, (e) => e.type === "utterance", seen);
    for (let i = 0; i < 6; i++) await w.room.alarm();
    const types = (path: string) => w.sent.filter((s) => s.url.endsWith(path)).map((s) => JSON.parse(s.body).event.type);
    await until(() => types("/partials").includes("utterance"));
    expect(types("/plain")).not.toContain("utterance.partial");
    expect(types("/plain")).toContain("utterance");
    expect(types("/plain")).not.toContain("draft.dropped");
    expect(types("/partials")).toEqual(expect.arrayContaining(["utterance.partial", "draft.dropped", "utterance"]));
    // the internal recipient never leaves the relay
    for (const s of w.sent) expect(JSON.parse(s.body).event.to).toBeUndefined();
  });
});
