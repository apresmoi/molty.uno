import { describe, expect, it } from "vitest";
import { b64Encode } from "../src/core/crypto.js";
import { decodeAudioFrame, encodeAudioFrame } from "../src/core/protocol.js";
import type { DeviceRoom } from "../src/core/room.js";
import { MemoryKV } from "../src/core/store.js";
import { DEVICE, FakeHost, type FakeSocket, KEY, LINK, connect, flush, makeRoom } from "./helpers.js";

const FAST = { cmdTimeoutMs: 150, pairWindowMs: 400, sayChunkSamples: 1600, sayLeadFrames: 2, listenGraceMs: 100 };

type Handler = (name: string, args: Record<string, any>, id: string) => { ok: boolean; result?: unknown; error?: string } | undefined;

/** Makes a fake device answer cmd frames (and optionally pair requests) like firmware would. */
function autoDevice(room: DeviceRoom, sock: FakeSocket, handler: Handler, pair: boolean | "none" = true) {
  const orig = sock.sendText.bind(sock);
  sock.sendText = (t: string) => {
    orig(t);
    const f = JSON.parse(t);
    if (f.t === "cmd") {
      const r = handler(f.name, f.args, f.id);
      if (r) setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "res", id: f.id, ...r })), 1);
    }
    if (f.t === "pair.request" && pair !== "none") {
      setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "pair.result", req: f.req, ok: pair, link_code: "NEWC2345" })), 5);
    }
  };
}

async function paired(handler: Handler = () => ({ ok: true, result: {} }), hello: Record<string, unknown> = {}) {
  const ctx = makeRoom({ timings: FAST });
  const sock = await connect(ctx.room, ctx.host, undefined, undefined, hello);
  autoDevice(ctx.room, sock, handler);
  const out = await ctx.room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "tester" });
  if (!out.ok || out.result.status !== "approved") throw new Error(`pair failed ${JSON.stringify(out)}`);
  return { ...ctx, sock, token: out.result.agent_token };
}

describe("device auth (TOFU)", () => {
  it("claims an unknown device_id on first hello and welcomes it", async () => {
    const { room, host, kv } = makeRoom();
    const s = await connect(room, host);
    expect(s.last("welcome")).toEqual({ t: "welcome" });
    expect(s.meta?.stage).toBe("live");
    expect((await kv.get<{ key_hash: string }>("device"))?.key_hash).toMatch(/^[0-9a-f]{64}$/);
  });

  it("closes 4401 on a different key for a claimed id, and accepts the right key again", async () => {
    const { room, host } = makeRoom();
    (await connect(room, host)).close(1000, "bye");
    const evil = await connect(room, host, b64Encode(new Uint8Array(32).fill(9)));
    expect(evil.closed?.code).toBe(4401);
    expect(evil.texts).toEqual([]);
    const good = await connect(room, host);
    expect(good.last("welcome")).toBeDefined();
  });

  it("closes 4401 on a hello for another device id, and 4400 when the first frame is not hello", async () => {
    const { room, host } = makeRoom();
    const s = host.open();
    room.socketOpen(s);
    await room.socketMessage(s, JSON.stringify({ t: "hello", id: "molty-zzzzzzzzzz", key: KEY }));
    expect(s.closed?.code).toBe(4401);
    const s2 = host.open();
    room.socketOpen(s2);
    await room.socketMessage(s2, JSON.stringify({ t: "res", id: "x", ok: true }));
    expect(s2.closed?.code).toBe(4400);
  });

  it("a second authenticated connection replaces the first", async () => {
    const { room, host } = makeRoom();
    const a = await connect(room, host);
    const b = await connect(room, host);
    expect(a.closed?.code).toBe(4409);
    expect(b.meta?.stage).toBe("live");
  });

  it("answers JSON ping with pong", async () => {
    const { room, host } = makeRoom();
    const s = await connect(room, host);
    await room.socketMessage(s, '{"t":"ping"}');
    expect(s.last("pong")).toEqual({ t: "pong" });
  });
});

describe("pairing", () => {
  it("approves: device sees agent_name + 4-digit code; agent gets a scoped token once", async () => {
    const { room, host, kv } = makeRoom({ timings: FAST });
    const s = await connect(room, host);
    autoDevice(room, s, () => ({ ok: true, result: { fw: "x" } }));
    const out = await room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "Test Agent" });
    expect(out.ok).toBe(true);
    if (!out.ok || out.result.status !== "approved") throw new Error();
    const req = s.last("pair.request");
    expect(req.agent_name).toBe("Test Agent");
    expect(req.code).toMatch(/^\d{4}$/);
    expect(out.result.code).toBe(req.code);
    expect(out.result.agent_token).toMatch(/^molty-k7m2q9xa4c\.[A-Za-z0-9_-]{43}$/);
    // only the hash is stored
    const tokens = await kv.list("tok:");
    expect(tokens.size).toBe(1);
    expect(JSON.stringify([...kv.data.values()])).not.toContain(out.result.agent_token);
    expect((await room.command(out.result.agent_token, "status", {})).ok).toBe(true);
  });

  it("rotates: the old link_code is dead after a pairing; the device's new one works", async () => {
    const { room, sock } = await paired();
    expect(await room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "again" })).toMatchObject({ ok: false, error: "bad_link_code" });
    const second = await room.pair({ device_id: DEVICE, link_code: "NEWC2345", agent_name: "again" });
    expect(second.ok && second.result.status).toBe("approved");
    expect(sock.frames().filter((f) => f.t === "pair.request")).toHaveLength(2);
  });

  it("the used link_code dies even when the device has not reported a new one yet", async () => {
    const { room, host } = makeRoom({ timings: FAST });
    const s = await connect(room, host);
    const p = room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "x" });
    await flush();
    await room.socketMessage(s, JSON.stringify({ t: "pair.result", req: s.last("pair.request").req, ok: true }));
    expect((await p).ok).toBe(true);
    expect(await room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "y" })).toMatchObject({ ok: false, error: "bad_link_code" });
  });

  it("an approval arriving after the window, with nobody waiting, mints nothing", async () => {
    const { room, host, kv } = makeRoom({ timings: FAST });
    const s = await connect(room, host);
    const p = await room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "x", wait_ms: 0 });
    if (!p.ok || p.result.status !== "pending") throw new Error();
    await new Promise((r) => setTimeout(r, FAST.pairWindowMs + 50));
    await room.socketMessage(s, JSON.stringify({ t: "pair.result", req: s.last("pair.request").req, ok: true }));
    expect((await kv.list("tok:")).size).toBe(0);
    expect(await room.pair({ device_id: DEVICE, pair_id: p.result.pair_id })).toMatchObject({ ok: false, error: "timeout" });
  });

  it("rejects wrong link codes without bothering the device", async () => {
    const { room, host } = makeRoom();
    const s = await connect(room, host);
    expect(await room.pair({ device_id: DEVICE, link_code: "ZZZZ2345", agent_name: "x" })).toMatchObject({ ok: false, error: "bad_link_code" });
    expect(await room.pair({ device_id: DEVICE, link_code: "bad", agent_name: "x" })).toMatchObject({ ok: false, error: "bad_link_code" });
    expect(s.last("pair.request")).toBeUndefined();
  });

  it("reports device_offline, rejected, timeout and pair_busy", async () => {
    const { room, host } = makeRoom({ timings: FAST });
    expect(await room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "x" })).toMatchObject({ ok: false, error: "device_offline" });
    const s = await connect(room, host);
    autoDevice(room, s, () => undefined, false);
    expect(await room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "x" })).toMatchObject({ ok: false, error: "rejected" });

    const ctx2 = makeRoom({ timings: FAST });
    const s2 = await connect(ctx2.room, ctx2.host);
    autoDevice(ctx2.room, s2, () => undefined, "none");
    const first = ctx2.room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "x" });
    await flush();
    expect(await ctx2.room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "y" })).toMatchObject({ ok: false, error: "pair_busy" });
    expect(await first).toMatchObject({ ok: false, error: "timeout" });
    // a late approval after the window does not mint a token
    const req = s2.last("pair.request");
    await ctx2.room.socketMessage(s2, JSON.stringify({ t: "pair.result", req: req.req, ok: true }));
    expect((await ctx2.kv.list("tok:")).size).toBe(0);
  });

  it("device-side timeout reason maps to timeout", async () => {
    const { room, host } = makeRoom({ timings: FAST });
    const s = await connect(room, host);
    const orig = s.sendText.bind(s);
    s.sendText = (t) => {
      orig(t);
      const f = JSON.parse(t);
      if (f.t === "pair.request") setTimeout(() => void room.socketMessage(s, JSON.stringify({ t: "pair.result", req: f.req, ok: false, reason: "timeout" })), 2);
    };
    expect(await room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "x" })).toMatchObject({ ok: false, error: "timeout" });
  });

  it("short waits return pending + pair_id; resuming claims the token exactly once", async () => {
    const { room, host } = makeRoom({ timings: FAST });
    const s = await connect(room, host);
    const p = await room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "x", wait_ms: 0 });
    if (!p.ok || p.result.status !== "pending") throw new Error(JSON.stringify(p));
    expect(p.result.pair_id).toMatch(/^p_/);
    const req = s.last("pair.request");
    expect(p.result.code).toBe(req.code);
    await room.socketMessage(s, JSON.stringify({ t: "pair.result", req: req.req, ok: true }));
    const r = await room.pair({ device_id: DEVICE, pair_id: p.result.pair_id, wait_ms: 0 });
    expect(r.ok && r.result.status).toBe("approved");
    expect(await room.pair({ device_id: DEVICE, pair_id: p.result.pair_id })).toMatchObject({ ok: false, error: "not_found" });
  });

  it("ignores pair.result with the wrong req", async () => {
    const { room, host, kv } = makeRoom({ timings: FAST });
    const s = await connect(room, host);
    const p = room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "x" });
    await flush();
    await room.socketMessage(s, JSON.stringify({ t: "pair.result", req: "forged", ok: true }));
    expect((await kv.list("tok:")).size).toBe(0);
    expect(await p).toMatchObject({ ok: false, error: "timeout" });
  });
});

describe("commands", () => {
  it("routes cmd -> res and returns the device result", async () => {
    const seen: any[] = [];
    const { room, token } = await paired((name, args) => (seen.push({ name, args }), { ok: true, result: { volume: args.volume } }));
    expect(await room.command(token, "volume", { volume: 7 })).toEqual({ ok: true, result: { volume: 7 } });
    expect(await room.command(token, "state", { state: { blink: 0.5 } })).toMatchObject({ ok: true });
    expect(seen).toEqual([{ name: "volume", args: { volume: 7 } }, { name: "state", args: { blink: 0.5 } }]);
  });

  it("status merges relay info", async () => {
    const { room, token } = await paired(() => ({ ok: true, result: { uptime: 5, mood: "idle" } }));
    const out = await room.command(token, "status", {});
    expect(out).toMatchObject({ ok: true, result: { uptime: 5, relay: { device_id: DEVICE, online: true, fw: "test-fw", board: null, caps: ["say", "listen"] } } });
  });

  it("a board without speaker/mic: say/listen are unsupported up front, status names the board", async () => {
    const seen: string[] = [];
    const { room, token, voice } = await paired((name) => (seen.push(name), { ok: true, result: {} }), {
      board: "waveshare-esp32-s3-touch-lcd-1.28",
      caps: ["fluid", "show", "mood", "creature"],
    });
    expect(await room.command(token, "say", { text: "hi" })).toMatchObject({ ok: false, error: "unsupported" });
    expect(await room.command(token, "listen", {})).toMatchObject({ ok: false, error: "unsupported" });
    expect(voice.ttsCalls).toEqual([]);
    expect(seen).toEqual([]);
    const out = await room.command(token, "status", {});
    expect(out).toMatchObject({ ok: true, result: { relay: { board: "waveshare-esp32-s3-touch-lcd-1.28", caps: ["fluid", "show", "mood", "creature"] } } });
  });

  it("rejects bad tokens, passes device errors through, times out, reports offline", async () => {
    const { room, token, sock } = await paired((name) => (name === "imu" ? { ok: false, error: "imu_unavailable" } : name === "mood" ? undefined : { ok: true }));
    expect(await room.command("nope", "status", {})).toMatchObject({ ok: false, error: "unauthorized" });
    expect(await room.command(`${DEVICE}.${"A".repeat(43)}`, "status", {})).toMatchObject({ ok: false, error: "unauthorized" });
    expect(await room.command(token, "imu", {})).toEqual({ ok: false, error: "imu_unavailable" });
    expect(await room.command(token, "mood", { mood: "happy" })).toEqual({ ok: false, error: "device_timeout" });
    expect(await room.command(token, "volume", { volume: 99 })).toMatchObject({ ok: false, error: "invalid_args" });
    sock.close(1000, "gone");
    expect(await room.command(token, "status", {})).toMatchObject({ ok: false, error: "device_offline" });
  });

  it("fails pending commands immediately when the device disconnects", async () => {
    const { room, token, sock } = await paired(() => undefined);
    const p = room.command(token, "imu", {});
    await flush();
    sock.close(1006, "drop");
    expect(await p).toEqual({ ok: false, error: "device_offline" });
  });

  it("revoke kills the token", async () => {
    const { room, token } = await paired();
    expect((await room.revoke(token)).ok).toBe(true);
    expect(await room.command(token, "status", {})).toMatchObject({ ok: false, error: "unauthorized" });
  });
});

describe("say / listen jobs", () => {
  it("say: TTS, device cmd, paced pcm16 frames with end, then done {ms}", async () => {
    const { room, token, sock, voice } = await paired();
    voice.ttsSamples = 4000; // 250 ms -> 3 frames of 1600
    const start = await room.command(token, "say", { text: "hi there", voice: "luna" });
    if (!start.ok) throw new Error(JSON.stringify(start));
    const job = start.result.job as string;
    expect(job).toMatch(/^molty-k7m2q9xa4c\.j_/);
    const queued = await room.job(token, job, 0);
    expect(["queued", "running"]).toContain(queued.ok && queued.result.status);
    const done = await room.job(token, job, 3000);
    expect(done).toMatchObject({ ok: true, result: { job, kind: "say", status: "done", result: { ms: 250, text: "hi there" } } });
    const cmd = sock.frames().find((f) => f.t === "cmd" && f.name === "say");
    expect(cmd.args).toMatchObject({ job, text: "hi there", ms: 250, voice: "luna", rate: 16000 });
    expect(voice.ttsCalls).toEqual([{ text: "hi there", voice: "luna" }]);
    const frames = sock.bins.map((b) => decodeAudioFrame(b)!);
    expect(frames.map((f) => [f.header.s, f.header.seq, f.header.end, f.payload.byteLength])).toEqual([
      [job, 0, false, 3200],
      [job, 1, false, 3200],
      [job, 2, true, 1600],
    ]);
    expect(new Int16Array(frames[0].payload.slice().buffer)[0]).toBe(1000);
  });

  it("say paces frames in real time after the lead", async () => {
    const { room, token, sock, voice } = await paired();
    voice.ttsSamples = 1600 * 6; // 6 frames of 100 ms, lead 2
    const t0 = Date.now();
    const start = await room.command(token, "say", { text: "pace" });
    const job = (start as any).result.job;
    await room.job(token, job, 3000);
    const elapsed = Date.now() - t0;
    expect(sock.bins).toHaveLength(6);
    expect(elapsed).toBeGreaterThanOrEqual(550); // 600 ms of audio
  });

  it("say finishes early on evt say.done; audio_busy while a job runs", async () => {
    const { room, token, sock, voice } = await paired();
    voice.ttsSamples = 16000 * 5;
    const job = ((await room.command(token, "say", { text: "long" })) as any).result.job;
    expect(await room.command(token, "listen", { ms: 100 })).toMatchObject({ ok: false, error: "audio_busy" });
    await new Promise((r) => setTimeout(r, 100));
    await room.socketMessage(sock, JSON.stringify({ t: "evt", name: "say.done", data: { job } }));
    const done = await room.job(token, job, 1000);
    expect(done.ok && done.result.status).toBe("done");
    expect(sock.bins.length).toBeLessThan(50);
  });

  it("say fails when the device refuses or disconnects", async () => {
    const { room, token, sock } = await paired((name) => (name === "say" ? { ok: false, error: "speaker_busy" } : { ok: true }));
    const job = ((await room.command(token, "say", { text: "x" })) as any).result.job;
    expect(await room.job(token, job, 2000)).toMatchObject({ ok: true, result: { status: "failed", error: "speaker_busy" } });
    void sock;
  });

  it("listen: device streams pcm16 up, relay runs STT on end -> {text, duration_ms, peak, rms}", async () => {
    let sock!: FakeSocket;
    const ctx = await paired((name, args) => {
      if (name === "listen") {
        setTimeout(() => {
          const pcm = new Int16Array(1600).fill(16384);
          const bytes = new Uint8Array(pcm.buffer);
          void ctx.room.socketMessage(sock, encodeAudioFrame({ s: args.job, seq: 0, kind: "pcm16", rate: 16000, end: false }, bytes));
          void ctx.room.socketMessage(sock, encodeAudioFrame({ s: "other-stream", seq: 0, kind: "pcm16", rate: 16000, end: false }, bytes));
          void ctx.room.socketMessage(sock, encodeAudioFrame({ s: args.job, seq: 1, kind: "pcm16", rate: 16000, end: true }, bytes));
        }, 10);
      }
      return { ok: true };
    });
    sock = ctx.sock;
    const job = ((await ctx.room.command(ctx.token, "listen", { ms: 200 })) as any).result.job;
    const done = await ctx.room.job(ctx.token, job, 2000);
    const cmd = sock.frames().find((f) => f.t === "cmd" && f.name === "listen");
    expect(cmd.args).toEqual({ job, ms: 200, rate: 16000 });
    expect(done).toMatchObject({ ok: true, result: { status: "done", result: { text: "hello molty", duration_ms: 200, peak: 0.5, rms: 0.5 } } });
    expect(ctx.voice.sttCalls[0].length).toBe(3200);
  });

  it("listen fails no_audio when the device never streams", async () => {
    const { room, token } = await paired();
    const job = ((await room.command(token, "listen", { ms: 50 })) as any).result.job;
    expect(await room.job(token, job, 2000)).toMatchObject({ ok: true, result: { status: "failed", error: "no_audio" } });
  });

  it("TTS errors fail the job with the provider message", async () => {
    const { room, token, voice } = await paired();
    voice.tts = async () => {
      throw new Error("Binding AI needs to be run remotely");
    };
    const job = ((await room.command(token, "say", { text: "x" })) as any).result.job;
    expect(await room.job(token, job, 1000)).toMatchObject({ ok: true, result: { status: "failed", error: "Binding AI needs to be run remotely" } });
  });

  it("after eviction, an orphaned running job reports failed:interrupted; tokens and device key survive", async () => {
    const kv = new MemoryKV();
    const host = new FakeHost();
    const a = makeRoom({ kv, host, timings: FAST });
    const s = await connect(a.room, host);
    autoDevice(a.room, s, () => undefined);
    const p = await a.room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "x" });
    const token = (p as any).result.agent_token;
    a.voice.ttsSamples = 16000 * 10;
    const job = ((await a.room.command(token, "say", { text: "x" })) as any).result.job;
    // a fresh room on the same storage + sockets == the DO woke after eviction
    const b = makeRoom({ kv, host, timings: FAST });
    expect(await b.room.job(token, job, 0)).toMatchObject({ ok: true, result: { status: "failed", error: "interrupted" } });
    // the socket's own metadata still marks it live: commands route without a new hello
    autoDevice(b.room, s, () => ({ ok: true, result: { pong: 1 } }));
    expect(await b.room.command(token, "imu", {})).toEqual({ ok: true, result: { pong: 1 } });
  });

  it("job ids are scoped to the device and token", async () => {
    const { room, token } = await paired();
    expect(await room.job(token, "molty-zzzzzzzzzz.j_abcdefghijkl", 0)).toMatchObject({ ok: false, error: "not_found" });
    expect(await room.job("bad", "molty-k7m2q9xa4c.j_abcdefghijkl", 0)).toMatchObject({ ok: false, error: "unauthorized" });
  });
});

describe("forget + link code updates", () => {
  it("forget wipes key, tokens and link; a new key can then claim", async () => {
    const { room, token, sock, kv, host } = await paired();
    await room.socketMessage(sock, JSON.stringify({ t: "forget" }));
    expect(sock.last("forgotten")).toBeDefined();
    expect(sock.closed?.code).toBe(4410);
    expect(kv.data.size).toBe(0);
    const fresh = await connect(room, host, b64Encode(new Uint8Array(32).fill(3)));
    expect(fresh.last("welcome")).toBeDefined();
    expect(await room.command(token, "status", {})).toMatchObject({ ok: false, error: "unauthorized" });
  });

  it("link_code frames and evts update the accepted code", async () => {
    const { room, host } = makeRoom({ timings: FAST });
    const s = await connect(room, host, KEY, null);
    autoDevice(room, s, () => ({ ok: true }));
    expect(await room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "x" })).toMatchObject({ error: "bad_link_code" });
    await room.socketMessage(s, JSON.stringify({ t: "link_code", link_code: LINK }));
    expect((await room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "x" })).ok).toBe(true);
    await room.socketMessage(s, JSON.stringify({ t: "evt", name: "link_code", data: { link_code: "QQQQ2345" } }));
    expect((await room.pair({ device_id: DEVICE, link_code: "QQQQ2345", agent_name: "y" })).ok).toBe(true);
  });
});
