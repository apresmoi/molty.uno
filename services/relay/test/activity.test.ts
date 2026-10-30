// Activity log (core/activity.ts): one timeline per Molty — agent calls, job ends, device
// events, pairing, voice box, settings and the device's own log lines — with secrets
// stripped, transcripts redacted at write time when the owner says so, bounded retention,
// and agent isolation (an agent reads only its own entries).
import { describe, expect, it } from "vitest";
import { ACTIVITY_LIMITS, ActivityLog, lineLevel, parseActivityQuery, summarizeArgs } from "../src/core/activity.js";
import type { RelayBackend } from "../src/core/api.js";
import { encodeAudioFrame } from "../src/core/protocol.js";
import type { DeviceRoom } from "../src/core/room.js";
import { type Clock, MemoryKV } from "../src/core/store.js";
import { handleMcp } from "../src/surfaces/mcp.js";
import { handleRest } from "../src/surfaces/rest.js";
import { DEVICE, type FakeSocket, KEY, LINK, connect, flush, makeRoom } from "./helpers.js";

const FAST = { cmdTimeoutMs: 300, pairWindowMs: 400, sayChunkSamples: 1600, sayLeadFrames: 2, listenGraceMs: 200, sayDoneGraceMs: 300 };

/** A device that acks cmds, plays say streams (say.done) and streams a short mic clip for listen. */
function device(room: DeviceRoom, sock: FakeSocket, opts: { cmdError?: (name: string) => string | undefined } = {}) {
  const text = sock.sendText.bind(sock);
  const bin = sock.sendBinary.bind(sock);
  let code = LINK;
  const codes = ["NEWC2345", "NEXT2345", "LAST2345"];
  sock.sendText = (t: string) => {
    text(t);
    const f = JSON.parse(t);
    if (f.t === "cmd") {
      const err = opts.cmdError?.(f.name);
      setTimeout(() => void room.socketMessage(sock, JSON.stringify(err ? { t: "res", id: f.id, ok: false, error: err } : { t: "res", id: f.id, ok: true, result: f.name === "status" ? { rssi: -61, fw: "test-fw" } : {} })), 1);
      if (f.name === "listen" && !err)
        setTimeout(() => {
          void room.socketMessage(sock, encodeAudioFrame({ s: f.args.job, seq: 0, kind: "pcm16", rate: 16000, end: false }, new Uint8Array(new Int16Array(3200).fill(8000).buffer)));
          void room.socketMessage(sock, new TextEncoder().encode(JSON.stringify({ s: f.args.job, seq: 1, kind: "pcm16", rate: 16000, end: true, reason: "silence", speech_ms: 180 })));
        }, 10);
    }
    if (f.t === "pair.request") {
      code = codes.shift() ?? code;
      setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "pair.result", req: f.req, ok: true, link_code: code })), 2);
    }
  };
  sock.sendBinary = (b: Uint8Array) => {
    bin(b);
    const nl = b.indexOf(10);
    const head = JSON.parse(new TextDecoder().decode(nl < 0 ? b : b.subarray(0, nl)));
    if (head.end) setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "evt", name: "say.done", data: { job: head.s } })), 3);
  };
  return { code: () => code };
}

async function world() {
  const ctx = makeRoom({ timings: FAST });
  ctx.voice.sttText = "remind me about the dentist";
  const sock = await connect(ctx.room, ctx.host);
  const dev = device(ctx.room, sock);
  const pair = async (name: string) => {
    const out = await ctx.room.pair({ device_id: DEVICE, link_code: dev.code(), agent_name: name });
    if (!out.ok || out.result.status !== "approved") throw new Error(JSON.stringify(out));
    return out.result.agent_token;
  };
  const owner = (query: Record<string, unknown> = {}) => ctx.room.ownerActivity(dev.code(), { action: "get", query });
  const entries = async (query: Record<string, unknown> = {}) => {
    const r = await owner(query);
    if (!r.ok) throw new Error(JSON.stringify(r));
    return r.result.entries as any[];
  };
  const raw = () => JSON.stringify([...ctx.kv.data.entries()].filter(([k]) => k.startsWith("al:")));
  return { ...ctx, sock, dev, pair, owner, entries, raw, backend: { device: () => ctx.room } as RelayBackend };
}

/** A device `log` frame on its socket. */
const logFrame = (room: DeviceRoom, sock: FakeSocket, frame: Record<string, unknown>) => room.socketMessage(sock, JSON.stringify({ t: "log", ...frame }));
const tagOf = (job: string) => job.split(".j_")[1];

describe("activity: an entry for each kind", () => {
  it("calls (op, args, outcome, ms), job ends (timings, voice, reason, heard/said), status rssi, refusals", async () => {
    const w = await world();
    const a = await w.pair("Claude");
    const talk = await w.room.talk(a, { say: "Hello there.", listen: true });
    expect(talk.ok).toBe(true);
    const job = (talk as any).result.job as string;
    await w.room.command(a, "status", {});
    await w.room.command(a, "mood", { mood: "nope" }); // invalid_args
    const all = await w.entries();
    const call = all.find((e) => e.kind === "call" && e.op === "talk");
    expect(call).toMatchObject({ agent: "Claude", job, ok: true, args: { say: "Hello there.", listen: true }, data: { status: "done" } });
    expect(call.ms).toBeGreaterThanOrEqual(0);
    expect(call.who).toBeUndefined(); // the isolation key never leaves the relay
    const end = all.find((e) => e.kind === "job" && e.job === job);
    expect(end).toMatchObject({ op: "talk", agent: "Claude", ok: true, data: { status: "done", reason: "silence", heard: "remind me about the dentist", said: "Hello there.", voice: { tts: "fake", stt: "fake" } } });
    expect(end.data.timings).toMatchObject({ say: expect.any(Object), listen: expect.any(Object), total: expect.any(Number) });
    expect(all.find((e) => e.op === "status")).toMatchObject({ ok: true, data: { rssi: -61 } });
    expect(all.find((e) => e.op === "mood")).toMatchObject({ ok: false, error: "invalid_args" });
    expect(all.find((e) => e.kind === "pair")).toMatchObject({ ev: "approved", agent: "Claude" });
    expect(all.find((e) => e.kind === "device" && e.ev === "connect")).toMatchObject({ data: { fw: "test-fw" } });
  });

  it("a failed talk is a failed job + a failed call carrying the job id (also in the error's details)", async () => {
    const ctx = makeRoom({ timings: FAST });
    const sock = await connect(ctx.room, ctx.host);
    device(ctx.room, sock, { cmdError: (n) => (n === "listen" ? "mic_busy" : undefined) });
    const out = await ctx.room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "Grokbot" });
    const tok = (out as any).result.agent_token;
    const r = await ctx.room.talk(tok, { listen: true });
    expect(r).toMatchObject({ ok: false, error: "mic_busy", details: { job: expect.stringMatching(/\.j_/) } });
    const job = (r as any).details.job;
    const page = await ctx.room.ownerActivity("NEWC2345", { action: "get", query: { job } });
    const es = (page as any).result.entries;
    expect(es.map((e: any) => [e.kind, e.ok, e.error])).toEqual([
      ["call", false, "mic_busy"],
      ["job", false, "mic_busy"],
    ]);
    expect((await ctx.room.ownerActivity("NEWC2345", { action: "get", query: { errors: "1" } }) as any).result.entries.every((e: any) => e.ok === false)).toBe(true);
  });

  it("device events: gestures, a replaced connection's disconnect with its close code, a refused key, a dropped link", async () => {
    const w = await world();
    await w.room.socketMessage(w.sock, JSON.stringify({ t: "evt", name: "shake", data: { peak_g: 2.4, reversals: 5, ms: 900 } }));
    await w.room.socketMessage(w.sock, JSON.stringify({ t: "evt", name: "button", data: { id: "accept" } }));
    await w.room.socketMessage(w.sock, JSON.stringify({ t: "evt", name: "say.done", data: { job: "x" } })); // internal: not logged
    const bad = w.host.open();
    w.room.socketOpen(bad);
    await w.room.socketMessage(bad, JSON.stringify({ t: "hello", id: DEVICE, key: "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=" }));
    const second = await connect(w.room, w.host); // replaces the first: it closes 4409
    second.close(1006, "gone");
    await flush();
    const dev = (await w.entries({ kind: "device" })).map((e) => [e.ev, e.data]);
    expect(dev).toEqual([
      ["connect", { fw: "test-fw", board: null }],
      ["shake", { peak_g: 2.4, reversals: 5, ms: 900 }],
      ["button", { id: "accept" }],
      ["refused", { code: 4401, reason: "device key mismatch" }],
      ["disconnect", { code: 4409 }],
      ["connect", { fw: "test-fw", board: null }],
      ["disconnect", { code: 1006 }],
    ]);
  });

  it("refused hellos are capped hard (unauthenticated: anyone with the id could flood the log)", async () => {
    const w = await world();
    for (let i = 0; i < 10; i++) {
      const bad = w.host.open();
      w.room.socketOpen(bad);
      await w.room.socketMessage(bad, JSON.stringify({ t: "hello", id: DEVICE, key: "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=" }));
    }
    expect((await w.entries({ kind: "device" })).filter((e) => e.ev === "refused")).toHaveLength(ACTIVITY_LIMITS.refusedPerMinute);
  });

  it("pairing changes: approved, revoked; voice box unpair and settings changes by the owner", async () => {
    const w = await world();
    const a = await w.pair("Claude");
    await w.room.revoke(a);
    await w.room.voiceSettings(w.dev.code(), { action: "set", mode: "cloud" });
    await w.room.ownerActivity(w.dev.code(), { action: "set", transcripts: false });
    const es = await w.entries();
    expect(es.filter((e) => e.kind === "pair").map((e) => [e.ev, e.agent])).toEqual([
      ["approved", "Claude"],
      ["revoked", "Claude"],
    ]);
    expect(es.filter((e) => e.kind === "settings").map((e) => [e.ev, e.data])).toEqual([
      ["voice", { mode: "cloud", voice: null }],
      ["activity", { transcripts: false }],
    ]);
  });

  it("device log lines: job tag -> job id, uptime -> wall time (current boot: up; previous boot: sync, else approx), level, dedupe by boot+seq", async () => {
    const w = await world();
    const a = await w.pair("Claude");
    const say = await w.room.command(a, "say", { text: "hi" });
    const job = (say as any).result.job as string;
    const t0 = Date.now();
    await logFrame(w.room, w.sock, { boot: 7, up: 100000, lines: [[10, 99000, tagOf(job), "EVT audio.say start ms=500"], [11, 99500, "", "E (99500) agent_link: boom"], [12, 99800, "", "\u001b[0;33mW (99800) wifi: weak"]] });
    await logFrame(w.room, w.sock, { boot: 7, up: 100200, lines: [[11, 99500, "", "E (99500) agent_link: boom"], [13, 100100, "", "I (100100) x: y"]] }); // 11 again: skipped
    await logFrame(w.room, w.sock, { boot: 6, prev: true, sync: [5000, t0 - 60000], lines: [[3, 6000, "", "EVT boot reset=poweron"]] });
    await logFrame(w.room, w.sock, { boot: 5, prev: true, lines: [[1, 10, "", "EVT link.disconnect why=wifi"]] });
    await logFrame(w.room, w.sock, { boot: "x", up: 1, lines: [[1, 1, "", "nope"]] }); // malformed: dropped
    await logFrame(w.room, w.sock, { boot: 7, up: 1, lines: [[20, 1, "bad tag!", "x"], [21, 1, "", 5]] }); // bad lines: dropped
    const lines = await w.entries({ kind: "device_log" });
    const by = (seq: number, boot = 7) => lines.find((e) => e.data.seq === seq && e.data.boot === boot);
    expect(lines).toHaveLength(6);
    expect(by(10)).toMatchObject({ job, data: { level: "EVT", text: "EVT audio.say start ms=500" } });
    expect(Math.abs(by(10).ts - (t0 - 1000))).toBeLessThan(200); // received now, 1000 ms before `up`
    expect(by(11)).toMatchObject({ ok: false, data: { level: "E" } });
    expect(by(12).data.level).toBe("W");
    expect(by(13).data.level).toBe("I");
    expect(by(3, 6)).toMatchObject({ ts: t0 - 59000, data: { prev: true } });
    expect(by(12).data.text).toBe("W (99800) wifi: weak"); // colour escape gone
    expect(by(1, 5).data).toMatchObject({ prev: true, approx: true });
    // owner's job filter: the say's call, its job end and its device line
    await w.room.job(a, job, 5000);
    expect((await w.entries({ job })).map((e) => e.kind).sort()).toEqual(["call", "device_log", "job"]); // the wait poll succeeded: not logged
  });
});

describe("activity: secrets never reach storage", () => {
  it("webhook headers, body and full URL; token-shaped values; link codes", async () => {
    const w = await world();
    const a = await w.pair("Claude");
    await w.room.webhook(a, { action: "set", spec: { url: "https://hooks.example.com/molty?key=SUPERSECRETQUERY", headers: { authorization: "Bearer sk-live-SUPERSECRETHEADER" }, body: '{"k":"SUPERSECRETBODY"}' } });
    await w.room.command(a, "state", { state: { note: a, api_key: "SUPERSECRETKEY", nested: { password: "SUPERSECRETPW" } } });
    const stored = w.raw();
    for (const s of ["SUPERSECRETQUERY", "SUPERSECRETHEADER", "SUPERSECRETBODY", "SUPERSECRETKEY", "SUPERSECRETPW", a, a.split(".")[1], LINK]) expect(stored).not.toContain(s);
    const es = await w.entries({ kind: "call" });
    expect(es.find((e) => e.op === "webhook").args).toEqual({ action: "set", url: "https://hooks.example.com/…", headers: 1, custom_body: true });
    expect(es.find((e) => e.op === "state").args).toEqual({ state: { note: "[secret]", api_key: "[secret]", nested: { password: "[secret]" } } });
  });

  it("summarizeArgs: secret keys and bearer-shaped values anywhere, strings clipped, depth bounded", () => {
    const tok = `${DEVICE}.${"A".repeat(43)}`;
    expect(summarizeArgs("x", { token: "t", link_code: "ABCD2345", code: "1234", list: [tok, "ok"], deep: { a: { b: { c: 1 } } }, long: "y ".repeat(250) })).toEqual({
      token: "[secret]",
      link_code: "[secret]",
      code: "[secret]",
      list: ["[secret]", "ok"],
      deep: { a: { b: "[…]" } },
      long: `${"y ".repeat(60).slice(0, 119)}…`,
    });
  });
});

describe("activity: log transcripts setting (redacted at write time)", () => {
  it("off: heard / said / say text never stored, entries marked redacted; on again: stored", async () => {
    const w = await world();
    const a = await w.pair("Claude");
    const off = await w.room.ownerActivity(w.dev.code(), { action: "set", transcripts: false });
    expect(off).toMatchObject({ ok: true, result: { transcripts: false } });
    await w.room.talk(a, { say: "Your PIN is forty two.", listen: true });
    await w.room.command(a, "show", { text: "classified memo" });
    const stored = w.raw();
    for (const s of ["forty two", "dentist", "classified memo"]) expect(stored).not.toContain(s);
    const es = await w.entries();
    const job = es.find((e) => e.kind === "job");
    expect(job).toMatchObject({ redacted: true, data: { heard: "", said: "" } });
    expect(es.find((e) => e.kind === "call" && e.op === "talk")).toMatchObject({ redacted: true, args: { say: "" } });
    expect(es.find((e) => e.op === "show")).toMatchObject({ redacted: true, args: { text: "" } });
    // metadata stays: timings, voice, reason
    expect(job.data).toMatchObject({ reason: "silence", voice: { tts: "fake", stt: "fake" }, timings: expect.any(Object) });
    await w.room.ownerActivity(w.dev.code(), { action: "set", transcripts: true });
    await w.room.talk(a, { say: "Back on.", listen: true });
    expect(w.raw()).toContain("Back on.");
    expect(w.raw()).toContain("dentist");
  });

  it("only the owner (current link code) reads or changes it", async () => {
    const w = await world();
    expect(await w.room.ownerActivity("ZZZZ2345", { action: "set", transcripts: false })).toMatchObject({ ok: false, error: "unauthorized" });
    expect(await w.room.ownerActivity(w.dev.code(), { action: "set", transcripts: "no" })).toMatchObject({ ok: false, error: "invalid_args" });
    expect((await w.owner()).ok && (await w.owner() as any).result.transcripts).toBe(true);
  });
});

describe("activity: agent isolation", () => {
  async function two() {
    const w = await world();
    const a = await w.pair("Claude");
    const b = await w.pair("Grokbot");
    w.voice.sttText = "alpha secret words";
    const ta = await w.room.talk(a, { say: "Claude speaking.", listen: true });
    w.voice.sttText = "bravo private words";
    const tb = await w.room.talk(b, { say: "Grokbot speaking.", listen: true });
    const ja = (ta as any).result.job;
    const jb = (tb as any).result.job;
    await logFrame(w.room, w.sock, { boot: 1, up: 50000, lines: [[1, 49000, tagOf(ja), "EVT audio.listen end A"], [2, 49500, tagOf(jb), "EVT audio.listen end B"], [3, 49600, "", "EVT TL! untagged"]] });
    return { w, a, b, ja, jb };
  }

  it("agent A reads only its own calls, jobs and device lines — never B's transcripts", async () => {
    const { w, a, ja, jb } = await two();
    const r = await w.room.activityLog(a, {});
    expect(r.ok).toBe(true);
    const es = (r as any).result.entries as any[];
    const body = JSON.stringify(r);
    expect(body).not.toContain("Grokbot");
    expect(body).not.toContain("bravo private words");
    expect(body).not.toContain(jb);
    expect(body).not.toContain("untagged");
    expect(es.every((e) => e.who === undefined)).toBe(true);
    expect(es.filter((e) => e.kind !== "device").every((e) => e.agent === "Claude" || (e.kind === "device_log" && e.job === ja))).toBe(true);
    expect(es.find((e) => e.kind === "job")).toMatchObject({ job: ja, data: { heard: "alpha secret words" } });
    expect(es.find((e) => e.kind === "device_log")).toMatchObject({ job: ja, data: { text: "EVT audio.listen end A" } });
    expect(es.find((e) => e.kind === "pair")).toMatchObject({ ev: "approved", agent: "Claude" });
  });

  it("A asking for B's job, or for agent=Grokbot, still gets nothing of B's; the owner sees both", async () => {
    const { w, a, jb } = await two();
    expect(((await w.room.activityLog(a, { job: jb })) as any).result.entries).toEqual([]);
    const named = (await w.room.activityLog(a, { agent: "Grokbot" })) as any;
    expect(JSON.stringify(named)).not.toContain("Grokbot");
    const owner = await w.entries();
    expect(new Set(owner.filter((e) => e.agent).map((e) => e.agent))).toEqual(new Set(["Claude", "Grokbot"]));
    expect((await w.entries({ agent: "Grokbot" })).every((e) => e.agent === "Grokbot")).toBe(true);
  });

  it("device connects/disconnects show up for an agent only around its own jobs", async () => {
    const { w, a } = await two();
    const idle = await w.pair("Idle"); // no jobs: no device events at all
    w.sock.close(1006, "gone");
    await flush();
    // the disconnect right after A's job is inside its span
    expect(((await w.room.activityLog(a, { kind: "device" })) as any).result.entries.map((e: any) => [e.ev, e.data])).toContainEqual(["disconnect", { code: 1006 }]);
    expect(((await w.room.activityLog(idle, {})) as any).result.entries.map((e: any) => e.kind)).toEqual(["pair"]);
  });

  it("a voice box token cannot read activity", async () => {
    const w = await world();
    await w.pair("Claude");
    const out = await w.room.pair({ device_id: DEVICE, link_code: w.dev.code(), agent_name: "box", role: "voice" });
    const vt = (out as any).result.agent_token;
    expect(await w.room.activityLog(vt, {})).toMatchObject({ ok: false, error: "forbidden" });
    expect(await w.room.activityLog("nope", {})).toMatchObject({ ok: false, error: "unauthorized" });
  });
});

describe("activity: review findings (merge3)", () => {
  it("P0-1: naming another agent's job (an unknown 'wait' command with args.job) grants nothing; ops are only known command names", async () => {
    const w = await world();
    const victim = await w.pair("Claude");
    const attacker = await w.pair("Mallory");
    const say = (await w.room.command(victim, "say", { text: "hi" })) as any;
    const job = say.result.job as string;
    await w.room.job(victim, job, 5000);
    await logFrame(w.room, w.sock, { boot: 1, up: 50000, lines: [[1, 49000, tagOf(job), "EVT victim line"]] });
    const before = JSON.stringify(await w.room.activityLog(attacker, {}));
    expect(before).not.toContain("victim line");
    expect(await w.room.command(attacker, "wait", { job })).toMatchObject({ ok: false, error: "unknown_command" });
    await w.room.job(attacker, job, 0); // a molty_wait on it: refused or not, it is not the attacker's job
    const after = (await w.room.activityLog(attacker, {})) as any;
    expect(JSON.stringify(after)).not.toContain("victim line");
    expect(after.result.entries.filter((e: any) => e.kind === "device_log")).toEqual([]);
    expect(after.result.entries.filter((e: any) => e.kind === "call").map((e: any) => e.op)).toContain("unknown_command");
    expect(after.result.entries.some((e: any) => e.op === "wait" && e.job === job)).toBe(false);
    // even a call entry that DID carry the job and the attacker's identity grants nothing: only job records do
    const attackerWho = (await import("../src/core/crypto.js")).sha256Hex;
    const who = (await attackerWho(attacker)).slice(0, 16);
    await w.room.activity.record({ ts: Date.now(), kind: "call", op: "say", agent: "Mallory", who, job, ok: true });
    expect(JSON.stringify(await w.room.activityLog(attacker, {}))).not.toContain("victim line");
    // the victim still sees its own line
    expect(JSON.stringify(await w.room.activityLog(victim, {}))).toContain("victim line");
  });

  it("P0-1: audio_busy does not disclose the running job's id to another agent", async () => {
    const w = await world();
    const a = await w.pair("Claude");
    const b = await w.pair("Mallory");
    const say = (await w.room.command(a, "say", { text: "a long enough sentence to keep the speaker busy" })) as any;
    const busy = await w.room.command(b, "say", { text: "me" });
    expect(busy).toMatchObject({ ok: false, error: "audio_busy" });
    expect(JSON.stringify(busy)).not.toContain(say.result.job);
    await w.room.job(a, say.result.job, 5000);
  });

  it("P0-2: job results go through the same secret sanitizer as calls (one write path)", async () => {
    const w = await world();
    const a = await w.pair("Claude");
    const tok = `${DEVICE}.${"Q".repeat(43)}`;
    const r = (await w.room.command(a, "say", { text: `your key is ${tok} ok` })) as any;
    await w.room.job(a, r.result.job, 5000);
    const r2 = (await w.room.command(a, "say", { text: tok })) as any;
    await w.room.job(a, r2.result.job, 5000);
    expect(w.raw()).not.toContain("Q".repeat(43));
    const jobs = (await w.entries({ kind: "job" })).map((e) => e.data.said);
    expect(jobs).toEqual(["your key is [secret] ok", "[secret]"]);
    // and directly: any kind, any field
    await w.room.activity.record({ ts: Date.now(), kind: "settings", ev: "x", data: { note: `Bearer ${"Z".repeat(30)}`, nested: { api_key: "plain" }, code: 4401 } });
    expect((await w.entries({ kind: "settings" })).at(-1).data).toEqual({ note: "[secret]", nested: { api_key: "[secret]" }, code: 4401 });
  });
});

describe("activity: bounded retention", () => {
  function fakeClock(start = 1_800_000_000_000) {
    let now = start;
    const clock: Clock = { now: () => now, setTimeout: () => 0, clearTimeout: () => {} };
    return { clock, advance: (ms: number) => (now += ms) };
  }

  it("never more than maxChunks chunks of <= chunkEntries / chunkBytes; the oldest go first", async () => {
    const kv = new MemoryKV();
    const { clock } = fakeClock();
    const log = new ActivityLog({ deviceId: DEVICE, kv, clock });
    const total = Math.ceil((ACTIVITY_LIMITS.maxChunks * ACTIVITY_LIMITS.chunkEntries + 500) / 50) * 50;
    for (let i = 0; i < total; i += 50) await log.record(Array.from({ length: 50 }, (_, k) => ({ ts: clock.now(), kind: "device" as const, ev: "tap", data: { i: i + k } })));
    const chunks = [...(await kv.list<any>("al:c:")).values()];
    expect(chunks.length).toBeLessThanOrEqual(ACTIVITY_LIMITS.maxChunks);
    for (const c of chunks) {
      expect(c.entries.length).toBeLessThanOrEqual(ACTIVITY_LIMITS.chunkEntries);
      expect(JSON.stringify(c.entries).length).toBeLessThanOrEqual(ACTIVITY_LIMITS.chunkBytes + 64);
    }
    const page = await log.query({ limit: 500 }, { owner: true });
    expect((page.entries.at(-1) as any).data.i).toBe(total - 1); // newest kept
    const kept = chunks.reduce((n, c) => n + c.entries.length, 0);
    expect(kept).toBeLessThanOrEqual(ACTIVITY_LIMITS.maxChunks * ACTIVITY_LIMITS.chunkEntries);
    expect(kept).toBeGreaterThan((ACTIVITY_LIMITS.maxChunks - 1) * ACTIVITY_LIMITS.chunkEntries);
    // big entries: chunks roll by bytes
    const kv2 = new MemoryKV();
    const log2 = new ActivityLog({ deviceId: DEVICE, kv: kv2, clock });
    for (let i = 0; i < 40; i++) await log2.record({ ts: clock.now(), kind: "device_log", data: { text: "x".repeat(1500) } });
    for (const c of (await kv2.list<any>("al:c:")).values()) expect(c.bytes).toBeLessThanOrEqual(ACTIVITY_LIMITS.chunkBytes);
  });

  it("24 h: older entries are never returned and are trimmed as the log moves on", async () => {
    const kv = new MemoryKV();
    const { clock, advance } = fakeClock();
    const log = new ActivityLog({ deviceId: DEVICE, kv, clock });
    for (let i = 0; i < 3; i++) await log.record(Array.from({ length: ACTIVITY_LIMITS.chunkEntries }, () => ({ ts: clock.now(), kind: "device" as const, ev: "old" })));
    advance(ACTIVITY_LIMITS.maxAgeMs + 1000);
    expect((await log.query({}, { owner: true })).entries).toEqual([]);
    for (let i = 0; i < 2; i++) await log.record(Array.from({ length: ACTIVITY_LIMITS.chunkEntries }, () => ({ ts: clock.now(), kind: "device" as const, ev: "new" })));
    const stored = [...(await kv.list<any>("al:c:")).values()].flatMap((c) => c.entries);
    expect(stored.some((e) => e.ev === "old")).toBe(false);
    expect((await log.query({ limit: 500 }, { owner: true })).entries.every((e: any) => e.ev === "new")).toBe(true);
  });

  it("since=a<id> pages forward oldest-first; no since = the newest page", async () => {
    const kv = new MemoryKV();
    const { clock, advance } = fakeClock();
    const log = new ActivityLog({ deviceId: DEVICE, kv, clock });
    for (let i = 0; i < 10; i++) {
      await log.record({ ts: clock.now(), kind: "device", ev: "tap", data: { i } });
      advance(10);
    }
    const newest = await log.query({ limit: 3 }, { owner: true });
    expect(newest.entries.map((e: any) => e.data.i)).toEqual([7, 8, 9]);
    expect(newest).toMatchObject({ next: "a10", more: true });
    const fwd = await log.query({ since: "a2", limit: 3 }, { owner: true });
    expect(fwd.entries.map((e: any) => e.data.i)).toEqual([2, 3, 4]);
    expect(fwd).toMatchObject({ next: "a5", more: true });
    const rest = await log.query({ since: "a5", limit: 50 }, { owner: true });
    expect(rest).toMatchObject({ next: "a10", more: false });
    expect((await log.query({ since: "a10" }, { owner: true })).entries).toEqual([]);
  });

  it("forgetting the device wipes the log", async () => {
    const w = await world();
    await w.pair("Claude");
    expect(w.raw()).not.toBe("[]");
    await w.room.socketMessage(w.sock, JSON.stringify({ t: "forget" }));
    expect([...w.kv.data.keys()].filter((k) => k.startsWith("al:"))).toEqual([]);
  });
});

describe("activity: surfaces", () => {
  it("REST owner route: link code header, CORS, query params, POST transcripts; agent route GET /v1/activity", async () => {
    const w = await world();
    const a = await w.pair("Claude");
    await w.room.command(a, "status", {});
    const base = `https://r/v1/devices/${DEVICE}/activity`;
    const opt = await handleRest(new Request(base, { method: "OPTIONS" }), w.backend);
    expect(opt.status).toBe(204);
    expect(opt.headers.get("access-control-allow-headers")).toContain("x-molty-link-code");
    const no = await handleRest(new Request(base), w.backend);
    expect(no.status).toBe(401);
    const ok = await handleRest(new Request(`${base}?kind=call&limit=5`, { headers: { "x-molty-link-code": w.dev.code() } }), w.backend);
    expect(ok.status).toBe(200);
    const body = (await ok.json()) as any;
    expect(body.result).toMatchObject({ transcripts: true, limits: { max_age_h: 24 }, more: false });
    expect(body.result.entries.map((e: any) => e.op)).toEqual(["status"]);
    const bad = await handleRest(new Request(`${base}?kind=nope`, { headers: { "x-molty-link-code": w.dev.code() } }), w.backend);
    expect(bad.status).toBe(400);
    const set = await handleRest(new Request(base, { method: "POST", headers: { "x-molty-link-code": w.dev.code(), "content-type": "application/json" }, body: '{"transcripts":false}' }), w.backend);
    expect(((await set.json()) as any).result.transcripts).toBe(false);
    const mine = await handleRest(new Request("https://r/v1/activity?kind=call", { headers: { authorization: `Bearer ${a}` } }), w.backend);
    expect(mine.status).toBe(200);
    expect(((await mine.json()) as any).result.entries.map((e: any) => e.op)).toEqual(["status"]);
    expect((await handleRest(new Request("https://r/v1/activity"), w.backend)).status).toBe(401);
  });

  it("MCP molty_activity returns the caller's own entries", async () => {
    const w = await world();
    const a = await w.pair("Claude");
    await w.room.command(a, "status", {});
    const res = await handleMcp(
      new Request("https://r/mcp", { method: "POST", headers: { "content-type": "application/json", accept: "application/json, text/event-stream", authorization: `Bearer ${a}` }, body: JSON.stringify({ jsonrpc: "2.0", id: 1, method: "tools/call", params: { name: "molty_activity", arguments: { kind: "call" } } }) }),
      w.backend,
    );
    const out = (await res.json()) as any;
    expect(out.result.isError).toBe(false);
    expect(out.result.structuredContent.entries.map((e: any) => e.op)).toEqual(["status"]);
  });

  it("parseActivityQuery checks every param", () => {
    expect(parseActivityQuery({ since: "a12", limit: "5", kind: "call,job", errors: "1", job: `${DEVICE}.j_abcdefgh` })).toEqual({ since: "a12", limit: 5, kind: ["call", "job"], errors: true, job: `${DEVICE}.j_abcdefgh` });
    expect(parseActivityQuery({ since: "1700000000000" })).toEqual({ since: 1700000000000 });
    for (const bad of [{ since: "e4" }, { limit: "0" }, { limit: "501" }, { kind: "x" }, { job: "nope" }, { errors: "maybe" }]) expect(typeof parseActivityQuery(bad)).toBe("string");
  });

  it("lineLevel reads ESP_LOG levels (after a colour escape) and EVT", () => {
    expect(["EVT x", "E (1) t: m", "\u001b[0;31mE (1) t: m", "W (2) t: m", "I (3) t: m", "garbage"].map(lineLevel)).toEqual(["EVT", "E", "E", "W", "I", "?"]);
  });
});

it("old relay compat: the device's log frame is accepted only on a live socket (a hello-stage socket must say hello first)", async () => {
  const ctx = makeRoom({ timings: FAST });
  const s = ctx.host.open();
  ctx.room.socketOpen(s);
  await ctx.room.socketMessage(s, JSON.stringify({ t: "log", boot: 1, up: 1, lines: [[1, 1, "", "x"]] }));
  expect(s.closed?.code).toBe(4400);
  expect([...ctx.kv.data.keys()].filter((k) => k.startsWith("al:"))).toEqual([]);
  void KEY;
});
