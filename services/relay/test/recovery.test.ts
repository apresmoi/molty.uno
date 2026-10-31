// Recovery when a turn goes wrong: a failed say is cancelled on the device, the failure reason
// is in the activity log, a talk's uncollected words become an `utterance`, a tap's end is an
// ordinary end of turn, and a stale in_reply_to does not inflate the reply time. Plus `gestures`.
import { describe, expect, it } from "vitest";
import { decodeAudioFrame, encodeAudioFrame } from "../src/core/protocol.js";
import type { DeviceRoom } from "../src/core/room.js";
import { claimTurn } from "../src/core/turns.js";
import { DEVICE, type FakeSocket, LINK, connect, makeRoom } from "./helpers.js";

const FAST = { cmdTimeoutMs: 100, pairWindowMs: 400, sayChunkSamples: 1600, sayLeadFrames: 2, listenGraceMs: 200, sayDoneGraceMs: 300, collectGraceMs: 40 };
const CAPS = ["say", "listen", "listen_stop", "listen_window", "ack", "shake_talk", "say_cancel", "gestures"];

interface Dev {
  end?: Record<string, unknown>;
  /** Delays the listen's frames (ms). */
  listenAfter?: number;
  /** cmd say is taken but never answered: the relay times out, the device waits. */
  sayLost?: boolean;
}

function device(room: DeviceRoom, sock: FakeSocket, dev: Dev) {
  const cmds: Array<{ name: string; args: any }> = [];
  const origText = sock.sendText.bind(sock);
  const origBin = sock.sendBinary.bind(sock);
  sock.sendText = (t: string) => {
    origText(t);
    const f = JSON.parse(t);
    if (f.t === "pair.request") setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "pair.result", req: f.req, ok: true })), 1);
    if (f.t !== "cmd") return;
    cmds.push({ name: f.name, args: f.args });
    if (f.name === "say" && dev.sayLost) return;
    const result = f.name === "listen" ? { job: f.args.job, ms: 30000, ack: "chirp" } : f.name === "gestures" ? { bindings: { tap: "event" } } : {};
    setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "res", id: f.id, ok: true, result })), 1);
    if (f.name === "listen") {
      setTimeout(() => {
        void room.socketMessage(sock, encodeAudioFrame({ s: f.args.job, seq: 0, kind: "pcm16", rate: 16000, end: false }, new Uint8Array(new Int16Array(3200).fill(8000).buffer)));
        void room.socketMessage(sock, new TextEncoder().encode(JSON.stringify({ s: f.args.job, seq: 1, kind: "pcm16", rate: 16000, end: true, ...dev.end })));
      }, dev.listenAfter ?? 5);
    }
  };
  sock.sendBinary = (b: Uint8Array) => {
    origBin(b);
    const fr = decodeAudioFrame(b)!;
    if (fr.header.end) setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "evt", name: "say.done", data: { job: fr.header.s } })), 1);
  };
  return { cmds };
}

async function setup(dev: Dev = {}, opts: { caps?: string[]; timings?: Record<string, number> } = {}) {
  const ctx = makeRoom({ timings: { ...FAST, ...opts.timings } });
  const sock = await connect(ctx.room, ctx.host, undefined, LINK, { caps: opts.caps ?? CAPS });
  const d = device(ctx.room, sock, dev);
  const out = await ctx.room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "talker" });
  if (!out.ok || out.result.status !== "approved") throw new Error(JSON.stringify(out));
  return { ...ctx, sock, dev: d, token: out.result.agent_token };
}

const pause = (ms: number) => new Promise((r) => setTimeout(r, ms));
const until = async (cond: () => boolean, ms = 1500) => {
  const end = Date.now() + ms;
  while (!cond() && Date.now() < end) await pause(5);
};
const utterances = async (w: Awaited<ReturnType<typeof setup>>) => {
  const r = (await w.room.events(w.token, { since: 0, wait_ms: 0 })) as any;
  return ((r.result?.events ?? []) as any[]).filter((e) => e.type === "utterance");
};

describe("a failed say", () => {
  it("is cancelled on a device that has the cap", async () => {
    const w = await setup({ sayLost: true });
    const r = await w.room.talk(w.token, { say: "Hello there.", listen: false });
    expect(r.ok).toBe(false);
    await until(() => w.dev.cmds.some((c) => c.name === "say_cancel"));
    const job = w.dev.cmds.find((c) => c.name === "say")!.args.job;
    expect(w.dev.cmds.find((c) => c.name === "say_cancel")!.args).toEqual({ job });
  });

  it("older firmware is not sent a command it does not know", async () => {
    const w = await setup({ sayLost: true }, { caps: CAPS.filter((c) => c !== "say_cancel") });
    expect((await w.room.talk(w.token, { say: "Hello there.", listen: false })).ok).toBe(false);
    await pause(30);
    expect(w.dev.cmds.some((c) => c.name === "say_cancel")).toBe(false);
  });

  it("a say that went well is not cancelled", async () => {
    const w = await setup();
    expect((await w.room.talk(w.token, { say: "Hello there.", listen: false })).ok).toBe(true);
    await pause(30);
    expect(w.dev.cmds.some((c) => c.name === "say_cancel")).toBe(false);
  });

  it("the activity log says why it failed", async () => {
    const w = await setup({ sayLost: true });
    await w.room.talk(w.token, { say: "Hello there.", listen: false });
    await pause(20);
    const a = (await w.room.activityLog(w.token, { kind: "job" })) as any;
    const entry = a.result.entries.find((e: any) => e.kind === "job" && e.op === "talk");
    expect(entry).toMatchObject({ ok: false, error: "device_timeout" });
  });
});

describe("a talk's words nobody collected", () => {
  it("go out as one utterance", async () => {
    const w = await setup({ listenAfter: 40, end: { reason: "turn_end" } }, { timings: { talkBudgetMs: 5 } });
    const r = (await w.room.talk(w.token, {})) as any;
    expect(r.result.status).toBe("pending");
    await pause(150);
    const u = await utterances(w);
    expect(u).toHaveLength(1);
    expect(u[0].data).toMatchObject({ turn_id: r.result.job, trigger: "talk", reason: "turn_end" });
    expect(u[0].data.text).toBeTruthy();
    // an agent that reads the job after all is told, so it does not answer twice
    const late = (await w.room.job(w.token, r.result.job, 0)) as any;
    expect(late.result.result).toMatchObject({ delivered_as: "utterance", turn_id: r.result.job });
  });

  it("are marked for an agent that reads the job while they go out", async () => {
    const w = await setup({ listenAfter: 40, end: { reason: "turn_end" } }, { timings: { talkBudgetMs: 5 } });
    const r = (await w.room.talk(w.token, {})) as any;
    const kv = (w.room as any).kv;
    const put = kv.put.bind(kv);
    let poll: Promise<any> | undefined;
    kv.put = async (k: string, v: any) => {
      if (!poll && v?.result?.delivered_as) {
        poll = w.room.job(w.token, r.result.job, 0);
        await pause(10);
      }
      return put(k, v);
    };
    await pause(150);
    expect(poll).toBeDefined();
    expect((await poll).result.result).toMatchObject({ delivered_as: "utterance" });
    expect(await utterances(w)).toHaveLength(1);
  });

  it("are not repeated when the talk call itself returned them", async () => {
    const w = await setup({ end: { reason: "turn_end" } });
    const r = (await w.room.talk(w.token, {})) as any;
    expect(r.result.status).toBe("done");
    await pause(120);
    expect(await utterances(w)).toHaveLength(0);
  });

  it("are not repeated when the agent followed the pending job", async () => {
    const w = await setup({ listenAfter: 40, end: { reason: "turn_end" } }, { timings: { talkBudgetMs: 5, collectGraceMs: 60 } });
    const r = (await w.room.talk(w.token, {})) as any;
    const v = (await w.room.job(w.token, r.result.job, 1000)) as any;
    expect(v.result.status).toBe("done");
    await pause(150);
    expect(await utterances(w)).toHaveLength(0);
  });

  it("nothing heard: nothing delivered", async () => {
    const w = await setup({ listenAfter: 30, end: { reason: "no_speech" } }, { timings: { talkBudgetMs: 5 } });
    await w.room.talk(w.token, {});
    await pause(150);
    expect(await utterances(w)).toHaveLength(0);
  });
});

describe("a tap's end of the recording", () => {
  it("is an ordinary end of turn: transcribed, reason tap", async () => {
    const w = await setup({ end: { reason: "tap" } });
    const r = (await w.room.talk(w.token, {})) as any;
    expect(r.result).toMatchObject({ status: "done", reason: "tap" });
    expect(r.result.heard).toBeTruthy();
  });
});

describe("a stale in_reply_to", () => {
  it("is timed from the last time the conversation heard the human", () => {
    const c = claimTurn({ id: "t", at: 1000, by: "a", heard_at: 50000 }, "a", "t", 51000);
    expect(c).toMatchObject({ ok: true, turn: { id: "t", at: 50000 } });
    expect(claimTurn({ id: "t", at: 1000, by: "a" }, "a", "t", 2000)).toMatchObject({ ok: true, turn: { at: 1000 } });
  });
});

describe("a conversation on one turn id", () => {
  it("each reply is timed from the words it answers, not the turn's start", async () => {
    const w = await setup({ listenAfter: 30, end: { reason: "turn_end" } }, { timings: { talkBudgetMs: 5 } });
    const first = (await w.room.talk(w.token, {})) as any;
    await pause(120); // uncollected: its words are now turn `first.job`
    const turn = first.result.job;
    const heard = (await w.room.talk(w.token, { say: "Okay.", in_reply_to: turn, listen_ms: 5000 })) as any;
    await w.room.job(w.token, heard.result.job, 1000);
    await pause(150); // far longer than the agent "thinks" below: the old base would show it
    const reply = (await w.room.talk(w.token, { say: "Right.", in_reply_to: turn, listen: false })) as any;
    const done = reply.result.status === "done" ? reply.result : ((await w.room.job(w.token, reply.result.job, 1000)) as any).result.result;
    const firstAnswer = ((await w.room.job(w.token, heard.result.job, 0)) as any).result.result.timings.turn.reply;
    expect(done.timings.turn.id).toBe(turn);
    expect(done.timings.turn.reply).toBeGreaterThanOrEqual(100);
    expect(done.timings.turn.reply).toBeLessThan(firstAnswer + 200);
    expect(done.timings.turn.reply).toBeLessThan(260);
  });
});

describe("gestures", () => {
  it("reads and sets, and passes what the device answers", async () => {
    const w = await setup();
    expect(await w.room.command(w.token, "gestures", {})).toMatchObject({ ok: true, result: { bindings: { tap: "event" } } });
    expect((await w.room.command(w.token, "gestures", { bindings: { button_reject: "talk" } })).ok).toBe(true);
    expect(w.dev.cmds.filter((c) => c.name === "gestures").map((c) => c.args)).toEqual([{}, { bindings: { button_reject: "talk" } }]);
  });

  it("refuses unknown inputs and actions before the device sees them", async () => {
    const w = await setup();
    expect(await w.room.command(w.token, "gestures", { bindings: { nose: "talk" } })).toMatchObject({ ok: false, error: "invalid_args" });
    expect(await w.room.command(w.token, "gestures", { bindings: { tap: "dance" } })).toMatchObject({ ok: false, error: "invalid_args" });
    expect(w.dev.cmds.some((c) => c.name === "gestures")).toBe(false);
  });

  it("firmware without the cap: unsupported", async () => {
    const w = await setup({}, { caps: CAPS.filter((c) => c !== "gestures") });
    expect(await w.room.command(w.token, "gestures", {})).toMatchObject({ ok: false, error: "unsupported" });
  });
});
