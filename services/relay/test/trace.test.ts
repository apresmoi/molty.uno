// Turn trace (core/trace.ts, docs/AGENT-LINK.md "Turn trace"): the device's frame fields, the voice
// box's stt.trace / stt.hint, the relay's ordering of resent frames, the trace stored with the job
// (capped), `trace` activity entries (webhook deliveries, replies with client_timing), and the
// owner's "keep audio" setting (core/audio-keep.ts) with its owner-only WAV route.
import { describe, expect, it } from "vitest";
import type { RelayBackend } from "../src/core/api.js";
import { ActivityLog } from "../src/core/activity.js";
import { AUDIO_KEEP, AudioKeep, wav } from "../src/core/audio-keep.js";
import { CommandError, clientTiming, validateAsk, validateCommand, validateTalk } from "../src/core/commands.js";
import { decodeAudioFrame, encodeAudioFrame } from "../src/core/protocol.js";
import type { DeviceRoom } from "../src/core/room.js";
import { MemoryKV } from "../src/core/store.js";
import { ListenTrace, SeqOrder, TRACE_LIMITS, capTrace, cleanBoxTrace } from "../src/core/trace.js";
import { handleRest } from "../src/surfaces/rest.js";
import { DEVICE, type FakeSocket, LINK, connect, flush, makeRoom } from "./helpers.js";

const FAST = { cmdTimeoutMs: 300, pairWindowMs: 400, sayChunkSamples: 1600, sayLeadFrames: 2, listenGraceMs: 200, sayDoneGraceMs: 300, boxSttFinalMs: 400, askGraceMs: 100 };
const CAPS = ["say", "listen", "listen_stop", "listen_window", "ack", "shake_talk", "trace", "hints"];
const enc = new TextEncoder();

const until = async (cond: () => boolean | Promise<boolean>, ms = 2500) => {
  const end = Date.now() + ms;
  while (!(await cond()) && Date.now() < end) await new Promise((r) => setTimeout(r, 5));
};

/** A device that answers commands and plays says; the test streams each listen's frames itself. */
function device(room: DeviceRoom, sock: FakeSocket) {
  const cmds: Array<{ name: string; args: any }> = [];
  const hints: any[] = [];
  const text = sock.sendText.bind(sock);
  const bin = sock.sendBinary.bind(sock);
  let codes = 0;
  sock.sendText = (t: string) => {
    text(t);
    const f = JSON.parse(t);
    if (f.t === "hint") hints.push(f);
    if (f.t === "pair.request") setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "pair.result", req: f.req, ok: true, link_code: ["NEWC2345", "NEWD2345", "NEWE2345", "NEWF2345"][codes++] })), 2);
    if (f.t !== "cmd") return;
    cmds.push({ name: f.name, args: f.args });
    const result = f.name === "listen" ? { job: f.args.job, ms: 30000, ack: "chirp" } : {};
    setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "res", id: f.id, ok: true, result })), 1);
  };
  sock.sendBinary = (b: Uint8Array) => {
    bin(b);
    const fr = decodeAudioFrame(b)!;
    if (fr.header.end) setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "evt", name: "say.done", data: { job: fr.header.s } })), 5);
  };
  /** One listen frame: `n` samples of `fill`, header extras (seq, end, ct, lv, …). */
  const frame = (job: string, h: Record<string, unknown>, n = 4800, fill = 1000) =>
    room.socketMessage(sock, n ? encodeAudioFrame({ s: job, seq: 0, kind: "pcm16", rate: 16000, end: false, ...h } as any, new Uint8Array(new Int16Array(n).fill(fill).buffer)) : enc.encode(JSON.stringify({ s: job, kind: "pcm16", rate: 16000, end: false, ...h })));
  const lastListen = () => cmds.filter((c) => c.name === "listen").at(-1)?.args;
  return { cmds, hints, frame, lastListen };
}

/** A voice box: records control frames and audio; the test speaks for it (say). On stt.end: stt.trace (final) then stt.final. */
function box(room: DeviceRoom, sock: FakeSocket, finalTrace?: Record<string, unknown>, noFinal = false) {
  const control: any[] = [];
  const audio: Record<string, number[]> = {};
  const text = sock.sendText.bind(sock);
  const say = (m: unknown) => room.socketMessage(sock, JSON.stringify(m));
  sock.sendText = (t: string) => {
    text(t);
    const f = JSON.parse(t);
    control.push(f);
    if (f.t === "stt.start") audio[f.id] = [];
    if (f.t === "stt.end" && !noFinal)
      setTimeout(() => {
        if (finalTrace) void say({ t: "stt.trace", id: f.id, trace: finalTrace });
        void say({ t: "stt.final", id: f.id, text: "hi Molty, how are you today?", ms: { final: 7 } });
      }, 3);
  };
  sock.sendBinary = (b: Uint8Array) => {
    const fr = decodeAudioFrame(b)!;
    // the first sample of each chunk tells which device frame it came from
    audio[fr.header.s]?.push(new Int16Array(fr.payload.slice().buffer)[0]);
  };
  const sttId = () => control.filter((f) => f.t === "stt.start").at(-1)?.id as string;
  return { control, audio, say, sttId };
}

async function setup(o: { caps?: string[]; box?: boolean; finalTrace?: Record<string, unknown>; noFinal?: boolean; fetch?: (r: Request) => Promise<Response>; kv?: MemoryKV } = {}) {
  const ctx = makeRoom({
    timings: FAST,
    kv: o.kv,
    scheduler: { setAlarm: async () => {} },
    fetch: o.fetch ?? (async () => new Response("ok")),
    resolveHost: async () => ["93.184.216.34"],
  });
  const sock = await connect(ctx.room, ctx.host, undefined, LINK, { caps: o.caps ?? CAPS });
  const dev = device(ctx.room, sock);
  const pair = async (name: string, role?: "voice") => {
    const code = (await ctx.kv.get<{ code: string }>("link"))!.code;
    const out = await ctx.room.pair({ device_id: DEVICE, link_code: code, agent_name: name, ...(role ? { role } : {}) });
    if (!out.ok || out.result.status !== "approved") throw new Error(JSON.stringify(out));
    await flush();
    return out.result.agent_token;
  };
  const token = await pair("talker");
  let b: ReturnType<typeof box> | undefined;
  if (o.box) {
    const vt = await pair("gpu box", "voice");
    const auth = await ctx.room.voiceAuth(vt);
    if (!auth.ok) throw new Error("voice auth");
    const vs = ctx.host.open();
    ctx.room.voiceOpen(vs, auth.result.hash);
    b = box(ctx.room, vs, o.finalTrace, o.noFinal);
    await ctx.room.socketMessage(vs, JSON.stringify({ t: "voice.hello", name: "gpu box", version: "0.1.0", voices: [], stt: true, tts: true }));
  }
  const code = async () => (await ctx.kv.get<{ code: string }>("link"))!.code;
  const owner = async (query: Record<string, unknown> = {}) => ((await ctx.room.ownerActivity(await code(), { action: "get", query })) as any).result;
  const backend: RelayBackend = { device: () => ctx.room };
  return { ...ctx, sock, dev, token, pair, box: b, code, owner, backend };
}

const jobEntry = async (w: Awaited<ReturnType<typeof setup>>, job: string) => {
  let e: any;
  await until(async () => !!(e = (await w.owner({ job })).entries.find((x: any) => x.kind === "job")));
  return e;
};

const BOX_TRACE = {
  v: 1,
  step_ms: 80,
  skip: 12,
  thr: 0.6,
  hold_s: 1.2,
  audio_s: 1.2,
  act: [[0.3, 1.0]],
  pause: "ffeeddccbb",
  pause_every: 2,
  words: [["hi", 0.32, 0.5], ["Molty,", 0.5, null], ["today?", 0.9, 1.1]],
  end: { at: 1.12, rule: "punct", word: "today?", held_s: 0, ema: 0.71 },
  bogus: "dropped",
};

describe("device frame trace fields (protocol.ts)", () => {
  const dec = (h: Record<string, unknown>) => decodeAudioFrame(enc.encode(JSON.stringify({ s: "j", seq: 1, kind: "pcm16", rate: 16000, end: true, ...h })))!.header;

  it("valid fields are kept; malformed ones are dropped, never fatal", () => {
    expect(dec({ ct: 340, lv: "A0b0C0", cl: 3, rt: 2, wq: 1500, tr: { wall: 8970, cap: 5400, r0: -69, r1: -79, set: 340, setc: 0, gain: 6, pk: 998, sent: 18, fail: 3, drop: 0, dropms: 0, x: 1 } })).toMatchObject({
      ct: 340,
      lv: "a0b0c0",
      cl: 3,
      rt: 2,
      wq: 1500,
      tr: { wall: 8970, cap: 5400, r0: -69, r1: -79, set: 340, setc: 0, gain: 6, pk: 998, sent: 18, fail: 3, drop: 0, dropms: 0 },
    });
    expect(dec({ tr: { x: 1 } })).not.toHaveProperty("tr");
    const bad = dec({ ct: -1, lv: "abc", cl: 1.5, rt: "2", wq: 1e9, tr: { r0: 5, gain: 0, pk: 1001, setc: 2 } });
    for (const k of ["ct", "lv", "cl", "rt", "wq", "tr"]) expect(bad).not.toHaveProperty(k);
    expect(dec({ lv: "zz" })).not.toHaveProperty("lv");
    expect(dec({ cl: 0, rt: 0 })).toEqual({ s: "j", seq: 1, kind: "pcm16", rate: 16000, end: true }); // zeros are "omitted"
  });
});

describe("SeqOrder", () => {
  it("in order passes through; out of order waits for the gap; duplicates are dropped", () => {
    const o = new SeqOrder<string>();
    expect(o.offer(0, "a", 0)).toEqual(["a"]);
    expect(o.offer(2, "c", 1)).toEqual([]);
    expect(o.waitingSince()).toBe(1);
    expect(o.offer(1, "b", 2)).toEqual(["b", "c"]);
    expect(o.offer(1, "b", 3)).toBe("dup");
    expect(o.offer(0, "a", 3)).toBe("dup");
    expect(o.dup).toBe(2);
    expect(o.waitingSince()).toBeUndefined();
  });

  it("a gap given up counts as lost and releases what waited", () => {
    const o = new SeqOrder<number>();
    o.offer(0, 0, 0);
    o.offer(3, 3, 0);
    o.offer(4, 4, 0);
    expect(o.skip()).toEqual([3, 4]);
    expect(o.lost).toEqual([[1, 2]]);
    expect(o.offer(1, 1, 0)).toBe("dup"); // too late: given up
  });

  it("more than holdFrames waiting gives the gap up at once", () => {
    const o = new SeqOrder<number>(3);
    o.offer(0, 0, 0);
    expect(o.offer(2, 2, 0)).toEqual([]);
    expect(o.offer(3, 3, 0)).toEqual([]);
    expect(o.offer(4, 4, 0)).toEqual([]);
    expect(o.offer(5, 5, 0)).toEqual([2, 3, 4, 5]);
    expect(o.lost).toEqual([[1, 1]]);
  });
});

describe("ListenTrace + capTrace", () => {
  it("levels in capture order with lost audio as 00, clipping, frames, the device summary", () => {
    const t = new ListenTrace(1000, "shake");
    t.frame({ s: "j", seq: 0, kind: "pcm16", rate: 16000, end: false, ct: 300, lv: "a0a1a2", cl: 4 }, 9600, 1400);
    t.frame({ s: "j", seq: 2, kind: "pcm16", rate: 16000, end: false, ct: 900, lv: "b0b1b2", rt: 2, wq: 1500 }, 9600, 3000);
    t.frame({ s: "j", seq: 3, kind: "pcm16", rate: 16000, end: true, ct: 1200, lv: "c0", reason: "turn_end", speech_ms: 900, tr: { r0: -69, r1: -79, set: 300, dropms: 300, sent: 4, fail: 2 } }, 3200, 3100);
    t.lost = [[1, 1]];
    const b = t.build() as any;
    expect(b.dev.lv).toBe("a0a1a2000000b0b1b2c0");
    expect(b.dev.lv0).toBe(300);
    expect(b.dev.clip).toEqual([[3, 4]]);
    expect(b.dev.clip_n).toBe(4);
    expect(b.dev.frames).toEqual([[0, 300, 400, 0, null], [2, 900, 2000, 2, 1500], [3, 1200, 2100, 0, null]]);
    expect(b.dev).toMatchObject({ sent: 4, fail: 2, resent: 1, lost: [[1, 1]], lost_ms: 300, rssi: [-69, -79], set: 300, end: "turn_end", speech_ms: 900 });
  });

  it("old firmware: a partial trace (frames + arrival only), no error", () => {
    const t = new ListenTrace(0, "agent");
    t.frame({ s: "j", seq: 0, kind: "pcm16", rate: 16000, end: false }, 9600, 50);
    t.frame({ s: "j", seq: 1, kind: "pcm16", rate: 16000, end: true, reason: "silence" }, 0, 80);
    const b = t.build() as any;
    expect(b).toEqual({ v: 1, trigger: "agent", t0: 0, dev: { frames: [[0, null, 50, 0, null], [1, null, 80, 0, null]], sent: 2, fail: 0, resent: 0, end: "silence" }, relay: {} });
  });

  it("caps at 4096 B: the frame list is trimmed first, the decisions stay", () => {
    const t = new ListenTrace(0, "agent");
    for (let i = 0; i < 400; i++) t.frame({ s: "j", seq: i, kind: "pcm16", rate: 16000, end: false, ct: i * 300, lv: "808182" }, 9600, i * 300 + 40);
    t.box = cleanBoxTrace({ ...BOX_TRACE, pause: "ab".repeat(300) });
    const full = t.build();
    expect(JSON.stringify(full).length).toBeGreaterThan(TRACE_LIMITS.maxBytes);
    const c = capTrace(full) as any;
    expect(JSON.stringify(c).length).toBeLessThanOrEqual(TRACE_LIMITS.maxBytes);
    expect(c.trimmed).toBe(true);
    expect(c.dev.frames).toHaveLength(16); // first and last 8
    expect(c.dev.frames_n).toBe(400);
    expect(typeof c.dev.lv).toBe("string");
    expect(c.box.end.rule).toBe("punct");
    // a trace already under the cap is untouched
    const small = new ListenTrace(0, "agent").build();
    expect(capTrace(small)).toBe(small);
    // just over the cap because of frames only: frames are trimmed to their ends, levels kept whole
    const f = new ListenTrace(0, "agent");
    for (let i = 0; i < 160; i++) f.frame({ s: "j", seq: i, kind: "pcm16", rate: 16000, end: false, ct: i * 300, lv: "808182" }, 9600, i * 300);
    const cf = capTrace(f.build()) as any;
    expect(cf.dev.frames).toHaveLength(16);
    expect(cf.dev.lv).toHaveLength(160 * 6);
  });

  it("cleanBoxTrace keeps known keys only and refuses junk", () => {
    expect(cleanBoxTrace(BOX_TRACE)).not.toHaveProperty("bogus");
    expect(cleanBoxTrace("x")).toBeUndefined();
    expect(cleanBoxTrace({ nope: 1 })).toBeUndefined();
    expect(cleanBoxTrace({ words: "x".repeat(TRACE_LIMITS.boxBytes) })).toBeUndefined();
  });
});

describe("a shake turn's trace (device + box + relay)", () => {
  it("is assembled from the frames (resent, out of order), the box's stt.trace, the shake and the relay's timings; stored with the job, never in the agent's result", async () => {
    const w = await setup({ box: true, finalTrace: { ...BOX_TRACE, final_ms: 7 } });
    await w.room.socketMessage(w.sock, JSON.stringify({ t: "evt", name: "shake", data: { peak_g: 8.2, reversals: 4, ms: 420, listen: "shake-1" } }));
    await until(() => !!w.dev.lastListen());
    const job = w.dev.lastListen().job as string;
    await until(() => !!w.box!.sttId());
    // frame 1 failed twice on the device (resent): it arrives after frame 2
    await w.dev.frame(job, { seq: 0, ct: 340, lv: "a0b0c0", cl: 3, wq: 20 }, 4800, 100);
    await w.dev.frame(job, { seq: 2, ct: 940, lv: "a2a2a2", wq: 30 }, 4800, 300);
    await w.dev.frame(job, { seq: 1, ct: 640, lv: "a1a1a1", rt: 2, wq: 1500 }, 4800, 200);
    await w.dev.frame(job, { seq: 1, ct: 640, lv: "a1a1a1", rt: 3 }, 4800, 200); // a duplicate
    const id = w.box!.sttId();
    await w.box!.say({ t: "stt.hint", id, k: "question" });
    await w.box!.say({ t: "stt.hint", id, k: "nonsense" });
    expect(w.dev.hints).toEqual([{ t: "hint", job, k: "question" }]);
    await w.box!.say({ t: "stt.trace", id, trace: BOX_TRACE });
    await w.box!.say({ t: "stt.turn_end", id });
    await w.dev.frame(job, { seq: 3, end: true, reason: "turn_end", ct: 1240, lv: "90", tr: { wall: 1400, cap: 1060, sent: 4, fail: 2, r0: -69, r1: -79, set: 340, setc: 0, gain: 6, pk: 998 } }, 1600, 400);
    await w.box!.say({ t: "stt.hint", id, k: "pause" }); // after the end: not forwarded
    const e = await jobEntry(w, job);
    expect(w.dev.hints).toHaveLength(1);
    // the box got the audio in seq order
    expect(w.box!.audio[id]).toEqual([100, 200, 300]);
    const t = e.data.trace;
    expect(t).toMatchObject({ v: 1, trigger: "shake" });
    expect(t.dev).toMatchObject({ shake: { peak_g: 8.2, rev: 4, ms: 420 }, set: 340, wall: 1400, cap: 1060, gain: 6, pk: 998, lv: "a0b0c0a1a1a1a2a2a290", lv0: 340, late: 1, clip: [[3, 3]], clip_n: 3, sent: 4, fail: 2, resent: 1, dup: 1, rssi: [-69, -79] });
    expect(t.dev.shake.at).toBeLessThanOrEqual(0);
    expect(t.dev.frames.map((f: number[]) => f[0])).toEqual([0, 2, 1, 3]); // as they arrived (the end frame after the box turn end too)
    expect(t.box).toMatchObject({ final_ms: 7, end: { rule: "punct", word: "today?" }, words: BOX_TRACE.words });
    expect(t.box).not.toHaveProperty("bogus");
    expect(t.relay).toMatchObject({ voice: "box", hints: [["question", expect.any(Number)]] });
    for (const k of ["cmd", "first_frame", "turn_end", "end", "stt", "utterance"]) expect(typeof t.relay[k], k).toBe("number");
    expect(t.relay.utterance).toBeGreaterThanOrEqual(t.relay.end);
    expect(JSON.stringify(t).length).toBeLessThanOrEqual(4096);
    // the agent's view of the job carries no trace
    const view = (await w.room.job(w.token, job, 0)) as any;
    expect(view.result).not.toHaveProperty("trace");
    expect(view.result.result).not.toHaveProperty("trace");
    expect(JSON.stringify(view.result)).not.toContain('"dev"');
    // eslint-disable-next-line no-console
    if (process.env.TRACE_EXAMPLE) console.log(JSON.stringify(t, null, 1));
  });

  it("hints reach only firmware with the `hints` cap", async () => {
    const w = await setup({ box: true, caps: CAPS.filter((c) => c !== "hints") });
    const r = w.room.command(w.token, "listen", {});
    await until(() => !!w.box!.sttId());
    await w.box!.say({ t: "stt.hint", id: w.box!.sttId(), k: "pause" });
    expect(w.dev.hints).toHaveLength(0);
    await w.box!.say({ t: "stt.turn_end", id: w.box!.sttId(), reason: "no_speech" });
    await r;
  });

  it("transcripts off: the trace keeps its timings but loses the box's words", async () => {
    const w = await setup({ box: true, finalTrace: BOX_TRACE });
    await w.room.ownerActivity(await w.code(), { action: "set", transcripts: false });
    void w.room.talk(w.token, {});
    await until(() => !!w.dev.lastListen() && !!w.box!.sttId());
    const job = w.dev.lastListen().job;
    for (let i = 0; i < 8; i++) await w.dev.frame(job, { seq: i, ct: i * 300, lv: "a0a0a0" });
    await w.dev.frame(job, { seq: 8, end: true, reason: "silence", speech_ms: 300 }, 0);
    const e = await jobEntry(w, job);
    expect(e.redacted).toBe(true);
    expect(e.data.trace.box.words).toEqual([]);
    expect(e.data.trace.box.end.word).toBe("");
    // a long hex level series is data, not a "[secret]" (the log's credential scrubber lets it through)
    expect(e.data.trace.dev.lv).toBe("a0".repeat(24));
  });
});

describe("resent / late frames (relay ordering)", () => {
  it("a seq that never comes is given up after 1 s: the end frame waits for it, then the listen ends and the gap is reported lost", async () => {
    const w = await setup({ caps: ["say", "listen"] });
    const job = ((await w.room.command(w.token, "listen", {})) as any).result.job;
    await until(() => !!w.dev.lastListen());
    const t0 = Date.now();
    await w.dev.frame(job, { seq: 0 });
    await w.dev.frame(job, { seq: 2 });
    await w.dev.frame(job, { seq: 3, end: true, reason: "silence", speech_ms: 400 }, 0);
    const v = (await w.room.job(w.token, job, 3000)) as any;
    expect(Date.now() - t0).toBeGreaterThanOrEqual(TRACE_LIMITS.gapMs - 50);
    expect(v.result).toMatchObject({ status: "done", result: { duration_ms: 600 } });
    const e = await jobEntry(w, job);
    expect(e.data.trace.dev).toMatchObject({ lost: [[1, 1]], end: "silence" });
    // old firmware: no trace fields, no box — a partial trace, no error
    expect(e.data.trace.dev).not.toHaveProperty("lv");
    expect(e.data.trace).not.toHaveProperty("box");
    expect(e.data.trace.relay.voice).toBe("fake");
  });

  it("a late frame fills its gap at once (no wait), and the cloud STT gets the audio in seq order", async () => {
    const w = await setup({ caps: ["say", "listen"] });
    const job = ((await w.room.command(w.token, "listen", {})) as any).result.job;
    await until(() => !!w.dev.lastListen());
    await w.dev.frame(job, { seq: 1 }, 1600, 2);
    await w.dev.frame(job, { seq: 2, end: true, reason: "silence", speech_ms: 400 }, 1600, 3);
    await w.dev.frame(job, { seq: 0 }, 1600, 1);
    const t0 = Date.now();
    const v = (await w.room.job(w.token, job, 3000)) as any;
    expect(Date.now() - t0).toBeLessThan(500);
    expect(v.result.status).toBe("done");
    const pcm = w.voice.sttCalls.at(-1)!;
    expect([pcm[0], pcm[1600], pcm[3200]]).toEqual([1, 2, 3]);
    await w.dev.frame(job, { seq: 3 }); // after the end: dropped
  });
});

describe("client_timing", () => {
  it("validates: an object of up to 8 lower-case keys of ms 0..3600000", () => {
    expect(clientTiming({ received_ms: 120.04, model_ms: 900 })).toEqual({ received_ms: 120, model_ms: 900 });
    for (const bad of [5, "x", [], { Model: 1 }, { model_ms: -1 }, { model_ms: 3600001 }, { model_ms: "9" }, { model_ms: Infinity }, Object.fromEntries(Array.from({ length: 9 }, (_, i) => [`k${i}`, 1]))]) {
      expect(() => clientTiming(bad), JSON.stringify(bad)).toThrow(CommandError);
    }
    expect(validateTalk({ say: "hi", client_timing: { model_ms: 5 } }).client_timing).toEqual({ model_ms: 5 });
    expect(validateAsk({ options: ["yes"], client_timing: { model_ms: 5 } }).client_timing).toEqual({ model_ms: 5 });
    const say = validateCommand("say", { text: "hi", client_timing: { model_ms: 5 } });
    expect(say.client_timing).toEqual({ model_ms: 5 });
    expect(say.args).not.toHaveProperty("client_timing"); // never sent to the device
    expect(() => validateTalk({ say: "hi", client_timing: { "bad key": 1 } })).toThrow(/client_timing/);
  });

  it("a reply to a turn: timings.client, and an ev:\"reply\" trace entry on the turn — seen by its agent, not by another", async () => {
    const w = await setup({ caps: ["say", "listen", "listen_stop", "listen_window", "ack", "shake_talk"] });
    w.voice.ttsSamples = 1600;
    const other = await w.pair("other");
    await w.room.socketMessage(w.sock, JSON.stringify({ t: "evt", name: "shake", data: { peak_g: 2.4, reversals: 5, ms: 900, listen: "shake-1" } }));
    await until(() => !!w.dev.lastListen());
    const turn = w.dev.lastListen().job;
    await w.dev.frame(turn, { seq: 0 });
    await w.dev.frame(turn, { seq: 1, end: true, reason: "silence", speech_ms: 400 }, 0);
    await until(async () => (await w.kv.get("turn")) !== undefined);
    const out = (await w.room.talk(w.token, { say: "fine thanks", listen: false, in_reply_to: turn, client_timing: { received_ms: 40, model_ms: 850 } })) as any;
    expect(out).toMatchObject({ ok: true, result: { status: "done" } });
    const job = out.result.job;
    const rec = await w.kv.get<any>(`job:${job}`);
    expect(rec.result.timings.client).toEqual({ received_ms: 40, model_ms: 850 });
    await flush();
    const reply = (await w.owner({ job: turn })).entries.find((e: any) => e.kind === "trace" && e.ev === "reply");
    expect(reply).toMatchObject({ agent: "talker", job: turn, data: { op: "talk", job, client_timing: { received_ms: 40, model_ms: 850 } } });
    expect(typeof reply.data.reply_ms).toBe("number");
    expect(typeof reply.data.first_audio_ms).toBe("number");
    const mine = ((await w.room.activityLog(w.token, { kind: "trace" })) as any).result.entries;
    expect(mine.some((e: any) => e.ev === "reply")).toBe(true);
    const theirs = ((await w.room.activityLog(other, { kind: "trace" })) as any).result.entries;
    expect(theirs).toHaveLength(0);
    // a say with client_timing but no turn: timings.client only
    const s = (await w.room.command(w.token, "say", { text: "and you?", client_timing: { model_ms: 1 } })) as any;
    const done = (await w.room.job(w.token, s.result.job, 3000)) as any;
    expect(done.result.result.timings.client).toEqual({ model_ms: 1 });
    expect(w.dev.cmds.filter((c) => c.name === "say").every((c) => !("client_timing" in c.args))).toBe(true);
  });
});

describe("webhook delivery in the turn's trace", () => {
  it("each utterance delivery: status, latency, attempt, time after the utterance — only for utterances", async () => {
    let status = 500;
    const w = await setup({ caps: ["say", "listen", "listen_stop", "listen_window", "ack", "shake_talk"], fetch: async () => new Response("x", { status }) });
    expect(((await w.room.webhook(w.token, { action: "set", spec: { url: "https://hooks.example.org/molty" } })) as any).ok).toBe(true);
    await w.room.socketMessage(w.sock, JSON.stringify({ t: "evt", name: "shake", data: { peak_g: 2.4, reversals: 5, ms: 900, listen: "shake-1" } }));
    await until(() => !!w.dev.lastListen());
    const turn = w.dev.lastListen().job;
    await w.dev.frame(turn, { seq: 0 });
    await w.dev.frame(turn, { seq: 1, end: true, reason: "silence", speech_ms: 400 }, 0);
    await until(async () => (await w.kv.get("turn")) !== undefined);
    await flush();
    await w.room.alarm(); // shake + utterance: the utterance fails (500)
    status = 200;
    for (const d of (await w.kv.list<any>("whq:")).entries()) await w.kv.put(d[0], { ...d[1], due: 0 });
    await w.room.alarm(); // the retry delivers
    const hooks = (await w.owner({ job: turn })).entries.filter((e: any) => e.kind === "trace" && e.ev === "webhook");
    expect(hooks).toHaveLength(2);
    expect(hooks[0]).toMatchObject({ agent: "talker", ok: false, data: { status: 500, attempt: 1, error: "http_500" } });
    expect(hooks[1]).toMatchObject({ agent: "talker", ok: true, data: { status: 200, attempt: 2 } });
    expect(hooks[1].data).not.toHaveProperty("error");
    // the agent sees its own deliveries in its activity (not another agent's)
    const own = ((await w.room.activityLog(w.token, { kind: "trace" })) as any).result.entries.filter((e: any) => e.ev === "webhook");
    expect(own).toHaveLength(2);
    for (const h of hooks) {
      expect(typeof h.data.ms).toBe("number");
      expect(typeof h.data.after_ms).toBe("number");
    }
  });
});

describe("keep audio for debugging (owner only)", () => {
  const audioReq = (w: Awaited<ReturnType<typeof setup>>, job: string, headers: Record<string, string>) =>
    handleRest(new Request(`https://relay.test/v1/devices/${DEVICE}/activity/audio/${encodeURIComponent(job)}`, { headers }), w.backend);

  const listen = async (w: Awaited<ReturnType<typeof setup>>, fill = 1000) => {
    const job = ((await w.room.command(w.token, "listen", {})) as any).result.job as string;
    await until(() => w.dev.lastListen()?.job === job);
    await w.dev.frame(job, { seq: 0 }, 4800, fill);
    await w.dev.frame(job, { seq: 1, end: true, reason: "silence", speech_ms: 300 }, 1600, fill);
    await w.room.job(w.token, job, 3000);
    return job;
  };

  it("off by default; on: the listen's PCM as WAV through the owner route only (link code), never with an agent token", async () => {
    const w = await setup({ caps: ["say", "listen"] });
    expect(await w.owner()).toMatchObject({ keep_audio: false, audio: { turns: 5, max_s: 30, max_age_h: 24 } });
    const off = await listen(w);
    expect((await jobEntry(w, off)).data).not.toHaveProperty("audio");
    expect((await audioReq(w, off, { "x-molty-link-code": await w.code() })).status).toBe(404);
    // the console's POST (REST, link code), alone without transcripts
    const set = await handleRest(new Request(`https://relay.test/v1/devices/${DEVICE}/activity`, { method: "POST", headers: { "content-type": "application/json", "x-molty-link-code": await w.code() }, body: JSON.stringify({ keep_audio: true }) }), w.backend);
    expect(((await set.json()) as any).result).toMatchObject({ keep_audio: true, transcripts: true });
    const pre = await handleRest(new Request(`https://relay.test/v1/devices/${DEVICE}/activity/audio/${DEVICE}.j_abcdefghijkl`, { method: "OPTIONS" }), w.backend);
    expect(pre.status).toBe(204);
    expect(pre.headers.get("access-control-allow-headers")?.toLowerCase()).toContain("x-molty-link-code");
    const job = await listen(w, 1234);
    const e = await jobEntry(w, job);
    expect(e.data.audio).toBe(true);
    expect(e.data.trace.relay.audio_kept).toBe(true);
    const r = await audioReq(w, job, { "x-molty-link-code": await w.code() });
    expect(r.status).toBe(200);
    expect(r.headers.get("content-type")).toBe("audio/wav");
    expect(r.headers.get("access-control-allow-origin")).toBe("*");
    const bytes = new Uint8Array(await r.arrayBuffer());
    expect(new TextDecoder().decode(bytes.subarray(0, 4))).toBe("RIFF");
    expect(bytes.byteLength).toBe(44 + 6400 * 2);
    expect(new DataView(bytes.buffer).getInt16(44, true)).toBe(1234);
    // the wrong code, an agent's token (as a bearer or as the code), nothing: 401
    expect((await audioReq(w, job, { "x-molty-link-code": "WRONG234" })).status).toBe(401);
    expect((await audioReq(w, job, { authorization: `Bearer ${w.token}` })).status).toBe(401);
    expect((await audioReq(w, job, { "x-molty-link-code": w.token })).status).toBe(401);
    // a job of another device: not found
    expect((await audioReq(w, "molty-zzzzzzzzzz.j_abcdefghijkl", { "x-molty-link-code": await w.code() })).status).toBe(404);
    // the setting change is in the log
    expect((await w.owner({ kind: "settings" })).entries.some((x: any) => x.ev === "activity" && x.data.keep_audio === true)).toBe(true);
  });

  it("transcripts off: nothing is kept and what was kept is deleted; keep_audio off deletes too", async () => {
    const w = await setup({ caps: ["say", "listen"] });
    await w.room.ownerActivity(await w.code(), { action: "set", keep_audio: true });
    const a = await listen(w);
    expect((await w.kv.list("aud:")).size).toBeGreaterThan(0);
    await w.room.ownerActivity(await w.code(), { action: "set", transcripts: false });
    expect((await w.kv.list("aud:")).size).toBe(0);
    const b = await listen(w);
    expect((await w.kv.list("aud:")).size).toBe(0);
    expect((await jobEntry(w, b)).data).not.toHaveProperty("audio");
    expect((await audioReq(w, a, { "x-molty-link-code": await w.code() })).status).toBe(404);
    await w.room.ownerActivity(await w.code(), { action: "set", transcripts: true });
    await listen(w);
    expect((await w.kv.list("aud:")).size).toBeGreaterThan(0);
    await w.room.ownerActivity(await w.code(), { action: "set", keep_audio: false });
    expect((await w.kv.list("aud:")).size).toBe(0);
    expect(((await w.room.ownerActivity(await w.code(), { action: "set", keep_audio: "yes" })) as any).error).toBe("invalid_args");
    expect(((await w.room.ownerActivity(await w.code(), { action: "set" })) as any).error).toBe("invalid_args");
  });

  it("forgetting the device deletes kept audio", async () => {
    const w = await setup({ caps: ["say", "listen"] });
    await w.room.ownerActivity(await w.code(), { action: "set", keep_audio: true });
    await listen(w);
    expect((await w.kv.list("aud:")).size).toBeGreaterThan(0);
    await w.room.socketMessage(w.sock, JSON.stringify({ t: "forget" }));
    expect((await w.kv.list("aud:")).size).toBe(0);
  });

  it("AudioKeep: newest 5, first 30 s, expiry after 24 h (on read and on the alarm), next expiry", async () => {
    let now = 1_000_000;
    const clock = { now: () => now, setTimeout: () => 0, clearTimeout: () => {} };
    const kv = new MemoryKV();
    const k = new AudioKeep(kv, clock);
    for (let i = 0; i < 6; i++) {
      now += 1000;
      expect(await k.store(`j${i}`, [new Uint8Array(new Int16Array(800).fill(i).buffer)])).toBe(true);
    }
    expect(await k.jobs()).toEqual(["j5", "j4", "j3", "j2", "j1"]);
    expect(await k.get("j0")).toBeUndefined();
    expect(new Int16Array((await k.get("j3"))!.buffer)[0]).toBe(3);
    expect(await k.nextExpiry()).toBe(1_000_000 + 2000 + AUDIO_KEEP.maxAgeMs);
    // 31 s of audio keeps 30 s, in 64 KB pieces
    now += 1000;
    expect(await k.store("long", [new Uint8Array(31 * 32000)])).toBe(true);
    expect((await k.get("long"))!.byteLength).toBe(30 * 32000);
    expect(await k.jobs()).toEqual(["long", "j5", "j4", "j3", "j2"]);
    expect(await k.store("empty", [])).toBe(false);
    now += AUDIO_KEEP.maxAgeMs - 1500; // j2..j4 are 24 h old now, j5 and long not yet
    expect(await k.get("j2")).toBeUndefined(); // gone on read
    expect(await k.jobs()).toEqual(["long", "j5"]);
    now += AUDIO_KEEP.maxAgeMs;
    expect(await k.purge()).toBeUndefined();
    expect((await kv.list("aud:")).size).toBe(0);
    expect(wav(new Uint8Array(4)).byteLength).toBe(48);
  });

  it("the room alarm expires kept audio and is scheduled for it", async () => {
    const alarms: number[] = [];
    let now = 5_000_000;
    const clock = { now: () => now, setTimeout: (f: () => void, ms: number) => setTimeout(f, ms), clearTimeout: (h: unknown) => clearTimeout(h as ReturnType<typeof setTimeout>) };
    const ctx = makeRoom({ clock, scheduler: { setAlarm: async (at) => void alarms.push(at) } });
    await ctx.kv.put("al:cfg", { transcripts: true, keep_audio: true });
    const k = new AudioKeep(ctx.kv, clock);
    await k.store(`${DEVICE}.j_aaaaaaaaaaaa`, [new Uint8Array(3200)]);
    now += AUDIO_KEEP.maxAgeMs + 1;
    await ctx.room.alarm();
    expect((await ctx.kv.list("aud:")).size).toBe(0);
    // a kept listen schedules the alarm at its expiry
    now = 6_000_000;
    await k.store(`${DEVICE}.j_bbbbbbbbbbbb`, [new Uint8Array(3200)]);
    await ctx.room.alarm();
    expect(alarms.at(-1)).toBe(6_000_000 + AUDIO_KEEP.maxAgeMs);
  });
});

describe("review round 1: the trace survives a relay-side end and a failed STT", () => {
  const TR = { wall: 1400, cap: 1060, sent: 3, fail: 0, r0: -70, r1: -71, gain: 6, pk: 500 };

  it("a box's end of turn says where its time went: silence, the model's lag, the settings", async () => {
    const w = await setup({ box: true });
    const talk = w.room.talk(w.token, {});
    await until(() => !!w.dev.lastListen() && !!w.box!.sttId());
    const job = w.dev.lastListen().job;
    const id = w.box!.sttId();
    await w.dev.frame(job, { seq: 0, ct: 0, lv: "a0" }, 4800, 11); // 300 ms of audio reached the relay
    await w.box!.say({ t: "stt.trace", id, trace: { thr: 0.6, hold_s: 1.2, words: [["hi", 0.1, 0.15]], end: { at: 0.25, rule: "hold" } } });
    await w.box!.say({ t: "stt.turn_end", id });
    const r = (await talk) as any;
    expect(r.result.timings.listen.end_of_turn).toEqual({ silence_ms: 100, behind_ms: 50, rule: "hold", thr: 0.6, hold_s: 1.2 });
  });

  it("P1: after the box's turn end the device's end frame (its tr) still lands in the job's trace — header only, never audio; the agent's result does not wait", async () => {
    const w = await setup({ box: true, finalTrace: BOX_TRACE });
    const talk = w.room.talk(w.token, {});
    await until(() => !!w.dev.lastListen() && !!w.box!.sttId());
    const job = w.dev.lastListen().job;
    const id = w.box!.sttId();
    await w.dev.frame(job, { seq: 0, ct: 0, lv: "a0a0a0" }, 4800, 11);
    await w.box!.say({ t: "stt.turn_end", id });
    expect(await talk).toMatchObject({ ok: true, result: { status: "done", reason: "turn_end" } });
    // the job is done; the device's own end comes now (listen_stop reached it)
    await w.dev.frame(job, { seq: 1, end: true, reason: "turn_end", ct: 300, lv: "90", tr: TR }, 1600, 99);
    const e = await jobEntry(w, job);
    expect(e.data.trace.dev).toMatchObject({ wall: 1400, cap: 1060, gain: 6, pk: 500, rssi: [-70, -71], end: "turn_end", lv: "a0a0a090", late: 1 });
    expect(w.box!.audio[id]).toEqual([11]); // the late frame's PCM went nowhere
    expect(w.voice.sttCalls).toHaveLength(0);
  });

  it("P1: a device whose end frame never comes: the entry is written after the bounded wait, without tr", async () => {
    const w = await setup({ box: true, finalTrace: BOX_TRACE });
    const t0 = Date.now();
    const talk = w.room.talk(w.token, {});
    await until(() => !!w.dev.lastListen() && !!w.box!.sttId());
    const job = w.dev.lastListen().job;
    await w.dev.frame(job, { seq: 0, ct: 0, lv: "a0a0a0" });
    await w.box!.say({ t: "stt.turn_end", id: w.box!.sttId() });
    await talk;
    expect(Date.now() - t0).toBeLessThan(1000); // the result did not wait for it
    expect((await w.owner({ job })).entries.find((x: any) => x.kind === "job")).toBeUndefined();
    let e: any;
    await until(async () => !!(e = (await w.owner({ job })).entries.find((x: any) => x.kind === "job")), 4500);
    expect(Date.now() - t0).toBeGreaterThanOrEqual(2900);
    expect(e.data.trace.dev).not.toHaveProperty("wall");
    expect(e.data.trace.box.end.rule).toBe("punct");
  }, 8000);

  it("P1: STT failures keep the trace — the box never sends its final (box mode: stt_timeout), the cloud rejects", async () => {
    const w = await setup({ box: true, noFinal: true, caps: ["say", "listen"] });
    await w.room.voiceSettings(await w.code(), { action: "set", mode: "box" });
    const job = ((await w.room.command(w.token, "listen", {})) as any).result.job;
    await until(() => !!w.dev.lastListen());
    await w.dev.frame(job, { seq: 0, ct: 0, lv: "a0a0a0" });
    await w.dev.frame(job, { seq: 1, end: true, reason: "silence", speech_ms: 300, tr: TR }, 0);
    expect((await w.room.job(w.token, job, 3000)) as any).toMatchObject({ result: { status: "failed", error: "stt_timeout" } });
    const e = await jobEntry(w, job);
    expect(e).toMatchObject({ ok: false, error: "stt_timeout", data: { trace: { dev: { wall: 1400, lv: "a0a0a0", end: "silence" }, relay: { voice: "box" } } } });

    const c = await setup({ caps: ["say", "listen"] });
    c.voice.stt = async () => {
      throw new Error("provider_rejected");
    };
    const job2 = ((await c.room.command(c.token, "listen", {})) as any).result.job;
    await until(() => !!c.dev.lastListen());
    await c.dev.frame(job2, { seq: 0, ct: 0, lv: "b0b0b0" });
    await c.dev.frame(job2, { seq: 1, end: true, reason: "silence", speech_ms: 300 }, 0);
    expect((await c.room.job(c.token, job2, 3000)) as any).toMatchObject({ result: { status: "failed", error: "provider_rejected" } });
    const e2 = await jobEntry(c, job2);
    expect(e2.data.trace.dev).toMatchObject({ lv: "b0b0b0", end: "silence" });
  });

  it("P2: the cap counts UTF-8 bytes (CJK words are 3 bytes a character)", () => {
    const t = new ListenTrace(0, "agent");
    t.box = { words: Array.from({ length: 60 }, (_, i) => ["こんにちは世界ですこんにちは世界です" + i, i, i + 0.5]) } as any;
    const full = t.build();
    expect(JSON.stringify(full).length).toBeLessThan(TRACE_LIMITS.maxBytes);
    expect(new TextEncoder().encode(JSON.stringify(full)).byteLength).toBeGreaterThan(TRACE_LIMITS.maxBytes);
    const c = capTrace(full);
    expect(c.trimmed).toBe(true);
    expect(new TextEncoder().encode(JSON.stringify(c)).byteLength).toBeLessThanOrEqual(TRACE_LIMITS.maxBytes);
  });

  it("P2: ask takes in_reply_to — traced as a reply (with client_timing), never claiming the turn", async () => {
    const w = await setup({ caps: ["say", "listen", "listen_stop", "listen_window", "ack", "shake_talk", "ask"] });
    w.voice.ttsSamples = 1600;
    await w.room.socketMessage(w.sock, JSON.stringify({ t: "evt", name: "shake", data: { peak_g: 2.4, reversals: 5, ms: 900, listen: "shake-1" } }));
    await until(() => !!w.dev.lastListen());
    const turn = w.dev.lastListen().job;
    await w.dev.frame(turn, { seq: 0 });
    await w.dev.frame(turn, { seq: 1, end: true, reason: "silence", speech_ms: 400 }, 0);
    await until(async () => (await w.kv.get("turn")) !== undefined);
    expect(validateAsk({ options: ["yes"], in_reply_to: turn }).in_reply_to).toBe(turn);
    expect(() => validateAsk({ options: ["yes"], in_reply_to: "nope" })).toThrow(/in_reply_to/);
    const out = (await w.room.ask(w.token, { say: "tea?", options: ["yes", "no"], timeout_s: 1, in_reply_to: turn, client_timing: { model_ms: 640 } })) as any;
    expect(out.ok).toBe(true);
    const reply = (await w.owner({ job: turn })).entries.find((e: any) => e.kind === "trace" && e.ev === "reply");
    expect(reply).toMatchObject({ agent: "talker", data: { op: "ask", client_timing: { model_ms: 640 } } });
    expect(typeof reply.data.reply_ms).toBe("number");
    expect(typeof reply.data.first_audio_ms).toBe("number");
    expect(((await w.kv.get<any>("turn")) as any).by).toBeUndefined(); // never claimed
    expect(out.result.timings.client).toEqual({ model_ms: 640 });
  });

  it("P2: a keep-audio store racing a wipe leaves nothing behind", async () => {
    let gate: (() => void) | undefined;
    let held = false;
    class SlowKV extends MemoryKV {
      async put<T>(key: string, value: T) {
        if (key.startsWith("aud:p:") && !held) {
          held = true;
          await new Promise<void>((r) => (gate = r));
        }
        return super.put(key, value);
      }
    }
    const kv = new SlowKV();
    const clock = { now: () => 1000, setTimeout: () => 0, clearTimeout: () => {} };
    const k = new AudioKeep(kv, clock);
    const store = k.store("j", [new Uint8Array(3 * AUDIO_KEEP.pieceBytes)]);
    await until(() => !!gate);
    await k.wipe(); // the owner turned it off mid-store
    gate!();
    expect(await store).toBe(false);
    expect((await kv.list("aud:")).size).toBe(0);
    // a generation read before the wipe (the setting was on then): the store writes nothing
    const g = k.generation;
    await k.wipe();
    expect(await k.store("j2", [new Uint8Array(3200)], g)).toBe(false);
    expect((await kv.list("aud:")).size).toBe(0);
  });
});

describe("review round 3: the trace tail starts where audio acceptance stops; forget voids delayed writes", () => {
  const TR = { wall: 1400, cap: 1060, sent: 2, fail: 0, r0: -70, r1: -71, gain: 6, pk: 500 };
  const jobEntries = async (w: Awaited<ReturnType<typeof setup>>, job: string) => {
    const out: any[] = [];
    for (const c of (await w.kv.list<any>("al:c:")).values()) for (const e of c.entries) if (e.job === job && e.kind === "job") out.push(e);
    return out;
  };

  it("P1: the device's end frame arriving while the box finalises (after stt.end, before stt.final) lands in the trace", async () => {
    const w = await setup({ box: true, noFinal: true });
    const talk = w.room.talk(w.token, {});
    await until(() => !!w.dev.lastListen() && !!w.box!.sttId());
    const job = w.dev.lastListen().job;
    const id = w.box!.sttId();
    await w.dev.frame(job, { seq: 0, ct: 0, lv: "a0a0a0" });
    await w.box!.say({ t: "stt.turn_end", id });
    await until(() => w.box!.control.some((f) => f.t === "stt.end" && f.id === id));
    await w.dev.frame(job, { seq: 1, end: true, reason: "turn_end", ct: 300, lv: "90", tr: TR }, 1600);
    await w.box!.say({ t: "stt.trace", id, trace: BOX_TRACE });
    await w.box!.say({ t: "stt.final", id, text: "hello", ms: { final: 5 } });
    expect(await talk).toMatchObject({ ok: true, result: { heard: "hello" } });
    const e = await jobEntry(w, job);
    expect(e.data.trace.dev).toMatchObject({ wall: 1400, rssi: [-70, -71], lv: "a0a0a090" });
    expect(await jobEntries(w, job)).toHaveLength(1); // exactly once
  });

  it("P1: … and while the listen's audio is being kept (retention write in progress)", async () => {
    let gate: (() => void) | undefined;
    let armed = false;
    class SlowKV extends MemoryKV {
      async put<T>(key: string, value: T) {
        if (armed && key.startsWith("aud:p:")) {
          armed = false;
          await new Promise<void>((r) => (gate = r));
        }
        return super.put(key, value);
      }
    }
    const w = await setup({ box: true, finalTrace: BOX_TRACE, kv: new SlowKV() });
    await w.room.ownerActivity(await w.code(), { action: "set", keep_audio: true });
    armed = true;
    const talk = w.room.talk(w.token, {});
    await until(() => !!w.dev.lastListen() && !!w.box!.sttId());
    const job = w.dev.lastListen().job;
    await w.dev.frame(job, { seq: 0, ct: 0, lv: "a0a0a0" });
    await w.box!.say({ t: "stt.turn_end", id: w.box!.sttId() });
    await until(() => !!gate);
    await w.dev.frame(job, { seq: 1, end: true, reason: "turn_end", ct: 300, lv: "90", tr: TR }, 1600);
    gate!();
    await talk;
    const e = await jobEntry(w, job);
    expect(e.data).toMatchObject({ audio: true, trace: { dev: { wall: 1400 } } });
    expect(await jobEntries(w, job)).toHaveLength(1);
  });

  it("P1: the device disconnecting ends the tail at once: the entry is written without tr", async () => {
    const w = await setup({ box: true, finalTrace: BOX_TRACE });
    const talk = w.room.talk(w.token, {});
    await until(() => !!w.dev.lastListen() && !!w.box!.sttId());
    const job = w.dev.lastListen().job;
    await w.dev.frame(job, { seq: 0, ct: 0, lv: "a0a0a0" });
    await w.box!.say({ t: "stt.turn_end", id: w.box!.sttId() });
    await talk;
    const t0 = Date.now();
    w.sock.close(1006, "gone");
    const e = await jobEntry(w, job);
    expect(Date.now() - t0).toBeLessThan(1000);
    expect(e.data.trace.dev).not.toHaveProperty("wall");
  });

  it("P1: forget during the tail: after 3.1 s no activity entry, no audio, no job comes back", async () => {
    const w = await setup({ box: true, finalTrace: BOX_TRACE });
    await w.room.ownerActivity(await w.code(), { action: "set", keep_audio: true });
    const talk = w.room.talk(w.token, {});
    await until(() => !!w.dev.lastListen() && !!w.box!.sttId());
    const job = w.dev.lastListen().job;
    await w.dev.frame(job, { seq: 0, ct: 0, lv: "a0a0a0" });
    await w.box!.say({ t: "stt.turn_end", id: w.box!.sttId() });
    await talk;
    expect(await jobEntries(w, job)).toHaveLength(0); // the tail is still open
    await w.room.socketMessage(w.sock, JSON.stringify({ t: "forget" }));
    await new Promise((r) => setTimeout(r, 3100));
    await flush();
    expect(await jobEntries(w, job)).toHaveLength(0);
    expect((await w.kv.list("aud:")).size).toBe(0);
    expect(await w.kv.get(`job:${job}`)).toBeUndefined();
  }, 8000);

  it("P1: forget while the listen's audio is being written: nothing of it stays", async () => {
    let gate: (() => void) | undefined;
    let armed = false;
    class SlowKV extends MemoryKV {
      async put<T>(key: string, value: T) {
        if (armed && key.startsWith("aud:p:")) {
          armed = false;
          await new Promise<void>((r) => (gate = r));
        }
        return super.put(key, value);
      }
    }
    const w = await setup({ caps: ["say", "listen"], kv: new SlowKV() });
    await w.room.ownerActivity(await w.code(), { action: "set", keep_audio: true });
    armed = true;
    const job = ((await w.room.command(w.token, "listen", {})) as any).result.job;
    await until(() => !!w.dev.lastListen());
    await w.dev.frame(job, { seq: 0 }, 4800);
    await w.dev.frame(job, { seq: 1, end: true, reason: "silence", speech_ms: 300 }, 0);
    await until(() => !!gate);
    await w.room.socketMessage(w.sock, JSON.stringify({ t: "forget" }));
    gate!();
    await new Promise((r) => setTimeout(r, 100));
    await flush();
    expect((await w.kv.list("aud:")).size).toBe(0);
    expect(await jobEntries(w, job)).toHaveLength(0);
    expect(await w.kv.get(`job:${job}`)).toBeUndefined();
  });
  it("P1: an activity write queued before forget never lands after the wipe (the log's own queue)", async () => {
    let gate: (() => void) | undefined;
    let armed = true;
    class SlowKV extends MemoryKV {
      async get<T>(key: string) {
        if (armed && key === "al:cfg") {
          armed = false;
          await new Promise<void>((r) => (gate = r));
        }
        return super.get<T>(key);
      }
    }
    const kv = new SlowKV();
    const log = new ActivityLog({ deviceId: DEVICE, kv, clock: { now: () => Date.now(), setTimeout: (f, ms) => setTimeout(f, ms), clearTimeout: (h) => clearTimeout(h as ReturnType<typeof setTimeout>) } });
    const first = log.record({ ts: Date.now(), kind: "job", job: `${DEVICE}.j_a`, op: "listen", ok: true, data: { heard: "secret words" } }); // running: held inside append
    const queued = log.record({ ts: Date.now(), kind: "job", job: `${DEVICE}.j_b`, op: "listen", ok: true, data: { heard: "more words" } }); // queued behind it
    await until(() => !!gate);
    const settled = log.settle().then(async () => {
      for (const key of (await kv.list("al:")).keys()) await kv.delete(key); // the room's prefix delete
    });
    gate!();
    await Promise.all([first, queued, settled]);
    expect((await kv.list("al:")).size).toBe(0);
    log.record({ ts: Date.now(), kind: "settings", ev: "activity", data: {} }); // a write after forget still works
    await log.settle();
    expect((await log.query({}, { owner: true })).entries).toHaveLength(1);
  });

  it("P1: forget while the listen's job entry is being written: the wipe waits for it, nothing comes back", async () => {
    let gate: (() => void) | undefined;
    let armed = false;
    class SlowKV extends MemoryKV {
      async put<T>(key: string, value: T) {
        if (armed && key.startsWith("al:c:") && JSON.stringify(value).includes('"kind":"job"')) {
          armed = false;
          await new Promise<void>((r) => (gate = r));
        }
        return super.put(key, value);
      }
    }
    const w = await setup({ caps: ["say", "listen"], kv: new SlowKV() });
    armed = true;
    const job = ((await w.room.command(w.token, "listen", {})) as any).result.job;
    await until(() => !!w.dev.lastListen());
    await w.dev.frame(job, { seq: 0 }, 4800);
    await w.dev.frame(job, { seq: 1, end: true, reason: "silence", speech_ms: 300 }, 0);
    await until(() => !!gate);
    const forgot = w.room.socketMessage(w.sock, JSON.stringify({ t: "forget" }));
    await new Promise((r) => setTimeout(r, 20));
    gate!();
    await forgot;
    await new Promise((r) => setTimeout(r, 100));
    await flush();
    const jobs = [...(await w.kv.list("al:c:")).values()].flatMap((c: any) => c.entries).filter((e: any) => e.job === job);
    expect(jobs).toHaveLength(0);
    expect(await w.kv.get(`job:${job}`)).toBeUndefined();
  });
});
