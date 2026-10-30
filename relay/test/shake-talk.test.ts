// Shake to talk (core/turns.ts, docs/AGENT-LINK.md "Shake to talk"): the device starts a listen on
// a shake and names it in the shake event; the relay adopts it as a listen job (normal voice path),
// emits ONE `utterance` when the human said something (inbox + webhooks), and the first agent to
// say / talk claims the turn (others: turn_claimed).
import { describe, expect, it } from "vitest";
import type { RelayBackend } from "../src/core/api.js";
import { type AgentEvent, SAMPLE_EVENTS } from "../src/core/events.js";
import { decodeAudioFrame, encodeAudioFrame } from "../src/core/protocol.js";
import type { DeviceRoom } from "../src/core/room.js";
import { TURN_HOLD_MS, claimTurn } from "../src/core/turns.js";
import { renderRequest } from "../src/core/webhook.js";
import { handleMcp } from "../src/surfaces/mcp.js";
import { handleRest } from "../src/surfaces/rest.js";
import { DEVICE, type FakeSocket, LINK, connect, flush, makeRoom } from "./helpers.js";

const FAST = { cmdTimeoutMs: 300, pairWindowMs: 400, sayChunkSamples: 1600, sayLeadFrames: 2, listenGraceMs: 200, sayDoneGraceMs: 300 };
const CAPS = ["say", "listen", "listen_stop", "listen_window", "ack", "shake_talk"];
const SHAKE = { peak_g: 2.4, reversals: 5, ms: 900 };

interface Dev {
  /** The listen's end frame extras. */
  end?: Record<string, unknown>;
  /** Mic samples before the end frame. */
  samples?: number;
  /** The device refuses `cmd listen {adopt}` with this code. */
  adoptError?: string;
}

/** A device that answers cmds, plays say streams and streams the adopted listen's audio. */
function device(room: DeviceRoom, sock: FakeSocket, dev: Dev) {
  const cmds: Array<{ name: string; args: any }> = [];
  const origText = sock.sendText.bind(sock);
  const origBin = sock.sendBinary.bind(sock);
  sock.sendText = (t: string) => {
    origText(t);
    const f = JSON.parse(t);
    if (f.t !== "cmd") return;
    cmds.push({ name: f.name, args: f.args });
    const refuse = f.name === "listen" && f.args.adopt && dev.adoptError;
    const result = f.name === "listen" ? { job: f.args.job, ms: 30000, ack: "chirp" } : {};
    setTimeout(() => void room.socketMessage(sock, JSON.stringify(refuse ? { t: "res", id: f.id, ok: false, error: dev.adoptError } : { t: "res", id: f.id, ok: true, result })), 1);
    if (f.name === "listen" && !refuse) {
      setTimeout(() => {
        const n = dev.samples ?? 3200;
        if (n) void room.socketMessage(sock, encodeAudioFrame({ s: f.args.job, seq: 0, kind: "pcm16", rate: 16000, end: false }, new Uint8Array(new Int16Array(n).fill(8000).buffer)));
        void room.socketMessage(sock, new TextEncoder().encode(JSON.stringify({ s: f.args.job, seq: n ? 1 : 0, kind: "pcm16", rate: 16000, end: true, ...(dev.end ?? { reason: "silence", speech_ms: 900 }) })));
      }, 15);
    }
  };
  sock.sendBinary = (b: Uint8Array) => {
    origBin(b);
    const fr = decodeAudioFrame(b)!;
    if (fr.header.end) setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "evt", name: "say.done", data: { job: fr.header.s } })), 5);
  };
  return { cmds };
}

interface Sent {
  url: string;
  body: string;
}

async function setup(dev: Dev = {}, opts: { caps?: string[] } = {}) {
  const sent: Sent[] = [];
  const ctx = makeRoom({
    timings: FAST,
    scheduler: { setAlarm: async () => {} },
    fetch: async (r) => {
      sent.push({ url: r.url, body: await r.text() });
      return new Response("ok");
    },
    resolveHost: async () => ["93.184.216.34"],
  });
  ctx.voice.ttsSamples = 1600;
  ctx.voice.sttText = "what's on my calendar today?";
  const sock = await connect(ctx.room, ctx.host, undefined, LINK, { caps: opts.caps ?? CAPS });
  const d = device(ctx.room, sock, dev);
  let code = LINK;
  const pairAgent = async (name: string) => {
    const out = await ctx.room.pair({ device_id: DEVICE, link_code: code, agent_name: name });
    if (!out.ok || out.result.status !== "approved") throw new Error(JSON.stringify(out));
    await flush();
    code = (await ctx.kv.get<{ code: string }>("link"))?.code ?? code;
    return out.result.agent_token;
  };
  // the device rotates its link code after a pairing (the fake answers pair.request itself)
  const origText = sock.sendText;
  sock.sendText = (t: string) => {
    origText(t);
    const f = JSON.parse(t);
    if (f.t === "pair.request") setTimeout(() => void ctx.room.socketMessage(sock, JSON.stringify({ t: "pair.result", req: f.req, ok: true, link_code: `NEWC${2 + sent.length}345`.slice(0, 8) })), 2);
  };
  const emit = (name: string, data: unknown) => ctx.room.socketMessage(sock, JSON.stringify({ t: "evt", name, data }));
  const backend: RelayBackend = { device: () => ctx.room };
  return { ...ctx, sock, dev: d, sent, pairAgent, emit, backend };
}

const until = async (cond: () => boolean | Promise<boolean>, ms = 1500) => {
  const end = Date.now() + ms;
  while (!(await cond()) && Date.now() < end) await new Promise((r) => setTimeout(r, 5));
};

/** A job's record, read without the ride-along (which would mark the inbox read). */
const jobRec = (kv: { get<T>(k: string): Promise<T | undefined> }, id: string) => kv.get<any>(`job:${id}`);
const inbox = async (room: DeviceRoom, token: string) => ((await room.events(token, { wait_ms: 0 })) as any).result.events as AgentEvent[];
/** Reads the token's inbox until an utterance (or ms) — the listen job runs in the background. */
async function utterances(room: DeviceRoom, token: string, ms = 1500): Promise<AgentEvent[]> {
  const seen: AgentEvent[] = [];
  await until(async () => {
    seen.push(...(await inbox(room, token)));
    return seen.some((e) => e.type === "utterance");
  }, ms);
  return seen;
}

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

const rest = async (b: RelayBackend, token: string, path: string, body: unknown) => {
  const r = await handleRest(new Request(`https://relay.test${path}`, { method: "POST", headers: { authorization: `Bearer ${token}`, "content-type": "application/json" }, body: JSON.stringify(body) }), b);
  return { status: r.status, body: (await r.json()) as any };
};

describe("shake to talk", () => {
  it("adopts the device's listen and emits ONE utterance with the words, timings and device state", async () => {
    const w = await setup();
    const token = await w.pairAgent("grok");
    await w.emit("shake", { ...SHAKE, listen: "shake-1" });
    // the relay took the shake's listen as its own job: cmd listen {job, adopt}
    const listen = w.dev.cmds.find((c) => c.name === "listen")!;
    expect(listen.args).toMatchObject({ adopt: "shake-1", rate: 16000, job: expect.stringMatching(/^molty-k7m2q9xa4c\.j_/) });
    expect(listen.args.ms).toBeUndefined(); // the device's own window (the setting), counted from the shake
    const events = await utterances(w.room, token);
    const turnId = listen.args.job;
    expect(events.map((e) => e.type)).toEqual(["shake", "utterance"]);
    expect(events[0].data).toEqual({ ...SHAKE, listening: true, turn_id: turnId }); // the local id never reaches agents
    expect(events[1].data).toEqual({
      turn_id: turnId,
      text: "what's on my calendar today?",
      reason: "silence",
      ms: { speech: 900, stt: expect.any(Number) },
      trigger: "shake",
      device: { caps: CAPS, mood: "thinking", voice: "fake" },
    });
    // once: nothing more arrives, and the turn's job holds the listen result
    await new Promise((r) => setTimeout(r, 60));
    expect(await inbox(w.room, token)).toEqual([]);
    const job = (await w.room.job(token, turnId, 0)) as any;
    expect(job.result).toMatchObject({ kind: "listen", status: "done", result: { text: "what's on my calendar today?" } });
  });

  it("no_speech (or an empty transcript): no utterance at all — only the shake", async () => {
    for (const dev of [{ end: { reason: "no_speech", speech_ms: 0 } }, { end: { reason: "silence", speech_ms: 400 } }]) {
      const w = await setup(dev);
      if (!("reason" in dev.end && dev.end.reason === "no_speech")) w.voice.sttText = "  ";
      const token = await w.pairAgent("a");
      await w.emit("shake", { ...SHAKE, listen: "shake-1" });
      const listen = w.dev.cmds.find((c) => c.name === "listen")!;
      await until(async () => (await jobRec(w.kv, listen.args.job))?.status === "done");
      const evs = await inbox(w.room, token);
      expect(evs.map((e) => e.type)).toEqual(["shake"]);
      expect(evs[0].data.listening).toBe(true);
      expect(await w.kv.get("turn")).toBeUndefined(); // no turn to claim
    }
  });

  it("a plain shake (setting off / older firmware) starts nothing; busy audio declines the device's listen", async () => {
    const w = await setup();
    const token = await w.pairAgent("a");
    await w.emit("shake", SHAKE);
    await flush();
    expect(w.dev.cmds.filter((c) => c.name === "listen" || c.name === "listen_stop")).toEqual([]);
    expect((await inbox(w.room, token)).map((e) => e.data)).toEqual([SHAKE]);
    // a malformed local id is ignored (plain shake)
    await w.emit("shake", { ...SHAKE, listen: "bad id!" });
    expect((await inbox(w.room, token)).map((e) => e.data)).toEqual([SHAKE]);
    // a say is playing: the shake's listen is declined (listen_stop on its local id), the shake stays plain
    const say = (await w.room.command(token, "say", { text: "One moment please, this is long." })) as any;
    expect(say.ok).toBe(true);
    await w.emit("shake", { ...SHAKE, listen: "shake-2" });
    await until(() => w.dev.cmds.some((c) => c.name === "listen_stop"));
    expect(w.dev.cmds.find((c) => c.name === "listen_stop")!.args).toEqual({ job: "shake-2", reason: "no_speech" });
    expect(w.dev.cmds.some((c) => c.name === "listen" && c.args.adopt)).toBe(false);
    expect((await inbox(w.room, token)).map((e) => e.data)).toEqual([SHAKE]);
  });

  it("a board without a mic declines; the device refusing the adoption ends the job with no utterance", async () => {
    const w = await setup({}, { caps: ["say", "shake_talk"] });
    const token = await w.pairAgent("a");
    await w.emit("shake", { ...SHAKE, listen: "shake-1" });
    await until(() => w.dev.cmds.some((c) => c.name === "listen_stop"));
    expect(w.dev.cmds.some((c) => c.name === "listen")).toBe(false);
    const v = await setup({ adoptError: "listen_gone" });
    const t2 = await v.pairAgent("a");
    await v.emit("shake", { ...SHAKE, listen: "shake-1" });
    const job = v.dev.cmds.find((c) => c.name === "listen")!.args.job;
    await until(async () => (await jobRec(v.kv, job))?.status === "failed");
    expect(await jobRec(v.kv, job)).toMatchObject({ status: "failed", error: "listen_gone" });
    expect((await inbox(v.room, t2)).map((e) => e.type)).toEqual(["shake"]);
  });

  it("a device cannot forge an utterance", async () => {
    const w = await setup();
    const token = await w.pairAgent("a");
    await w.emit("utterance", SAMPLE_EVENTS.utterance.data);
    expect(await inbox(w.room, token)).toEqual([]);
  });

  it("webhooks: the default body and templates carry the whole utterance (no confirming call needed)", async () => {
    const w = await setup();
    const token = await w.pairAgent("grok");
    const body = '{"say":"{{event.data.text}}","turn":"{{event.data.turn_id}}","speech":{{event.data.ms.speech}},"mood":"{{event.data.device.mood}}","why":"{{event.data.reason}}","line":"{{text}}","nope":{{event.data.ms.nope}}}';
    const reg = (await w.room.webhook(token, { action: "set", spec: { url: "https://hooks.example.org/molty", events: ["utterance"], body, content_type: "application/json" } })) as any;
    expect(reg.ok).toBe(true);
    // the dry run renders the sample utterance
    expect(JSON.parse(reg.result.dry_run.body)).toMatchObject({ say: "what's on my calendar today?", speech: 1800, mood: "thinking", why: "turn_end", nope: null });
    const plain = await w.pairAgent("plain");
    expect(((await w.room.webhook(plain, { action: "set", spec: { url: "https://hooks.example.org/plain" } })) as any).ok).toBe(true);
    await w.emit("shake", { ...SHAKE, listen: "shake-1" });
    await until(async () => (await w.kv.get("turn")) !== undefined);
    await flush();
    await w.room.alarm();
    const turnId = w.dev.cmds.find((c) => c.name === "listen")!.args.job;
    const mine = w.sent.filter((s) => s.url.endsWith("/molty"));
    expect(mine).toHaveLength(1); // filtered to utterance: the shake did not wake this agent
    expect(JSON.parse(mine[0].body)).toEqual({
      say: "what's on my calendar today?",
      turn: turnId,
      speech: 900,
      mood: "thinking",
      why: "silence",
      line: 'The human said to Molty: "what\'s on my calendar today?"',
      nope: null,
    });
    // no body template: {device, event} with the full utterance in event.data
    const def = w.sent.filter((s) => s.url.endsWith("/plain")).map((s) => JSON.parse(s.body));
    expect(def.map((d) => d.event.type)).toEqual(["shake", "utterance"]);
    expect(def[1]).toMatchObject({ device: DEVICE, event: { type: "utterance", data: { turn_id: turnId, text: "what's on my calendar today?", reason: "silence", trigger: "shake", ms: { speech: 900 }, device: { mood: "thinking", caps: CAPS } } } });
  });

  it("nested placeholders: up to 3 deep, own fields only, missing → default / null", () => {
    const ev = SAMPLE_EVENTS.utterance;
    const r = renderRequest({ url: "https://h.example.org/x?m={{event.data.device.mood}}", method: "POST", headers: {}, content_type: "application/json", body: '{"a":{{event.data.device.caps}},"b":"{{event.data.device.nope|none}}","c":{{event.data.text.length}}}' }, DEVICE, ev);
    expect(r.url).toBe("https://h.example.org/x?m=thinking");
    expect(JSON.parse(r.body)).toEqual({ a: ["say", "listen", "shake_talk"], b: "none", c: null });
  });
});

describe("who answers a turn", () => {
  async function turn() {
    const w = await setup();
    const a = await w.pairAgent("grok");
    const b = await w.pairAgent("claude");
    await w.emit("shake", { ...SHAKE, listen: "shake-1" });
    const evs = await utterances(w.room, a);
    const u = evs.find((e) => e.type === "utterance")!;
    return { ...w, a, b, turnId: u.data.turn_id as string };
  }

  it("the first say / talk claims the turn; another agent's gets turn_claimed (not audio_busy), with or without in_reply_to", async () => {
    const w = await turn();
    const r = await mcp(w.backend, w.a, "molty_talk", { say: "Two meetings.", listen: false, in_reply_to: w.turnId });
    expect(r.isError).toBe(false);
    // correlation: the answer's timings measure from the utterance
    expect(r.structuredContent.timings.turn).toEqual({ id: w.turnId, reply: expect.any(Number), first_audio: expect.any(Number) });
    expect(r.structuredContent.timings.turn.first_audio).toBeGreaterThanOrEqual(r.structuredContent.timings.turn.reply);
    const other = await rest(w.backend, w.b, `/v1/devices/${DEVICE}/talk`, { say: "Hello!", listen: false, in_reply_to: w.turnId });
    expect(other).toMatchObject({ status: 409, body: { ok: false, error: "turn_claimed", turn_id: w.turnId } });
    const bare = (await w.room.command(w.b, "say", { text: "Hello!" })) as any;
    expect(bare).toMatchObject({ ok: false, error: "turn_claimed" });
    // the claimant keeps the floor for follow-ups (no in_reply_to needed)
    const again = (await w.room.command(w.a, "say", { text: "Anything else?", in_reply_to: w.turnId })) as any;
    expect(again.ok).toBe(true);
    await until(async () => ((await w.room.job(w.a, again.result.job, 0)) as any).result.status === "done");
    expect(((await w.room.job(w.a, again.result.job, 0)) as any).result.result.timings.turn).toMatchObject({ id: w.turnId });
    // mood / show are never claimed
    expect(((await w.room.command(w.b, "mood", { mood: "happy" })) as any).ok).toBe(true);
  });

  it("a say without in_reply_to claims an unclaimed turn too", async () => {
    const w = await turn();
    const s = (await w.room.command(w.b, "say", { text: "On it." })) as any;
    expect(s.ok).toBe(true);
    const late = await mcp(w.backend, w.a, "molty_talk", { say: "Sure!", in_reply_to: w.turnId });
    expect(late.isError).toBe(true);
    expect(late.structuredContent).toMatchObject({ error: "turn_claimed" });
  });

  it("activity log: the shake, its utterance (a transcript: redacted when the owner says so), the adopted listen job and a turn_claimed refusal", async () => {
    const w = await turn();
    expect((await w.room.talk(w.a, { say: "Two meetings.", listen: false, in_reply_to: w.turnId })).ok).toBe(true);
    expect((await w.room.talk(w.b, { say: "Me too!", listen: false, in_reply_to: w.turnId })).ok).toBe(false);
    const owner = async (q: Record<string, unknown> = {}) => ((await w.room.ownerActivity((await w.kv.get<{ code: string }>("link"))!.code, { action: "get", query: q })) as any).result.entries as any[];
    const es = await owner();
    expect(es.find((e) => e.ev === "shake")).toMatchObject({ kind: "device", data: { peak_g: 2.4 } });
    expect(es.find((e) => e.ev === "utterance")).toMatchObject({ kind: "device", job: w.turnId, data: { text: "what's on my calendar today?", trigger: "shake" } });
    expect(es.find((e) => e.kind === "job" && e.job === w.turnId)).toMatchObject({ op: "listen", ok: true });
    expect(es.find((e) => e.kind === "job" && e.job === w.turnId).agent).toBeUndefined(); // the device's own turn: no agent
    expect(es.find((e) => e.kind === "call" && e.agent === "claude")).toMatchObject({ op: "talk", ok: false, error: "turn_claimed" });
    // neither agent reads the utterance through the activity log (it reaches them as an event)
    expect(JSON.stringify(await w.room.activityLog(w.a, {}))).not.toContain("calendar");
    // transcripts off: the next utterance's text never reaches storage
    await w.room.ownerActivity((await w.kv.get<{ code: string }>("link"))!.code, { action: "set", transcripts: false });
    w.voice.sttText = "remind me about the dentist";
    await w.emit("shake", { ...SHAKE, listen: "shake-2" });
    await until(async () => (await owner()).filter((e) => e.ev === "utterance").length === 2);
    expect((await owner()).filter((e) => e.ev === "utterance").at(-1)).toMatchObject({ redacted: true, data: { text: "" } });
    expect(JSON.stringify([...w.kv.data.entries()].filter(([k]) => k.startsWith("al:")))).not.toContain("dentist");
  });

  it("P1-3: an answer refused for audio_busy claims nothing — the next agent may still answer the turn", async () => {
    const w = await setup();
    const a = await w.pairAgent("grok");
    const b = await w.pairAgent("claude");
    const c = await w.pairAgent("other");
    const turnId = `${DEVICE}.j_seededturn0001`;
    await w.kv.put("turn", { id: turnId, at: Date.now() });
    // the speaker / mic is held by another job (a listen claims no turn)
    const held = (await w.room.command(c, "listen", {})) as any;
    expect(held.ok).toBe(true);
    expect(await w.room.talk(a, { say: "Two meetings.", listen: false, in_reply_to: turnId })).toMatchObject({ ok: false, error: "audio_busy" });
    expect(await w.room.command(a, "say", { text: "Two meetings.", in_reply_to: turnId })).toMatchObject({ ok: false, error: "audio_busy" });
    expect((await w.kv.get<any>("turn")).by).toBeUndefined();
    await w.room.job(c, held.result.job, 5000);
    const r = (await w.room.command(b, "say", { text: "Hello!", in_reply_to: turnId })) as any;
    expect(r.ok).toBe(true);
    expect((await w.kv.get<any>("turn")).by).toBeDefined();
    await w.room.job(b, r.result.job, 5000);
  });

  it("in_reply_to is checked: a turn id, nothing else", async () => {
    const w = await turn();
    for (const bad of ["e5", "molty-k7m2q9xa4c", 7]) {
      expect(await w.room.command(w.a, "say", { text: "x", in_reply_to: bad })).toMatchObject({ ok: false, error: "invalid_args" });
      expect(await w.room.talk(w.a, { say: "x", in_reply_to: bad })).toMatchObject({ ok: false, error: "invalid_args" });
    }
  });

  it("claimTurn: expiry, stale replies, owner, others", () => {
    const t = { id: "molty-k7m2q9xa4c.j_aaaaaaaaaaaaaaaa", at: 1000 };
    expect(claimTurn(undefined, "A", undefined, 1000)).toEqual({ ok: true });
    expect(claimTurn(t, "A", undefined, 1000)).toEqual({ ok: true, turn: t, next: { ...t, by: "A" } });
    expect(claimTurn(t, "A", t.id, 1000)).toEqual({ ok: true, turn: t, next: { ...t, by: "A" } });
    const claimed = { ...t, by: "A" };
    expect(claimTurn(claimed, "A", undefined, 2000)).toEqual({ ok: true, turn: t });
    expect(claimTurn(claimed, "B", t.id, 2000)).toEqual({ ok: false, turn: t.id });
    expect(claimTurn(claimed, "B", undefined, 2000)).toEqual({ ok: false, turn: t.id });
    // an answer to an older turn: allowed, uncorrelated
    expect(claimTurn(claimed, "B", "molty-k7m2q9xa4c.j_bbbbbbbbbbbbbbbb", 2000)).toEqual({ ok: true });
    // after TURN_HOLD_MS everyone talks again
    expect(claimTurn(claimed, "B", undefined, 1000 + TURN_HOLD_MS + 1)).toEqual({ ok: true });
    expect(claimTurn(claimed, "B", t.id, 1000 + TURN_HOLD_MS)).toEqual({ ok: false, turn: t.id });
  });
});
