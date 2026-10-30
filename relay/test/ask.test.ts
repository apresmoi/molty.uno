// molty_ask / POST /v1/devices/<id>/ask: a question answered with a tap.
// Relay side only: validation, say -> ask -> answer, timeout / cancelled /
// offline, overlapping asks, molty_wait for long asks, the device `answer`
// event's strict checks, and `answer` in the inbox / webhooks.
import { describe, expect, it } from "vitest";
import type { RelayBackend } from "../src/core/api.js";
import { MAX_ASK_LABEL, MAX_ASK_OPTIONS, validateAsk } from "../src/core/commands.js";
import { AGENT_EVENT_TYPES, SAMPLE_EVENTS, agentEventData } from "../src/core/events.js";
import { decodeAudioFrame } from "../src/core/protocol.js";
import type { DeviceRoom } from "../src/core/room.js";
import { renderRequest } from "../src/core/webhook.js";
import { handleMcp } from "../src/surfaces/mcp.js";
import { handleRest } from "../src/surfaces/rest.js";
import { DEVICE, type FakeSocket, LINK, connect, makeRoom } from "./helpers.js";

const FAST = { cmdTimeoutMs: 300, pairWindowMs: 400, sayChunkSamples: 1600, sayLeadFrames: 2, sayDoneGraceMs: 300, askGraceMs: 100 };
const CAPS = ["say", "listen", "ask"];

/** A device that answers cmds, plays say streams (say.done on the end frame) and records the asks it was shown. */
function device(room: DeviceRoom, sock: FakeSocket) {
  const cmds: Array<{ name: string; args: any; at: number }> = [];
  /** sayDoneMs: delay from the say end frame to say.done; null = never (playback failed). */
  const ctl = { refuse: undefined as string | undefined, sayDoneMs: 5 as number | null };
  const log: Array<{ kind: string; at: number }> = [];
  const origText = sock.sendText.bind(sock);
  const origBin = sock.sendBinary.bind(sock);
  sock.sendText = (t: string) => {
    origText(t);
    const f = JSON.parse(t);
    if (f.t === "pair.request") setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "pair.result", req: f.req, ok: true, link_code: "NEWC2345" })), 2);
    if (f.t !== "cmd") return;
    cmds.push({ name: f.name, args: f.args, at: Date.now() });
    const err = f.name === "ask" ? ctl.refuse : undefined;
    setTimeout(() => void room.socketMessage(sock, JSON.stringify(err ? { t: "res", id: f.id, ok: false, error: err } : { t: "res", id: f.id, ok: true, result: {} })), 1);
  };
  sock.sendBinary = (b: Uint8Array) => {
    origBin(b);
    const fr = decodeAudioFrame(b)!;
    if (fr.header.end && ctl.sayDoneMs !== null)
      setTimeout(() => {
        log.push({ kind: "say.done", at: Date.now() });
        void room.socketMessage(sock, JSON.stringify({ t: "evt", name: "say.done", data: { job: fr.header.s } }));
      }, ctl.sayDoneMs);
  };
  const asks = () => cmds.filter((c) => c.name === "ask");
  const answer = (data: Record<string, unknown>) => room.socketMessage(sock, JSON.stringify({ t: "evt", name: "answer", data }));
  return { cmds, asks, answer, ctl, log };
}

async function setup(opts: { caps?: string[]; timings?: Record<string, number>; hello?: Record<string, unknown> } = {}) {
  const sent: Array<{ url: string; body: string }> = [];
  const ctx = makeRoom({
    timings: { ...FAST, ...opts.timings },
    scheduler: { setAlarm: async () => {} },
    resolveHost: async () => ["93.184.216.34"],
    fetch: async (r) => (sent.push({ url: r.url, body: await r.text() }), new Response("ok")),
  });
  const sock = await connect(ctx.room, ctx.host, undefined, LINK, { caps: opts.caps ?? CAPS, ...opts.hello });
  const dev = device(ctx.room, sock);
  const out = await ctx.room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "asker" });
  if (!out.ok || out.result.status !== "approved") throw new Error(JSON.stringify(out));
  const backend: RelayBackend = { device: () => ctx.room };
  return { ...ctx, sock, dev, token: out.result.agent_token, backend, sent };
}

const until = async (cond: () => boolean, ms = 2000) => {
  const end = Date.now() + ms;
  while (!cond() && Date.now() < end) await new Promise((r) => setTimeout(r, 3));
  if (!cond()) throw new Error("condition never met");
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

const rest = async (b: RelayBackend, token: string, body: unknown) => {
  const r = await handleRest(new Request(`https://relay.test/v1/devices/${DEVICE}/ask`, { method: "POST", headers: { authorization: `Bearer ${token}`, "content-type": "application/json" }, body: JSON.stringify(body) }), b);
  return { status: r.status, body: (await r.json()) as any };
};

describe("validateAsk", () => {
  const bad = (args: unknown) => expect(() => validateAsk(args), JSON.stringify(args)).toThrow();
  it("accepts 1..5 trimmed labels of 1..8 printable ASCII chars, unique ignoring case", () => {
    expect(validateAsk({ options: ["ok"] })).toEqual({ options: ["ok"], timeout_s: 30 });
    expect(validateAsk({ options: [" yes ", "no", "later", "idk", "A b ~!"], timeout_s: 120, say: "Q?", voice: "luna" })).toEqual({
      options: ["yes", "no", "later", "idk", "A b ~!"],
      timeout_s: 120,
      say: "Q?",
      voice: "luna",
    });
    expect(validateAsk({ options: ["x".repeat(MAX_ASK_LABEL)], timeout_s: 1 }).options[0]).toHaveLength(8);
    expect(validateAsk({ options: ["  12345678  "] }).options).toEqual(["12345678"]); // trimmed before the length check
  });
  it("rejects each rule at its boundary, with a message an agent can act on", () => {
    bad({});
    bad({ options: [] });
    bad({ options: "yes" });
    bad({ options: ["a", "b", "c", "d", "e", "f"] }); // 6 > MAX_ASK_OPTIONS
    expect(MAX_ASK_OPTIONS).toBe(5);
    bad({ options: ["x".repeat(MAX_ASK_LABEL + 1)] });
    bad({ options: ["   "] }); // empty after trim
    bad({ options: [""] });
    bad({ options: ["yes", 3] });
    bad({ options: ["café"] }); // non-ASCII
    bad({ options: ["👍"] });
    bad({ options: ["a\tb"] }); // control char
    bad({ options: ["\u007f"] }); // DEL is not printable
    bad({ options: ["Yes", "yes"] }); // duplicate, case-insensitive
    bad({ options: ["yes ", " yes"] }); // duplicate after trim
    bad({ options: ["ok"], timeout_s: 0 });
    bad({ options: ["ok"], timeout_s: 121 });
    bad({ options: ["ok"], timeout_s: 1.5 });
    bad({ options: ["ok"], say: "" });
    bad({ options: ["ok"], voice: "bad voice!" });
    bad({ options: ["ok"], bogus: 1 });
    expect(() => validateAsk({ options: ["maybe later"] })).toThrow(/1\.\.8 characters.*shorten it/);
    expect(() => validateAsk({ options: ["no", "NO"] })).toThrow(/unique/);
    expect(() => validateAsk({ options: ["ok"], timeout_s: 0 })).toThrow(/timeout_s must be an integer 1\.\.120/);
  });
});

describe("molty_ask", () => {
  it("happy path with say: speaks (until say.done), then shows the options; the tap is the answer", async () => {
    const w = await setup();
    w.voice.ttsSamples = 1600;
    const call = mcp(w.backend, w.token, "molty_ask", { say: "Build now?", options: ["yes", " no ", "later"], timeout_s: 20 });
    await until(() => w.dev.asks().length === 1);
    const ask = w.dev.asks()[0];
    expect(ask.args).toEqual({ job: expect.stringMatching(/^molty-k7m2q9xa4c\.j_/), options: ["yes", "no", "later"], timeout_ms: 20000 });
    // the ask goes out only after the say played (say.done)
    expect(w.dev.cmds.map((c) => c.name)).toEqual(["say", "ask"]);
    await w.dev.answer({ job: ask.args.job, index: 2, label: "later" });
    const r = await call;
    expect(r.isError).toBe(false);
    expect(r.structuredContent).toMatchObject({
      ok: true,
      job: ask.args.job,
      status: "done",
      answer: "later",
      index: 2,
      said: "Build now?",
      ms: { tts_first_audio: expect.any(Number), speech: expect.any(Number), answer: expect.any(Number), total: expect.any(Number) },
      timings: { say: expect.any(Object), ask: { cmd: expect.any(Number), wait: expect.any(Number) } },
    });
    expect(r.structuredContent.reason).toBeUndefined();
  });

  it("waits for the device's say.done (bounded) before showing the options; none -> playback_timeout, no ask", async () => {
    const w = await setup();
    w.voice.ttsSamples = 1600; // 100 ms of speech; say.done 200 ms after the last frame (< 300 ms grace)
    w.dev.ctl.sayDoneMs = 200;
    const call = w.room.ask(w.token, { say: "Q?", options: ["ok"] });
    await until(() => w.dev.asks().length === 1);
    expect(w.dev.asks()[0].at).toBeGreaterThanOrEqual(w.dev.log.find((l) => l.kind === "say.done")!.at);
    await w.dev.answer({ job: w.dev.asks()[0].args.job, index: 0, label: "ok" });
    expect(await call).toMatchObject({ ok: true, result: { answer: "ok" } });
    w.dev.ctl.sayDoneMs = null;
    expect(await w.room.ask(w.token, { say: "Q?", options: ["ok"] })).toMatchObject({ ok: false, error: "playback_timeout" });
    expect(w.dev.asks()).toHaveLength(1);
  });

  it("without say: no TTS, the ask goes out at once; a label-less answer takes options[index]", async () => {
    const w = await setup();
    const call = w.room.ask(w.token, { options: ["ok"] });
    await until(() => w.dev.asks().length === 1);
    await w.dev.answer({ job: w.dev.asks()[0].args.job, index: 0 });
    expect(await call).toMatchObject({ ok: true, result: { status: "done", answer: "ok", index: 0 } });
    expect(w.voice.ttsCalls).toHaveLength(0);
    expect(w.dev.cmds.map((c) => c.name)).toEqual(["ask"]);
    expect((await call as any).result.said).toBeUndefined();
  });

  it("timeout: the device reports it -> answer null, reason timeout", async () => {
    const w = await setup();
    const call = w.room.ask(w.token, { options: ["yes", "no"], timeout_s: 1 });
    await until(() => w.dev.asks().length === 1);
    await w.dev.answer({ job: w.dev.asks()[0].args.job, index: null, reason: "timeout" });
    expect(await call).toMatchObject({ ok: true, result: { status: "done", answer: null, index: null, reason: "timeout" } });
  });

  it("timeout: a device that never answers is resolved by the relay after timeout_ms + grace", async () => {
    const w = await setup();
    const t0 = Date.now();
    const r = await w.room.ask(w.token, { options: ["yes", "no"], timeout_s: 1 });
    expect(r).toMatchObject({ ok: true, result: { answer: null, index: null, reason: "timeout" } });
    expect(Date.now() - t0).toBeGreaterThanOrEqual(1000 + FAST.askGraceMs - 20);
  });

  it("cancelled: the device reports a later command replaced the question", async () => {
    const w = await setup();
    const call = w.room.ask(w.token, { options: ["yes", "no"] });
    await until(() => w.dev.asks().length === 1);
    expect(await w.room.command(w.token, "mood", { mood: "happy" })).toMatchObject({ ok: true });
    await w.dev.answer({ job: w.dev.asks()[0].args.job, index: null, reason: "cancelled" });
    expect(await call).toMatchObject({ ok: true, result: { answer: null, index: null, reason: "cancelled" } });
  });

  it("offline: the device disconnects mid-ask -> answer null, reason offline (not an error)", async () => {
    const w = await setup();
    const call = w.room.ask(w.token, { options: ["yes", "no"] });
    await until(() => w.dev.asks().length === 1);
    w.sock.close(1006, "gone");
    expect(await call).toMatchObject({ ok: true, result: { status: "done", answer: null, index: null, reason: "offline" } });
  });

  it("offline: replaced by a newer socket mid-ask also resolves offline", async () => {
    const w = await setup();
    const call = w.room.ask(w.token, { options: ["yes", "no"] });
    await until(() => w.dev.asks().length === 1);
    await connect(w.room, w.host, undefined, LINK, { caps: CAPS });
    expect(await call).toMatchObject({ ok: true, result: { answer: null, reason: "offline" } });
  });

  it("overlapping asks resolve independently: the second cancels the first on the device; each gets its own answer", async () => {
    const w = await setup();
    const first = w.room.ask(w.token, { options: ["yes", "no"] });
    await until(() => w.dev.asks().length === 1);
    const second = w.room.ask(w.token, { options: ["red", "green", "blue"] });
    await until(() => w.dev.asks().length === 2);
    const [j1, j2] = w.dev.asks().map((a) => a.args.job);
    expect(j1).not.toBe(j2);
    // the device: ask 2 replaced ask 1; a stray answer naming job 1 with job 2's index is dropped
    await w.dev.answer({ job: j1, index: null, reason: "cancelled" });
    expect(await first).toMatchObject({ ok: true, result: { job: j1, answer: null, reason: "cancelled" } });
    await w.dev.answer({ job: j1, index: 1, label: "no" }); // late, j1 already resolved: dropped
    await w.dev.answer({ job: j2, index: 1, label: "green" });
    expect(await second).toMatchObject({ ok: true, result: { job: j2, answer: "green", index: 1 } });
    // and in the reverse order: the later answer first
    const a = w.room.ask(w.token, { options: ["a", "b"] });
    await until(() => w.dev.asks().length === 3);
    const b = w.room.ask(w.token, { options: ["c", "d"] });
    await until(() => w.dev.asks().length === 4);
    const [ja, jb] = w.dev.asks().slice(2).map((x) => x.args.job);
    await w.dev.answer({ job: jb, index: 0, label: "c" });
    await w.dev.answer({ job: ja, index: null, reason: "cancelled" });
    expect(await b).toMatchObject({ ok: true, result: { job: jb, answer: "c" } });
    expect(await a).toMatchObject({ ok: true, result: { job: ja, answer: null, reason: "cancelled" } });
  });

  it("a long ask: blocks <= budget, returns pending, and molty_wait finishes it (also via the jobs URL)", async () => {
    const w = await setup({ timings: { talkBudgetMs: 50 } });
    const r = await mcp(w.backend, w.token, "molty_ask", { say: "Q?", options: ["yes", "no"], timeout_s: 120 });
    expect(r.structuredContent).toMatchObject({ ok: true, status: "pending", job: expect.any(String), hint: expect.stringMatching(/molty_wait/) });
    const job = r.structuredContent.job;
    const waiting = await mcp(w.backend, w.token, "molty_wait", { job, timeout_s: 0 });
    expect(waiting.structuredContent).toMatchObject({ ok: true, status: "pending" });
    await until(() => w.dev.asks().length === 1);
    expect(w.dev.asks()[0].args.timeout_ms).toBe(120000);
    const done = mcp(w.backend, w.token, "molty_wait", { job, timeout_s: 3 });
    await sleep(20);
    await w.dev.answer({ job, index: 0, label: "yes" });
    expect((await done).structuredContent).toMatchObject({ ok: true, status: "done", answer: "yes", index: 0, said: "Q?" });
    const view = await handleRest(new Request(`https://relay.test/v1/jobs/${job}`, { headers: { authorization: `Bearer ${w.token}` } }), w.backend);
    expect(await view.json()).toMatchObject({ job, kind: "ask", status: "done", result: { answer: "yes" } });
  });

  it("the speaker is held only while speaking: an ask waiting for a tap does not block a say", async () => {
    const w = await setup();
    w.voice.ttsSamples = 1600;
    const call = w.room.ask(w.token, { say: "Q?", options: ["yes", "no"] }); // took the speaker for its question
    await until(() => w.dev.asks().length === 1);
    expect(await w.room.command(w.token, "say", { text: "meanwhile" })).toMatchObject({ ok: true, result: { job: expect.any(String) } });
    await w.dev.answer({ job: w.dev.asks()[0].args.job, index: null, reason: "cancelled" });
    expect(await call).toMatchObject({ ok: true, result: { reason: "cancelled" } });
    // ...but an ask WITH say while another audio job runs is audio_busy
    w.voice.ttsSamples = 16000;
    await w.room.command(w.token, "say", { text: "long" });
    expect(await w.room.ask(w.token, { say: "Q?", options: ["ok"] })).toMatchObject({ ok: false, error: "audio_busy" });
  });

  it("device answer events are checked strictly; malformed ones never resolve the ask", async () => {
    const w = await setup();
    const call = w.room.ask(w.token, { options: ["yes", "no"] });
    await until(() => w.dev.asks().length === 1);
    const job = w.dev.asks()[0].args.job;
    for (const bad of [
      { job, index: 2, label: "x" }, // index outside this job's 2 options
      { job, index: 5 }, // outside any ask
      { job, index: -1 },
      { job, index: 0.5 },
      { job, index: "0" },
      { job, index: 0, label: "no" }, // label is not options[0]
      { job, index: 0, label: "YES" }, // exact match only
      { job, index: 0, label: 7 },
      { job, index: 0, reason: "timeout" }, // a tap with a reason
      { job, index: null }, // no answer without a reason
      { job, index: null, reason: "bored" },
      { job, index: null, reason: "offline" }, // relay-only reason
      { job, index: null, label: "yes", reason: "timeout" },
      { index: 0, label: "yes" }, // no job
      { job: `${DEVICE}.j_unknownjob00`, index: 0, label: "yes" }, // not a pending ask
      { job: "not-a-job", index: 0, label: "yes" },
    ]) {
      await w.dev.answer(bad);
    }
    await w.room.socketMessage(w.sock, JSON.stringify({ t: "evt", name: "answer", data: "yes" }));
    await sleep(20);
    let settled = false;
    void call.then(() => (settled = true));
    await sleep(10);
    expect(settled).toBe(false);
    expect(await w.room.events(w.token, { wait_ms: 0 })).toMatchObject({ ok: true, result: { events: [] } });
    await w.dev.answer({ job, index: 1, label: "no" });
    expect(await call).toMatchObject({ ok: true, result: { answer: "no", index: 1 } });
    await w.dev.answer({ job, index: 0, label: "yes" }); // a second answer for the same job: dropped
    const evs = (await w.room.events(w.token, { since: 0, wait_ms: 0 })) as any;
    expect(evs.result.events.map((e: any) => e.data)).toEqual([{ job, index: 1, label: "no" }]);
  });

  it("answer reaches the inbox (pull + ride-along) once per ask, including no-answer reasons", async () => {
    const w = await setup();
    const a = w.room.ask(w.token, { options: ["yes", "no"] });
    await until(() => w.dev.asks().length === 1);
    const j1 = w.dev.asks()[0].args.job;
    await w.dev.answer({ job: j1, index: 0, label: "yes" });
    // the asking call itself carries its answer event as a ride-along (and marks it read)
    const r1 = (await a) as any;
    expect(r1.inbox.events.map((e: any) => e.data)).toEqual([{ job: j1, index: 0, label: "yes" }]);
    const b = w.room.ask(w.token, { options: ["yes", "no"] });
    await until(() => w.dev.asks().length === 2);
    const j2 = w.dev.asks()[1].args.job;
    await w.dev.answer({ job: j2, index: null, reason: "timeout" });
    await b;
    const c = w.room.ask(w.token, { options: ["ok"] });
    await until(() => w.dev.asks().length === 3);
    const j3 = w.dev.asks()[2].args.job;
    w.sock.close(1006, "gone");
    await c;
    expect(((await w.room.events(w.token, { wait_ms: 0 })) as any).result.events).toEqual([]); // all handed over already
    const evs = (await w.room.events(w.token, { since: 0, wait_ms: 0 })) as any;
    expect(evs.result.events.map((e: any) => [e.type, e.data])).toEqual([
      ["answer", { job: j1, index: 0, label: "yes" }],
      ["answer", { job: j2, index: null, label: null, reason: "timeout" }],
      ["answer", { job: j3, index: null, label: null, reason: "offline" }],
    ]);
  });

  it("answer events are delivered to webhooks and render through templates (dry run covers answer)", async () => {
    const w = await setup();
    // A template valid for shake/tap/button but not for an answer (label is a string: must be quoted) is refused at registration.
    const bad = await w.room.webhook(w.token, { action: "set", spec: { url: "https://hooks.example.org/x", content_type: "application/json", body: '{"l":{{event.data.label|0}}}' } });
    expect(bad).toMatchObject({ ok: false, error: "invalid_args", message: expect.stringMatching(/^for a answer event/) });
    const reg = await w.room.webhook(w.token, {
      action: "set",
      spec: { url: "https://hooks.example.org/x", content_type: "application/json", body: '{"t":"{{event.type}}","l":"{{event.data.label}}","i":{{event.data.index}},"r":"{{event.data.reason|}}","text":"{{text}}"}', events: ["answer"] },
    });
    expect(reg.ok).toBe(true);
    const call = w.room.ask(w.token, { options: ["yes", "no"] });
    await until(() => w.dev.asks().length === 1);
    await w.dev.answer({ job: w.dev.asks()[0].args.job, index: 1, label: "no" });
    await call;
    await w.room.socketMessage(w.sock, JSON.stringify({ t: "evt", name: "tap", data: { x: 0.5, y: 0.5 } })); // filtered out
    await w.room.alarm();
    expect(w.sent.map((s) => JSON.parse(s.body))).toEqual([{ t: "answer", l: "no", i: 1, r: "", text: "Molty's question was answered: no" }]);
    // the sample used by every dry run is a valid answer
    expect(AGENT_EVENT_TYPES).toContain("answer");
    expect(agentEventData("answer", SAMPLE_EVENTS.answer.data)).toEqual(SAMPLE_EVENTS.answer.data);
    expect(JSON.parse(renderRequest({ url: "https://h.example.org/", method: "POST", headers: {} } as any, DEVICE, SAMPLE_EVENTS.answer).body).event.type).toBe("answer");
  });

  it("REST /ask: 200 done, 202 pending, 400 with the reason, 401, 409 unsupported / device_offline", async () => {
    const w = await setup();
    const call = rest(w.backend, w.token, { options: ["yes", "no"] });
    await until(() => w.dev.asks().length === 1);
    await w.dev.answer({ job: w.dev.asks()[0].args.job, index: 0, label: "yes" });
    expect(await call).toMatchObject({ status: 200, body: { ok: true, result: { status: "done", answer: "yes", index: 0 } } });
    const tooLong = await rest(w.backend, w.token, { options: ["absolutely"] });
    expect(tooLong).toMatchObject({ status: 400, body: { ok: false, error: "invalid_args", message: expect.stringMatching(/1\.\.8 characters/) } });
    expect(await rest(w.backend, "nope", { options: ["ok"] })).toMatchObject({ status: 401 });
    const slow = await setup({ timings: { talkBudgetMs: 30 } });
    const p = await rest(slow.backend, slow.token, { options: ["ok"], timeout_s: 60 });
    expect(p).toMatchObject({ status: 202, body: { ok: true, result: { status: "pending", job: expect.any(String) } } });
    slow.sock.close(1006, "bye");
    expect(await slow.room.ask(slow.token, { options: ["ok"] })).toMatchObject({ ok: false, error: "device_offline" });
  });

  it("boards without touch (no `ask` cap) are unsupported before anything reaches the device; say needs a speaker", async () => {
    const noTouch = await setup({ caps: ["say", "listen"] });
    expect(await noTouch.room.ask(noTouch.token, { say: "Q?", options: ["ok"] })).toMatchObject({ ok: false, error: "unsupported", message: expect.stringMatching(/touch/) });
    expect(noTouch.voice.ttsCalls).toHaveLength(0);
    expect(noTouch.dev.cmds).toEqual([]);
    const mute = await setup({ caps: ["ask"] });
    expect(await mute.room.ask(mute.token, { say: "Q?", options: ["ok"] })).toMatchObject({ ok: false, error: "unsupported", message: expect.stringMatching(/speaker/) });
    const call = mute.room.ask(mute.token, { options: ["ok"] });
    await until(() => mute.dev.asks().length === 1);
    await mute.dev.answer({ job: mute.dev.asks()[0].args.job, index: 0, label: "ok" });
    expect(await call).toMatchObject({ ok: true, result: { answer: "ok" } });
  });

  it("a device that refuses the ask cmd fails the job with its error code", async () => {
    const w = await setup();
    w.dev.ctl.refuse = "unsupported";
    expect(await w.room.ask(w.token, { options: ["ok"] })).toMatchObject({ ok: false, error: "unsupported" });
    w.dev.ctl.refuse = "busy";
    const r = await rest(w.backend, w.token, { options: ["ok"] });
    expect(r).toMatchObject({ status: 502, body: { ok: false, error: "busy" } });
  });
});
