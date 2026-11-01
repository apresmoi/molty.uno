// Quick phrases and the auto filler (core/phrases.ts, docs/AGENT-LINK.md "Quick phrases"):
// agent-owned phrases with pre-synthesised, cached audio; `quick {id}` plays one with no TTS
// call; a say / talk right after it queues behind it (no audio_busy, synthesis overlapped);
// Molty fills a silent turn itself after the owner's delay.
import { describe, expect, it } from "vitest";
import type { RelayBackend } from "../src/core/api.js";
import { BUILTIN_PHRASES, FILLER_DEFAULT_MS, FILLER_POOL, PHRASE_LIMITS, PhraseCache, type UtteranceCategory, type UtteranceClassifier, fillerMs, keywordClassifier, pickFiller } from "../src/core/phrases.js";
import { keywordCategory } from "../src/core/reflex.js";
import { decodeAudioFrame, encodeAudioFrame } from "../src/core/protocol.js";
import type { DeviceRoom } from "../src/core/room.js";
import { MemoryKV } from "../src/core/store.js";
import { handleMcp } from "../src/surfaces/mcp.js";
import { handleRest } from "../src/surfaces/rest.js";
import { DEVICE, type FakeSocket, FakeVoice, LINK, connect, flush, makeRoom } from "./helpers.js";

const FAST = { cmdTimeoutMs: 300, pairWindowMs: 400, sayChunkSamples: 1600, sayLeadFrames: 2, listenGraceMs: 200, sayDoneGraceMs: 300 };
const CAPS = ["working", "say", "say_cues", "listen", "listen_stop", "listen_window", "ack", "shake_talk"];
const FILLER_TEXTS = FILLER_POOL.map((id) => BUILTIN_PHRASES.find((p) => p.id === id)!.text);

/** TTS that takes a while (a cloud provider): what a queued talk overlaps with the phrase. */
class SlowVoice extends FakeVoice {
  delayMs = 0;
  async tts(text: string, opts: { voice?: string }) {
    if (this.delayMs) await new Promise((r) => setTimeout(r, this.delayMs));
    return super.tts(text, opts);
  }
  /** STT that takes a while: the transcript (and so the filler's arming) comes after the listen's end. */
  sttDelayMs = 0;
  async stt(pcm: Int16Array) {
    if (this.sttDelayMs) await new Promise((r) => setTimeout(r, this.sttDelayMs));
    return super.stt(pcm);
  }
}

interface Dev {
  /** The listen ack's filler_ms (the owner's setting); undefined = older firmware (not reported). */
  fillerMs?: number;
  /** The listen's end frame extras. */
  end?: Record<string, unknown>;
}

type Ev = { at: number; kind: "cmd" | "frame" | "done" | "refused" | "listen_end"; name?: string; args?: any; job?: string; end?: boolean; head?: any; bytes?: number };

/**
 * The firmware's audio rules: a say / listen is refused `busy` while another one runs (until
 * its say.done / the listen's end frame); say.done comes when playback ends (the stream's
 * audio length after its first frame), never before its end frame.
 */
function device(room: DeviceRoom, sock: FakeSocket, dev: Dev) {
  const log: Ev[] = [];
  let busy: string | undefined;
  const origText = sock.sendText.bind(sock);
  const origBin = sock.sendBinary.bind(sock);
  const reply = (id: string, res: object) => setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "res", id, ...res })), 1);
  sock.sendText = (t: string) => {
    origText(t);
    const f = JSON.parse(t);
    if (f.t === "pair.request") {
      setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "pair.result", req: f.req, ok: true, link_code: LINK })), 2);
      return;
    }
    if (f.t !== "cmd") return;
    if ((f.name === "say" || f.name === "listen") && busy) {
      log.push({ at: Date.now(), kind: "refused", name: f.name, args: f.args });
      return reply(f.id, { ok: false, error: "busy" });
    }
    log.push({ at: Date.now(), kind: "cmd", name: f.name, args: f.args });
    if (f.name === "say") busy = f.args.job;
    if (f.name === "listen") {
      busy = f.args.job;
      reply(f.id, { ok: true, result: { job: f.args.job, ms: 30000, ack: "chirp", ...(dev.fillerMs !== undefined ? { filler_ms: dev.fillerMs } : {}) } });
      setTimeout(() => {
        void room.socketMessage(sock, encodeAudioFrame({ s: f.args.job, seq: 0, kind: "pcm16", rate: 16000, end: false }, new Uint8Array(new Int16Array(3200).fill(8000).buffer)));
        busy = undefined;
        log.push({ at: Date.now(), kind: "listen_end", job: f.args.job });
        void room.socketMessage(sock, new TextEncoder().encode(JSON.stringify({ s: f.args.job, seq: 1, kind: "pcm16", rate: 16000, end: true, ...(dev.end ?? { reason: "silence", speech_ms: 900 }) })));
      }, 15);
      return;
    }
    reply(f.id, { ok: true, result: {} });
  };
  const play = new Map<string, { first: number; samples: number }>();
  sock.sendBinary = (b: Uint8Array) => {
    origBin(b);
    const fr = decodeAudioFrame(b)!;
    const nl = b.indexOf(10);
    const head = JSON.parse(new TextDecoder().decode(nl < 0 ? b : b.subarray(0, nl)));
    log.push({ at: Date.now(), kind: "frame", job: fr.header.s, end: fr.header.end, head, bytes: fr.payload.byteLength });
    const p = play.get(fr.header.s) ?? { first: Date.now(), samples: 0 };
    p.samples += fr.payload.byteLength / 2;
    play.set(fr.header.s, p);
    if (fr.header.end)
      setTimeout(() => {
        if (busy === fr.header.s) busy = undefined;
        log.push({ at: Date.now(), kind: "done", job: fr.header.s });
        void room.socketMessage(sock, JSON.stringify({ t: "evt", name: "say.done", data: { job: fr.header.s } }));
      }, Math.max(10, p.first + p.samples / 16 - Date.now()));
  };
  const says = () => log.filter((e) => e.kind === "cmd" && e.name === "say").map((e) => ({ at: e.at, ...e.args }));
  const moods = () => log.filter((e) => e.kind === "cmd" && e.name === "mood").map((e) => ({ at: e.at, ...e.args }));
  const refused = () => log.filter((e) => e.kind === "refused");
  const firstFrame = (job: string) => log.find((e) => e.kind === "frame" && e.job === job);
  const done = (job: string) => log.find((e) => e.kind === "done" && e.job === job);
  const listenEnd = () => log.filter((e) => e.kind === "listen_end").at(-1);
  return { log, says, moods, refused, firstFrame, done, listenEnd };
}

async function setup(dev: Dev = {}, opts: { ttsDelayMs?: number; sttDelayMs?: number; samples?: number; classifier?: UtteranceClassifier; heard?: string; fetch?: (r: Request) => Promise<Response> } = {}) {
  const voice = new SlowVoice();
  voice.ttsSamples = opts.samples ?? 4800; // 0.3 s
  voice.delayMs = opts.ttsDelayMs ?? 0;
  voice.sttDelayMs = opts.sttDelayMs ?? 0;
  if (opts.heard) voice.sttText = opts.heard;
  const ctx = makeRoom({ timings: FAST, voice, classifier: opts.classifier, fetch: opts.fetch });
  const sock = await connect(ctx.room, ctx.host, undefined, LINK, { caps: CAPS });
  const d = device(ctx.room, sock, dev);
  const pairAgent = async (name: string) => {
    const out = await ctx.room.pair({ device_id: DEVICE, link_code: LINK, agent_name: name });
    if (!out.ok || out.result.status !== "approved") throw new Error(JSON.stringify(out));
    await flush();
    return out.result.agent_token;
  };
  const backend: RelayBackend = { device: () => ctx.room };
  return { ...ctx, voice, sock, dev: d, pairAgent, backend, token: await pairAgent("grokbot") };
}

const until = async (cond: () => boolean | Promise<boolean>, ms = 3000) => {
  const end = Date.now() + ms;
  while (!(await cond()) && Date.now() < end) await new Promise((r) => setTimeout(r, 5));
};
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

let rid = 0;
const mcp = async (b: RelayBackend, token: string, name: string, args: unknown) =>
  ((await (
    await handleMcp(
      new Request("https://relay.test/mcp", {
        method: "POST",
        headers: { "content-type": "application/json", authorization: `Bearer ${token}` },
        body: JSON.stringify({ jsonrpc: "2.0", id: ++rid, method: "tools/call", params: { name, arguments: args } }),
      }),
      b,
    )
  ).json()) as any).result;

const rest = async (b: RelayBackend, token: string, method: string, path: string, body?: unknown) => {
  const r = await handleRest(new Request(`https://relay.test${path}`, { method, headers: { authorization: `Bearer ${token}`, "content-type": "application/json" }, ...(body !== undefined ? { body: JSON.stringify(body) } : {}) }), b);
  return { status: r.status, body: (await r.json()) as any };
};

const quick = (w: { room: DeviceRoom; token: string }, args: Record<string, unknown>, token = w.token) => w.room.command(token, "quick", args) as Promise<any>;

describe("phrase registration", () => {
  it("set / list / delete per agent; caps on count and length; ids and text validated", async () => {
    const w = await setup();
    const set = await mcp(w.backend, w.token, "molty_phrases", { action: "set", phrases: [{ id: "hmm", text: "Hmmmm, okay…" }, { id: "think", text: "[thinking] Ooh, let me think." }] });
    expect(set.isError).toBe(false);
    expect(set.structuredContent.phrases).toEqual([
      { id: "hmm", text: "Hmmmm, okay…", for: "any", ready: true },
      { id: "think", text: "Ooh, let me think.", for: "any", ready: true }, // cues stripped (kept as marks), audio cached
    ]);
    expect(set.structuredContent.builtins.find((b: any) => b.id === "hmm")).toMatchObject({ shadowed: true });
    expect(set.structuredContent.limits).toEqual({ per_agent: PHRASE_LIMITS.perAgent, text_max: PHRASE_LIMITS.textMax });
    // upsert: same id replaced, list via REST
    await rest(w.backend, w.token, "POST", `/v1/devices/${DEVICE}/phrases`, { phrases: [{ id: "hmm", text: "Hm." }] });
    const list = await rest(w.backend, w.token, "GET", `/v1/devices/${DEVICE}/phrases`);
    expect(list.status).toBe(200);
    expect(list.body.result.phrases.map((p: any) => [p.id, p.text])).toEqual([["hmm", "Hm."], ["think", "Ooh, let me think."]]);
    // caps
    const many = Array.from({ length: PHRASE_LIMITS.perAgent - 1 }, (_, i) => ({ id: `p${i}`, text: `Phrase ${i}.` }));
    expect((await w.room.phrases(w.token, { action: "set", phrases: many })) as any).toMatchObject({ ok: false, error: "invalid_args", message: expect.stringContaining(`at most ${PHRASE_LIMITS.perAgent}`) });
    expect((await w.room.phrases(w.token, { phrases: [{ id: "long", text: "x".repeat(PHRASE_LIMITS.textMax + 1) }] })) as any).toMatchObject({ ok: false, error: "invalid_args" });
    expect((await w.room.phrases(w.token, { phrases: [{ id: "long", text: "x".repeat(PHRASE_LIMITS.textMax) }] })) as any).toMatchObject({ ok: true });
    for (const bad of [{ id: "Bad Id", text: "x" }, { id: "ok", text: "" }, { id: "ok", text: "[nod]" }, { id: "ok", text: "two\nlines" }, { id: "ok", text: "x", extra: 1 }]) {
      expect((await w.room.phrases(w.token, { phrases: [bad] })) as any, JSON.stringify(bad)).toMatchObject({ ok: false, error: "invalid_args" });
    }
    // delete
    const del = await rest(w.backend, w.token, "DELETE", `/v1/devices/${DEVICE}/phrases?id=hmm,long`);
    expect(del.body.result.phrases.map((p: any) => p.id)).toEqual(["think"]);
    expect(((await w.room.phrases(w.token, { action: "delete", ids: ["*"] })) as any).result.phrases).toEqual([]);
  });

  it("isolation: an agent sees and plays only its own phrases (and the built-ins)", async () => {
    const w = await setup();
    const other = await w.pairAgent("claude");
    await w.room.phrases(w.token, { phrases: [{ id: "mine", text: "Grok here." }] });
    expect(((await w.room.phrases(other, {})) as any).result.phrases).toEqual([]);
    expect(await quick(w, { id: "mine" }, other)).toMatchObject({ ok: false, error: "not_found" });
    const b = await quick(w, { id: "one_sec" }, other);
    expect(b).toMatchObject({ ok: true, result: { phrase: "one_sec", text: "One sec." } });
    // revoking forgets an agent's phrases
    await w.room.revoke(w.token);
    expect(await w.kv.get(`phr:${(await (await import("../src/core/crypto.js")).sha256Hex(w.token))}`)).toBeUndefined();
  });
});

describe("quick: cached audio, zero TTS", () => {
  it("set pre-synthesises once; quick streams the cached audio + word cues with no TTS call; a miss synthesises once then caches", async () => {
    const w = await setup();
    await w.room.phrases(w.token, { phrases: [{ id: "think", text: "Let me think about that." }] });
    expect(w.voice.ttsCalls.map((c) => c.text)).toEqual(["Let me think about that."]);
    const r = await quick(w, { id: "think" });
    expect(r).toMatchObject({ ok: true, result: { job: expect.stringMatching(/\.j_/), phrase: "think", cached: true } });
    await until(() => !!w.dev.done(r.result.job));
    expect(w.voice.ttsCalls).toHaveLength(1); // no TTS for the play
    expect(w.dev.says()[0]).toMatchObject({ text: "Let me think about that.", cues: true });
    const frames = w.dev.log.filter((e) => e.kind === "frame" && e.job === r.result.job);
    expect(frames.reduce((n, f) => n + (f.bytes ?? 0), 0)).toBe(4800 * 2); // the cached PCM, all of it
    expect(frames.flatMap((f) => f.head.w ?? []).map((c: number[]) => c[0])).toEqual([0, 1, 2, 3, 4]); // every word cued
    const job = (await w.room.job(w.token, r.result.job, 2000)) as any;
    expect(job.result).toMatchObject({ kind: "quick", status: "done", result: { phrase: "think", cached: true, timings: { cached: true } } });
    // a built-in nobody warmed: synthesised once on the first play, cached for the next
    const a = await quick(w, { id: "on_it" });
    expect(a.result.cached).toBe(false);
    await until(() => !!w.dev.done(a.result.job));
    await w.room.job(w.token, a.result.job, 2000);
    const b = await quick(w, { id: "on_it" });
    expect(b.result.cached).toBe(true);
    await until(() => !!w.dev.done(b.result.job));
    expect(w.voice.ttsCalls.filter((c) => c.text === "On it.")).toHaveLength(1);
  });

  it("the cache is bounded (least recently used evicted) and a lost chunk is a miss, not garbage", async () => {
    const kv = new MemoryKV();
    let t = 0;
    const cache = new PhraseCache(kv, () => ++t);
    const pcm = new Int16Array(40000).fill(3); // 80 KB: two chunks
    for (let i = 0; i < PHRASE_LIMITS.cacheEntries + 3; i++) await cache.put("v|", { text: `p${i}`, pcm, cues: [[0, 0]], voice: "fake" });
    expect(await cache.size()).toBe(PHRASE_LIMITS.cacheEntries);
    expect(await cache.has("v|", "p0")).toBe(false); // the oldest went first
    expect((await cache.get("v|", `p${PHRASE_LIMITS.cacheEntries + 2}`))!.pcm).toEqual(pcm);
    const dataKeys = [...kv.data.keys()].filter((k) => k.startsWith("pcmd:"));
    expect(dataKeys).toHaveLength(PHRASE_LIMITS.cacheEntries * 2);
    kv.data.delete(dataKeys[0]);
    const lostText = (await kv.get<any>(`pcm:${dataKeys[0].slice("pcmd:".length, dataKeys[0].lastIndexOf(":"))}`))!.text;
    expect(await cache.get("v|", lostText)).toBeUndefined();
    expect(await cache.has("v|", lostText)).toBe(false); // dropped: re-synthesised on the next play
    expect(await cache.put("v|", { text: "too long", pcm: new Int16Array(PHRASE_LIMITS.maxSamples + 1), cues: [], voice: "fake" })).toBe(false);
  });
});

describe("quick then talk: one seamless answer", () => {
  it("the talk queues right behind the phrase: no audio_busy, never refused by the device, its TTS overlapped the phrase", async () => {
    const w = await setup({}, { ttsDelayMs: 200 });
    await w.room.phrases(w.token, { phrases: [{ id: "hmm", text: "Hmm, let me see." }] });
    const q = await quick(w, { id: "hmm" });
    expect(q.ok).toBe(true);
    const t0 = Date.now();
    const talk = (await w.room.talk(w.token, { say: "It is sunny.", listen: false })) as any;
    expect(talk).toMatchObject({ ok: true, result: { status: "done", said: "It is sunny." } });
    expect(w.dev.refused()).toEqual([]); // the device never saw a say while another one played
    const says = w.dev.says();
    expect(says.map((s) => s.text)).toEqual(["Hmm, let me see.", "It is sunny."]);
    const qDone = w.dev.done(q.result.job)!.at;
    const talkCmd = says[1].at;
    const talkFirst = w.dev.firstFrame(says[1].job)!.at;
    expect(talkCmd).toBeGreaterThanOrEqual(qDone);
    // no TTS wait after the phrase: the answer was synthesised while the phrase played (TTS takes 200 ms here)
    expect(talkFirst - qDone).toBeLessThan(120);
    expect(talkFirst - t0).toBeGreaterThan(0);
    expect(talk.result.timings.queue).toBeGreaterThan(0); // it waited for the phrase (startJob after)
    // nothing in between: no thinking mood after a phrase that has a follow-up
    expect(w.dev.moods().filter((m) => m.at > qDone && m.at < talkFirst)).toEqual([]);
  });

  it("a phrase with nothing after it puts thinking back up (the agent is still working)", async () => {
    const w = await setup();
    const q = await quick(w, { id: "let_me_think" });
    await until(() => w.dev.moods().length > 0);
    expect(w.dev.moods()[0]).toMatchObject({ mood: "thinking", hold_s: expect.any(Number) });
    expect(w.dev.moods()[0].at).toBeLessThanOrEqual(w.dev.done(q.result.job)!.at + 50);
  });

  it("a say's [working] becomes the mood at its end; an explicit mood during the say wins", async () => {
    const w = await setup();
    const a = await w.room.command(w.token, "say", { text: "Looking. [working]" });
    expect(a.ok).toBe(true);
    await until(() => w.dev.moods().some((x) => x.mood === "working"));
    expect(w.dev.moods().map((x) => x.mood)).toContain("working");

    const b = await setup();
    const said = (await b.room.command(b.token, "say", { text: "Looking. [working]" })) as any;
    expect(said.ok).toBe(true);
    await b.room.command(b.token, "mood", { mood: "idle" });
    await until(() => !!b.dev.done(said.result.job));
    await new Promise((r) => setTimeout(r, 400));
    expect(b.dev.moods().map((x) => x.mood)).not.toContain("working");
  });

  it("only its own agent may queue behind a phrase: another agent gets audio_busy (or turn_claimed in a turn)", async () => {
    const w = await setup();
    const other = await w.pairAgent("claude");
    const q = await quick(w, { id: "hmm" });
    expect(q.ok).toBe(true);
    expect(await w.room.command(other, "say", { text: "Me too." })).toMatchObject({ ok: false, error: "audio_busy" });
    // one queued job at most per chain end: a second follow-up of a non-quick is busy
    const first = await w.room.command(w.token, "say", { text: "Answer." });
    expect(first.ok).toBe(true);
    expect(await w.room.command(w.token, "say", { text: "Again." })).toMatchObject({ ok: false, error: "audio_busy" });
    await until(() => w.dev.says().length === 2 && !!w.dev.done(w.dev.says()[1].job));
    expect(w.dev.refused()).toEqual([]);
  });
});

describe("auto filler", () => {
  it("talk heard words, the agent stays silent: Molty says a filler after the owner's delay, keeps thinking, logs it; the agent's own quick is then skipped and its talk plays", async () => {
    const w = await setup({ fillerMs: 1500 });
    const talk = (await w.room.talk(w.token, { say: "What's up?" })) as any;
    expect(talk.result).toMatchObject({ heard: "hello molty", filler_in_ms: expect.any(Number) });
    expect(talk.result.filler_in_ms).toBeLessThanOrEqual(1500);
    const end = w.dev.listenEnd()!.at;
    await until(() => w.dev.says().length === 2, 2500);
    const filler = w.dev.says()[1];
    expect(FILLER_TEXTS).toContain(filler.text);
    expect(filler.at - end).toBeGreaterThanOrEqual(1450);
    expect(filler.at - end).toBeLessThan(1500 + 250);
    await until(() => !!w.dev.done(filler.job));
    await until(() => w.dev.moods().some((m) => m.at >= w.dev.done(filler.job)!.at - 50 && m.mood === "thinking"));
    // the agent's own quick, after Molty filled: skipped, not a second filler
    const q = await quick(w, { id: "let_me_think" });
    expect(q).toMatchObject({ ok: true, result: { skipped: "filled", filler: { phrase: expect.any(String) } } });
    expect(w.dev.says()).toHaveLength(2);
    const ans = (await w.room.talk(w.token, { say: "Sunny all day.", listen: false })) as any;
    expect(ans.ok).toBe(true);
    expect(w.dev.says().map((s) => s.text).at(-1)).toBe("Sunny all day.");
    // the owner's activity log shows Molty's own filler, attributed to no agent
    const log = (await w.room.ownerActivity(LINK, { action: "get", query: { kind: "job" } })) as any;
    const entry = log.result.entries.find((e: any) => e.op === "quick");
    expect(entry).toMatchObject({ job: filler.job, ok: true, data: { filler: true, turn: talk.result.job } });
    expect(entry.agent).toBeUndefined();
    // ...and in that turn's trace: Molty's filler, when it was heard after the human stopped
    const trace = (await w.room.ownerActivity(LINK, { action: "get", query: { kind: "trace", job: talk.result.job } })) as any;
    expect(trace.result.entries.find((e: any) => e.ev === "filler")).toMatchObject({ job: talk.result.job, data: { job: filler.job, phrase: expect.any(String), first_audio_ms: expect.any(Number) } });
    expect(trace.result.entries.find((e: any) => e.ev === "filler").data.first_audio_ms).toBeGreaterThanOrEqual(1450);
  }, 10000);

  it("the agent's reply arriving while the filler plays queues right after it (no cut, no audio_busy, no device refusal)", async () => {
    const w = await setup({ fillerMs: 1500 }, { samples: 9600 });
    await w.room.talk(w.token, { say: "Hi." });
    await until(() => w.dev.says().length === 2, 2500);
    const filler = w.dev.says()[1];
    const ans = (await w.room.talk(w.token, { say: "Here you go.", listen: false })) as any;
    expect(ans).toMatchObject({ ok: true, result: { status: "done" } });
    expect(w.dev.refused()).toEqual([]);
    const fDone = w.dev.done(filler.job)!.at;
    const a = w.dev.says()[2];
    expect(a.text).toBe("Here you go.");
    expect(a.at).toBeGreaterThanOrEqual(fDone);
    expect(a.at - fDone).toBeLessThan(80);
    // the filler played whole: all its frames, its end frame, then say.done
    expect(w.dev.log.filter((e) => e.kind === "frame" && e.job === filler.job).reduce((n, f) => n + (f.bytes ?? 0), 0)).toBe(9600 * 2);
  }, 10000);

  it("no filler when the agent answers (a quick, a talk, a mood) within the delay, when it is off, or when nothing was heard", async () => {
    // a quick within the delay: only the agent's phrase
    const a = await setup({ fillerMs: 1500 });
    await a.room.talk(a.token, { say: "Hi." });
    await sleep(300);
    await quick(a, { id: "hmm" });
    await sleep(1700);
    expect(a.dev.says().map((s) => s.text)).toEqual(["Hi.", "Hmm…"]);
    // a mood within the delay (the agent shows it is on it)
    const b = await setup({ fillerMs: 1500 });
    await b.room.talk(b.token, { say: "Hi." });
    await b.room.command(b.token, "mood", { mood: "thinking", hold_s: 30 });
    // off (the owner's setting 0)
    const c = await setup({ fillerMs: 0 });
    const ct = (await c.room.talk(c.token, { say: "Hi." })) as any;
    expect(ct.result.filler_in_ms).toBeUndefined();
    // nothing heard
    const d = await setup({ fillerMs: 1500, end: { reason: "no_speech", speech_ms: 0 } });
    await d.room.talk(d.token, { say: "Hi." });
    await sleep(1900);
    for (const x of [b, c, d]) expect(x.dev.says().map((s) => s.text)).toEqual(["Hi."]);
  }, 10000);

  it("older firmware (no filler_ms in the listen ack): the default delay", async () => {
    expect(fillerMs(undefined)).toBe(FILLER_DEFAULT_MS);
    expect(fillerMs(0)).toBe(0);
    expect(fillerMs(1500)).toBe(1500);
    expect(fillerMs(400)).toBe(400);
    expect(fillerMs(399)).toBe(FILLER_DEFAULT_MS);
    expect(fillerMs(999)).toBe(999);
    expect(fillerMs(9000)).toBe(FILLER_DEFAULT_MS);
    const w = await setup({});
    const t = (await w.room.talk(w.token, { say: "Hi." })) as any;
    expect(t.result.filler_in_ms).toBeGreaterThan(FILLER_DEFAULT_MS - 500);
  });

  it("context-picked: the phrases made for the utterance's category, else the `any` ones; never the same twice in a row", () => {
    const ids = (cat: UtteranceCategory, last?: string) => new Set([0, 0.2, 0.4, 0.6, 0.8, 0.999].map((r) => pickFiller(BUILTIN_PHRASES, cat, last, r)!.id));
    expect(ids("question")).toEqual(new Set(["good_question"]));
    expect(ids("request")).toEqual(new Set(["on_it", "checking"]));
    expect(ids("hard")).toEqual(new Set(["let_me_think"]));
    expect(ids("other")).toEqual(new Set(["hmm", "one_sec", "let_me_see"])); // never "Good question." after a statement
    for (const last of ["hmm", "one_sec", "let_me_see", "on_it", "checking"]) for (const cat of ["other", "request"] as const) expect(ids(cat, last).has(last)).toBe(false);
    expect(pickFiller(BUILTIN_PHRASES, "hard", "let_me_think", 0)!.id).toBe("let_me_think"); // the only fit: a repeat beats silence
    // an agent's pool without an `any` phrase has nothing for a statement
    expect(pickFiller([{ id: "q", text: "Ooh, good one.", for: "question" }], "other", undefined)).toBeUndefined();
    expect(pickFiller([{ id: "a", text: "Mmm." }], "question", undefined)!.id).toBe("a");
  });

  it("the keyword classifier is the reflex's keywords engine (one implementation)", () => {
    for (const t of ["Can you set an alarm for seven?", "Why is the sky blue?", "What time is it?", "I had a great day today."]) expect(keywordClassifier.classify(t)).toBe(keywordCategory(t));
  });

  it("shake to talk: the utterance says when Molty will fill; the filler claims nothing — the first agent to answer still owns the turn", async () => {
    const w = await setup({ fillerMs: 1500 });
    const other = await w.pairAgent("claude");
    await w.room.socketMessage(w.sock, JSON.stringify({ t: "evt", name: "shake", data: { peak_g: 2.4, reversals: 5, ms: 900, listen: "shake-1" } }));
    let utt: any;
    await until(async () => {
      const ev = ((await w.room.events(w.token, { wait_ms: 0 })) as any).result.events;
      utt ??= ev.find((e: any) => e.type === "utterance");
      return !!utt;
    });
    expect(utt.data.filler_in_ms).toBeGreaterThan(0);
    expect(utt.data.filler_in_ms).toBeLessThanOrEqual(1500);
    await until(() => w.dev.says().length === 1, 2500);
    expect(FILLER_TEXTS).toContain(w.dev.says()[0].text);
    // the filler did not claim the turn: the first agent's answer does (queued after the filler), the other's is refused
    const a = (await w.room.talk(other, { say: "On it.", listen: false, in_reply_to: utt.data.turn_id })) as any;
    expect(a).toMatchObject({ ok: true, result: { status: "done", timings: { turn: { id: utt.data.turn_id } } } });
    expect(await w.room.talk(w.token, { say: "Me!", listen: false, in_reply_to: utt.data.turn_id })).toMatchObject({ ok: false, error: "turn_claimed" });
    expect(await quick(w, { id: "hmm", in_reply_to: utt.data.turn_id })).toMatchObject({ ok: false, error: "turn_claimed" });
  }, 10000);

  it("an agent acting on the utterance before the relay has even finished the turn still stands the filler down; no agent paired: no filler", async () => {
    const w = await setup({ fillerMs: 1500 });
    const room = w.room as any;
    const append = room.appendEvent.bind(room);
    // the fastest possible agent: it reacts the moment the utterance is appended (before the listen job ends)
    room.appendEvent = async (type: string, data: unknown, relay?: boolean) => {
      await append(type, data, relay);
      if (type === "utterance") await w.room.command(w.token, "mood", { mood: "thinking", hold_s: 20 });
    };
    await w.room.socketMessage(w.sock, JSON.stringify({ t: "evt", name: "shake", data: { peak_g: 2.4, reversals: 5, ms: 900, listen: "shake-1" } }));
    await until(() => w.dev.moods().some((m) => m.hold_s === 20));
    await sleep(1900);
    expect(w.dev.says()).toEqual([]);
    // nobody paired to answer: Molty does not pretend someone is thinking
    const lone = await setup({ fillerMs: 1500 });
    await lone.room.revoke(lone.token);
    await lone.room.socketMessage(lone.sock, JSON.stringify({ t: "evt", name: "shake", data: { peak_g: 2.4, reversals: 5, ms: 900, listen: "shake-1" } }));
    await sleep(2000);
    expect(lone.dev.says()).toEqual([]);
  }, 10000);

  it("a filler only skips a quick for ITS turn: a new turn (the human spoke again) plays the agent's quick", async () => {
    const w = await setup({ fillerMs: 1500 });
    const shake = (n: number) => w.room.socketMessage(w.sock, JSON.stringify({ t: "evt", name: "shake", data: { peak_g: 2.4, reversals: 5, ms: 900, listen: `shake-${n}` } }));
    await shake(1);
    await until(() => w.dev.says().length === 1, 2500); // Molty's filler for turn 1
    await until(() => !!w.dev.done(w.dev.says()[0].job));
    await w.room.job(w.token, w.dev.says()[0].job, 0);
    await sleep(50);
    await shake(2); // the human speaks again, the agent still silent
    await until(() => w.dev.log.filter((e) => e.kind === "listen_end").length === 2);
    await sleep(100);
    const q = await quick(w, { id: "let_me_think" });
    expect(q).toMatchObject({ ok: true, result: { phrase: "let_me_think" } });
    expect(q.result.skipped).toBeUndefined();
  }, 10000);

  it("a quick claims the human's turn like a say: another agent is then refused", async () => {
    const w = await setup({ fillerMs: 0 });
    const other = await w.pairAgent("claude");
    await w.room.socketMessage(w.sock, JSON.stringify({ t: "evt", name: "shake", data: { peak_g: 2.4, reversals: 5, ms: 900, listen: "shake-1" } }));
    let turn: string | undefined;
    await until(async () => {
      const ev = ((await w.room.events(w.token, { wait_ms: 0 })) as any).result.events;
      turn ??= ev.find((e: any) => e.type === "utterance")?.data.turn_id;
      return !!turn;
    });
    const q = await quick(w, { id: "hmm", in_reply_to: turn });
    expect(q).toMatchObject({ ok: true });
    expect(await w.room.talk(other, { say: "Me!", listen: false })).toMatchObject({ ok: false, error: "turn_claimed" });
    // the quick is the agent's reply in the turn trace (op quick)
    await w.room.job(w.token, q.result.job, 2000);
    await until(async () => ((await w.room.ownerActivity(LINK, { action: "get", query: { kind: "trace", job: turn } })) as any).result.entries.some((e: any) => e.ev === "reply"));
    const tr = (await w.room.ownerActivity(LINK, { action: "get", query: { kind: "trace", job: turn } })) as any;
    expect(tr.result.entries.find((e: any) => e.ev === "reply")).toMatchObject({ agent: "grokbot", data: { op: "quick", job: q.result.job } });
    expect(await w.room.talk(w.token, { say: "Here it is.", listen: false })).toMatchObject({ ok: true, result: { status: "done" } });
  });
});

describe("whose fillers, and which one", () => {
  const fillerOf = async (w: Awaited<ReturnType<typeof setup>>, turn: string) => {
    await until(async () => ((await w.room.ownerActivity(LINK, { action: "get", query: { kind: "trace", job: turn } })) as any).result.entries.some((e: any) => e.ev === "filler"), 4000);
    return ((await w.room.ownerActivity(LINK, { action: "get", query: { kind: "trace", job: turn } })) as any).result.entries.find((e: any) => e.ev === "filler")?.data;
  };

  it("builtins:false — that agent neither sees nor plays the built-ins; others still do", async () => {
    const w = await setup();
    const other = await w.pairAgent("claude");
    const off = (await w.room.phrases(w.token, { builtins: false })) as any;
    expect(off.result).toMatchObject({ builtins: [], builtins_enabled: false, phrases: [] });
    expect(await quick(w, { id: "hmm" })).toMatchObject({ ok: false, error: "not_found" });
    expect(await quick(w, { id: "hmm" }, other)).toMatchObject({ ok: true });
    // its own still play; back on with builtins:true
    await w.room.phrases(w.token, { phrases: [{ id: "mine", text: "Mm-hm." }] });
    expect(((await w.room.phrases(w.token, {})) as any).result).toMatchObject({ builtins_enabled: false, phrases: [{ id: "mine" }] });
    expect(((await w.room.phrases(w.token, { builtins: true })) as any).result.builtins.length).toBe(BUILTIN_PHRASES.length);
    expect((await w.room.phrases(w.token, { builtins: "no" })) as any).toMatchObject({ ok: false, error: "invalid_args" });
  });

  it("Molty's filler is the last answering agent's own (filler:true, by category), else the built-ins; builtins off and none of its own → no filler", async () => {
    // the agent's own filler phrases: the one made for a request wins for a request
    const w = await setup({ fillerMs: 1500 }, { heard: "please turn on the lights" });
    await w.room.phrases(w.token, {
      phrases: [
        { id: "mm", text: "Mm, sure.", filler: true },
        { id: "go", text: "Right away!", filler: true, for: "request" },
        { id: "note", text: "Noted." }, // not a filler
      ],
    });
    const t = (await w.room.talk(w.token, { say: "Hi." })) as any;
    const d = await fillerOf(w, t.result.job);
    expect(d).toMatchObject({ phrase: "go", text: "Right away!", category: "request", classifier: "keywords", source: "agent", owner: "grokbot" });
    // fillers of ANOTHER agent are not used: the last answerer has none → the built-ins
    const v = await setup({ fillerMs: 1500 }, { heard: "what time is it?" });
    const other = await v.pairAgent("claude");
    await v.room.phrases(other, { phrases: [{ id: "mm", text: "Mm, sure.", filler: true }] });
    const t2 = (await v.room.talk(v.token, { say: "Hi." })) as any;
    expect(await fillerOf(v, t2.result.job)).toMatchObject({ phrase: "good_question", category: "question", source: "builtin" });
    // builtins off, no fillers of its own: Molty stays quiet
    const x = await setup({ fillerMs: 1500 });
    await x.room.phrases(x.token, { builtins: false });
    await x.room.talk(x.token, { say: "Hi." });
    await sleep(2000);
    expect(x.dev.says().map((s) => s.text)).toEqual(["Hi."]);
  }, 15000);

  it("the last agent to claim a shake turn owns Molty's next filler", async () => {
    const w = await setup({ fillerMs: 1500 }, { heard: "I had a long day" });
    const claude = await w.pairAgent("claude");
    await w.room.phrases(claude, { phrases: [{ id: "mm", text: "Mmm, I hear you.", filler: true }] });
    const shake = (n: number) => w.room.socketMessage(w.sock, JSON.stringify({ t: "evt", name: "shake", data: { peak_g: 2.4, reversals: 5, ms: 900, listen: `shake-${n}` } }));
    await shake(1);
    let turn: string | undefined;
    await until(async () => {
      const ev = ((await w.room.events(claude, { wait_ms: 0 })) as any).result.events;
      turn ??= ev.find((e: any) => e.type === "utterance")?.data.turn_id;
      return !!turn;
    });
    expect((await w.room.command(claude, "say", { text: "Tell me.", in_reply_to: turn })).ok).toBe(true);
    await until(() => w.dev.says().length === 1 && !!w.dev.done(w.dev.says()[0].job));
    await sleep(50);
    await shake(2); // nobody answers this one
    await until(() => w.dev.log.filter((e) => e.kind === "listen_end").length === 2);
    await until(() => w.dev.says().length === 2, 3000);
    expect(w.dev.says()[1].text).toBe("Mmm, I hear you.");
  }, 10000);


  it("a pluggable classifier picks the category (a reaction engine); a slow or broken one falls back to the keywords", async () => {
    const w = await setup({ fillerMs: 1500 }, { classifier: { name: "reflex-test", classify: async () => "hard" } });
    const t = (await w.room.talk(w.token, { say: "Hi." })) as any;
    expect(await fillerOf(w, t.result.job)).toMatchObject({ phrase: "let_me_think", category: "hard", classifier: "reflex-test" });
    const broken = await setup({ fillerMs: 1500 }, { heard: "what time is it?", classifier: { name: "broken", classify: () => { throw new Error("down"); } } });
    const t2 = (await broken.room.talk(broken.token, { say: "Hi." })) as any;
    expect(await fillerOf(broken, t2.result.job)).toMatchObject({ category: "question", classifier: "keywords" });
    const slow = await setup({ fillerMs: 1500 }, { heard: "what time is it?", classifier: { name: "slow", classify: () => new Promise((r) => setTimeout(() => r("request"), 3000)) } });
    const t3 = (await slow.room.talk(slow.token, { say: "Hi." })) as any;
    expect(await fillerOf(slow, t3.result.job)).toMatchObject({ category: "question", classifier: "keywords" });
  }, 15000);
});

describe("a fast filler (400..1500 ms): the delay is counted from the listen's end, nothing adds to it", () => {
  const traceOf = async (w: Awaited<ReturnType<typeof setup>>, turn: string) => {
    await until(async () => ((await w.room.ownerActivity(LINK, { action: "get", query: { kind: "trace", job: turn } })) as any).result.entries.some((e: any) => e.ev === "filler"), 3000);
    return ((await w.room.ownerActivity(LINK, { action: "get", query: { kind: "trace", job: turn } })) as any).result.entries.find((e: any) => e.ev === "filler")?.data;
  };
  const slowClassifier: UtteranceClassifier = { name: "slow", classify: () => new Promise((r) => setTimeout(() => r("hard"), 3000)) };

  it("400 ms: fires ~400 ms after the listen's end even with a slow STT and a classifier that is not in yet (the keywords pick; no wait for its budget)", async () => {
    const w = await setup({ fillerMs: 400 }, { sttDelayMs: 250, heard: "what time is it?", classifier: slowClassifier });
    const talk = (await w.room.talk(w.token, { say: "Hi." })) as any;
    expect(talk.result.filler_in_ms).toBeLessThanOrEqual(400 - 200); // the STT ate most of the delay already
    const end = w.dev.listenEnd()!.at;
    await until(() => w.dev.says().length === 2, 2000);
    const filler = w.dev.says()[1];
    expect(filler.at - end).toBeGreaterThanOrEqual(380);
    expect(filler.at - end).toBeLessThan(400 + 120); // the old await on the classifier's 400 ms budget would land ~650+
    const d = await traceOf(w, talk.result.job);
    expect(d).toMatchObject({ category: "question", classifier: "keywords", phrase: "good_question" });
    expect(d.first_audio_ms).toBeLessThan(400 + 120);
  }, 10000);

  it("a classifier that is in by the moment still decides (it ran in parallel with the delay)", async () => {
    const w = await setup({ fillerMs: 500 }, { classifier: { name: "quickish", classify: () => new Promise((r) => setTimeout(() => r("hard"), 150)) } });
    const talk = (await w.room.talk(w.token, { say: "Hi." })) as any;
    const end = w.dev.listenEnd()!.at;
    await until(() => w.dev.says().length === 2, 2000);
    expect(w.dev.says()[1].at - end).toBeLessThan(500 + 120);
    expect(await traceOf(w, talk.result.job)).toMatchObject({ phrase: "let_me_think", category: "hard", classifier: "quickish" });
  }, 10000);

  it("the delay is from the listen's end, not from when the transcript came back: a slow STT shortens the wait, never lengthens it", async () => {
    const w = await setup({ fillerMs: 800 }, { sttDelayMs: 300 });
    await w.room.talk(w.token, { say: "Hi." });
    const end = w.dev.listenEnd()!.at;
    await until(() => w.dev.says().length === 2, 2000);
    const at = w.dev.says()[1].at - end;
    expect(at).toBeGreaterThanOrEqual(780);
    expect(at).toBeLessThan(800 + 120);
  }, 10000);

  it("shake to talk at 400 ms: the filler follows the utterance's emit within the delay", async () => {
    const w = await setup({ fillerMs: 400 }, { classifier: slowClassifier });
    await w.room.socketMessage(w.sock, JSON.stringify({ t: "evt", name: "shake", data: { peak_g: 2.4, reversals: 5, ms: 900, listen: "shake-1" } }));
    await until(() => !!w.dev.listenEnd());
    const end = w.dev.listenEnd()!.at;
    await until(() => w.dev.says().length === 1, 2000);
    expect(w.dev.says()[0].at - end).toBeGreaterThanOrEqual(380);
    expect(w.dev.says()[0].at - end).toBeLessThan(400 + 120);
  }, 10000);

  it("agent first wins: its answer right after the turn (well inside 400 ms) skips the filler", async () => {
    const w = await setup({ fillerMs: 400 });
    await w.room.talk(w.token, { say: "Hi." });
    expect((await w.room.talk(w.token, { say: "Noon.", listen: false })) as any).toMatchObject({ ok: true });
    await sleep(700);
    expect(w.dev.says().map((s) => s.text)).toEqual(["Hi.", "Noon."]);
    // a mood counts too
    const m = await setup({ fillerMs: 400 });
    await m.room.talk(m.token, { say: "Hi." });
    await m.room.command(m.token, "mood", { mood: "thinking", hold_s: 20 });
    await sleep(700);
    expect(m.dev.says().map((s) => s.text)).toEqual(["Hi."]);
  }, 10000);

  it("filler first: the agent's reply arriving while it plays queues right behind it, seamless", async () => {
    const w = await setup({ fillerMs: 500 }, { samples: 9600 });
    await w.room.talk(w.token, { say: "Hi." });
    await until(() => w.dev.says().length === 2, 2000);
    const filler = w.dev.says()[1];
    expect(FILLER_TEXTS).toContain(filler.text);
    const ans = (await w.room.talk(w.token, { say: "Here you go.", listen: false })) as any;
    expect(ans).toMatchObject({ ok: true, result: { status: "done" } });
    expect(w.dev.refused()).toEqual([]);
    const fDone = w.dev.done(filler.job)!.at;
    const a = w.dev.says()[2];
    expect(a.text).toBe("Here you go.");
    expect(a.at).toBeGreaterThanOrEqual(fDone);
    expect(a.at - fDone).toBeLessThan(80);
  }, 10000);
});
