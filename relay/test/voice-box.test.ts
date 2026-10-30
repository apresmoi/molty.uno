// Voice box: pairing with role "voice", token role isolation both ways, the voice
// socket (replace, drop), provider selection by voice_mode with fallback, streaming
// STT during the listen, the timings' provider field and the owner (console) API.
import { describe, expect, it } from "vitest";
import type { RelayBackend } from "../src/core/api.js";
import { decodeAudioFrame, encodeAudioFrame } from "../src/core/protocol.js";
import type { DeviceRoom } from "../src/core/room.js";
import { handleMcp } from "../src/surfaces/mcp.js";
import { handleRest } from "../src/surfaces/rest.js";
import { DEVICE, type FakeHost, type FakeSocket, LINK, connect, makeRoom } from "./helpers.js";

const FAST = { cmdTimeoutMs: 300, pairWindowMs: 400, sayChunkSamples: 1600, sayLeadFrames: 2, listenGraceMs: 200, sayDoneGraceMs: 300, boxSttFinalMs: 300 };
const NEXT = ["NEWC2345", "NEWD2345", "NEWE2345", "NEWF2345"];

/** A device that answers cmds, plays say streams, and streams a listen as 3 chunks 30 ms apart, then the end frame. */
function device(room: DeviceRoom, sock: FakeSocket) {
  const log: Array<{ at: number; kind: string; name?: string; args?: any }> = [];
  let codes = 0;
  const text = sock.sendText.bind(sock);
  const bin = sock.sendBinary.bind(sock);
  sock.sendText = (t: string) => {
    text(t);
    const f = JSON.parse(t);
    if (f.t === "cmd") {
      log.push({ at: Date.now(), kind: "cmd", name: f.name, args: f.args });
      setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "res", id: f.id, ok: true, result: {} })), 1);
      if (f.name === "listen") {
        const job = f.args.job;
        const chunk = (seq: number) => encodeAudioFrame({ s: job, seq, kind: "pcm16", rate: 16000, end: false }, new Uint8Array(new Int16Array(1600).fill(4000 + seq).buffer));
        for (let i = 0; i < 3; i++) setTimeout(() => void room.socketMessage(sock, chunk(i)), 10 + 30 * i);
        setTimeout(() => {
          log.push({ at: Date.now(), kind: "end" });
          void room.socketMessage(sock, new TextEncoder().encode(JSON.stringify({ s: job, seq: 3, kind: "pcm16", rate: 16000, end: true, reason: "silence", speech_ms: 250 })));
        }, 110);
      }
    }
    if (f.t === "pair.request") setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "pair.result", req: f.req, ok: true, link_code: NEXT[codes++] })), 2);
  };
  sock.sendBinary = (b: Uint8Array) => {
    bin(b);
    const fr = decodeAudioFrame(b)!;
    if (fr.header.end) setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "evt", name: "say.done", data: { job: fr.header.s } })), 5);
  };
  return { log };
}

interface BoxOpts {
  /** Never answer tts / stt (a hung box). */
  silent?: boolean;
  stt?: boolean;
  tts?: boolean;
}

/** A voice box on the relay's voice socket: synthesises 3200 samples per tts, transcribes by counting the chunks it got. */
function box(room: DeviceRoom, sock: FakeSocket, o: BoxOpts = {}) {
  const got: Record<string, Array<{ at: number; bytes: number }>> = {};
  const tts: any[] = [];
  const control: any[] = [];
  const text = sock.sendText.bind(sock);
  const say = (m: unknown) => void room.socketMessage(sock, JSON.stringify(m));
  sock.sendText = (t: string) => {
    text(t);
    const f = JSON.parse(t);
    control.push(f);
    if (o.silent) return;
    if (f.t === "tts") {
      tts.push(f);
      setTimeout(() => {
        if (sock.closed) return;
        void room.socketMessage(sock, encodeAudioFrame({ s: f.id, seq: 0, kind: "pcm16", rate: 16000, end: false }, new Uint8Array(new Int16Array(3200).fill(500).buffer)));
        say({ t: "tts.end", id: f.id });
      }, 5);
    }
    if (f.t === "stt.start") got[f.id] = [];
    if (f.t === "stt.end") setTimeout(() => !sock.closed && say({ t: "stt.final", id: f.id, text: `box heard ${got[f.id].length} chunks`, ms: { final: 3 } }), 5);
  };
  sock.sendBinary = (b: Uint8Array) => {
    const fr = decodeAudioFrame(b)!;
    got[fr.header.s]?.push({ at: Date.now(), bytes: fr.payload.byteLength });
  };
  const hello = () => room.socketMessage(sock, JSON.stringify({ t: "voice.hello", name: "gpu box", version: "0.1.0", voices: ["a.wav", "b.wav"], stt: o.stt ?? true, tts: o.tts ?? true }));
  return { got, tts, control, hello };
}

async function pairAs(room: DeviceRoom, link: string, name: string, role?: "agent" | "voice") {
  const out = await room.pair({ device_id: DEVICE, link_code: link, agent_name: name, ...(role ? { role } : {}) });
  if (!out.ok || out.result.status !== "approved") throw new Error(JSON.stringify(out));
  return out.result;
}

async function openBox(room: DeviceRoom, host: FakeHost, token: string, o: BoxOpts = {}) {
  const auth = await room.voiceAuth(token);
  if (!auth.ok) throw new Error(JSON.stringify(auth));
  const sock = host.open();
  room.voiceOpen(sock, auth.result.hash);
  const b = box(room, sock, o);
  await b.hello();
  return { sock, ...b };
}

async function setup(o: { box?: BoxOpts | false; keepClosing?: boolean } = {}) {
  const ctx = makeRoom({ timings: FAST });
  ctx.host.keepClosing = !!o.keepClosing;
  const sock = await connect(ctx.room, ctx.host);
  const dev = device(ctx.room, sock);
  const agent = await pairAs(ctx.room, LINK, "talker");
  const voice = await pairAs(ctx.room, NEXT[0], "gpu box", "voice");
  const b = o.box === false ? undefined : await openBox(ctx.room, ctx.host, voice.agent_token, o.box || {});
  const backend: RelayBackend = { device: () => ctx.room };
  return { ...ctx, sock, dev, token: agent.agent_token, voiceToken: voice.agent_token, box: b, backend, link: NEXT[1] };
}

const until = async (cond: () => boolean, ms = 1000) => {
  const end = Date.now() + ms;
  while (!cond() && Date.now() < end) await new Promise((r) => setTimeout(r, 5));
};

const rest = async (b: RelayBackend, method: string, path: string, body?: unknown, headers: Record<string, string> = {}) => {
  const r = await handleRest(new Request(`https://relay.test${path}`, { method, headers: { "content-type": "application/json", ...headers }, body: body === undefined ? undefined : JSON.stringify(body) }), b);
  return { status: r.status, headers: r.headers, body: r.status === 204 ? null : ((await r.json()) as any) };
};

describe("voice box pairing", () => {
  it("role voice: same tap flow, the device is told it is a voice box, the token is marked voice", async () => {
    const { room, host, kv } = makeRoom({ timings: FAST });
    const s = await connect(room, host);
    device(room, s);
    const r = await pairAs(room, LINK, "gpu box", "voice");
    expect(r).toMatchObject({ status: "approved", role: "voice", agent_name: "gpu box", device_id: DEVICE });
    expect(s.last("pair.request").agent_name).toBe("voice box · gpu box");
    const recs = [...(await kv.list<any>("tok:")).values()];
    expect(recs).toEqual([{ agent_name: "gpu box", created: expect.any(Number), role: "voice" }]);
    // no inbox cursor for a box
    expect((await kv.list("cur:")).size).toBe(0);
    // a name that already says it is not prefixed twice
    await pairAs(room, NEXT[0], "Voice box upstairs", "voice");
    expect(s.last("pair.request").agent_name).toBe("Voice box upstairs");
  });

  it("agent pairing is unchanged: no role anywhere, name shown verbatim", async () => {
    const { room, host, kv } = makeRoom({ timings: FAST });
    const s = await connect(room, host);
    device(room, s);
    const r = await pairAs(room, LINK, "Claude");
    expect(Object.keys(r).sort()).toEqual(["agent_name", "agent_token", "code", "device_id", "status"]);
    expect(s.last("pair.request").agent_name).toBe("Claude");
    expect([...(await kv.list<any>("tok:")).values()][0]).not.toHaveProperty("role");
    const b: RelayBackend = { device: () => room };
    // REST pair reply keeps its exact shape for agents, and adds role only for a box
    const agent = await rest(b, "POST", "/v1/pair", { device_id: DEVICE, link_code: NEXT[0], agent_name: "rest agent" });
    expect(Object.keys(agent.body).sort()).toEqual(["agent_name", "agent_token", "code", "device_id", "ok"]);
    const voice = await rest(b, "POST", "/v1/pair", { device_id: DEVICE, link_code: NEXT[1], agent_name: "box", role: "voice" });
    expect(voice.body).toMatchObject({ ok: true, role: "voice" });
    expect((await rest(b, "POST", "/v1/pair", { device_id: DEVICE, link_code: NEXT[2], agent_name: "x", role: "admin" })).body).toMatchObject({ ok: false, error: "invalid_args" });
  });

  it("one box per device: pairing a new box revokes the old token and closes its socket", async () => {
    const w = await setup();
    const second = await pairAs(w.room, w.link, "new box", "voice");
    expect(w.box!.sock.closed?.code).toBe(4401);
    expect(await w.room.voiceAuth(w.voiceToken)).toMatchObject({ ok: false, error: "unauthorized" });
    expect((await w.room.voiceAuth(second.agent_token)).ok).toBe(true);
    // agents are untouched
    expect((await w.room.command(w.token, "mood", { mood: "happy" })).ok).toBe(true);
  });
});

describe("token roles are isolated", () => {
  it("a voice token is refused (forbidden) on every agent call, room and surfaces", async () => {
    const w = await setup();
    const v = w.voiceToken;
    const forbidden = { ok: false, error: "forbidden" };
    expect(await w.room.command(v, "status", {})).toMatchObject(forbidden);
    expect(await w.room.command(v, "say", { text: "hi" })).toMatchObject(forbidden);
    expect(await w.room.talk(v, {})).toMatchObject(forbidden);
    expect(await w.room.ask(v, { options: ["ok"] })).toMatchObject(forbidden);
    expect(await w.room.job(v, `${DEVICE}.j_aaaaaaaaaaaaaaaa`, 0)).toMatchObject(forbidden);
    expect(await w.room.events(v, { wait_ms: 0 })).toMatchObject(forbidden);
    expect(await w.room.webhook(v, { action: "get" })).toMatchObject(forbidden);
    expect(await w.room.updates(v)).toMatchObject(forbidden);
    expect(await w.room.update(v, {})).toMatchObject(forbidden);
    const auth = { authorization: `Bearer ${v}` };
    expect((await rest(w.backend, "POST", `/v1/devices/${DEVICE}/cmd`, { name: "status" }, auth)).status).toBe(403);
    expect((await rest(w.backend, "POST", `/v1/devices/${DEVICE}/talk`, {}, auth)).status).toBe(403);
    expect((await rest(w.backend, "GET", `/v1/devices/${DEVICE}/events`, undefined, auth)).status).toBe(403);
    expect((await rest(w.backend, "GET", "/v1/webhook", undefined, auth)).status).toBe(403);
    const mcp = await handleMcp(
      new Request("https://relay.test/mcp", {
        method: "POST",
        headers: { "content-type": "application/json", ...auth },
        body: JSON.stringify({ jsonrpc: "2.0", id: 1, method: "tools/call", params: { name: "molty_status", arguments: {} } }),
      }),
      w.backend,
    );
    const res = ((await mcp.json()) as any).result;
    expect(res.isError).toBe(true);
    expect(res.structuredContent).toMatchObject({ ok: false, error: "forbidden" });
    // the agent token still works
    expect((await w.room.command(w.token, "status", {})).ok).toBe(true);
  });

  it("an agent token is refused on the voice socket; garbage is unauthorized", async () => {
    const w = await setup({ box: false });
    expect(await w.room.voiceAuth(w.token)).toMatchObject({ ok: false, error: "forbidden" });
    expect(await w.room.voiceAuth(`${DEVICE}.${"A".repeat(43)}`)).toMatchObject({ ok: false, error: "unauthorized" });
    expect((await w.room.voiceAuth(w.voiceToken)).ok).toBe(true);
  });

  it("revoking a voice token works (DELETE /v1/token) and closes its socket; the box no longer serves", async () => {
    const w = await setup();
    const r = await rest(w.backend, "DELETE", "/v1/token", undefined, { authorization: `Bearer ${w.voiceToken}` });
    expect(r.body).toMatchObject({ ok: true, result: { revoked: true } });
    expect(w.box!.sock.closed?.code).toBe(4401);
    expect(await w.room.voiceAuth(w.voiceToken)).toMatchObject({ ok: false, error: "unauthorized" });
    const say = await w.room.command(w.token, "status", {});
    expect(say.ok && (say.result.relay as any).voice).toBe("fake");
  });

  it("a voice socket whose token was revoked meanwhile cannot greet", async () => {
    const w = await setup({ box: false });
    const auth = await w.room.voiceAuth(w.voiceToken);
    if (!auth.ok) throw new Error();
    const sock = w.host.open();
    w.room.voiceOpen(sock, auth.result.hash);
    for (const [k, t] of await w.kv.list<any>("tok:")) if (t.role === "voice") await w.kv.delete(k);
    await box(w.room, sock).hello();
    expect(sock.closed?.code).toBe(4401);
  });
});

describe("voice socket", () => {
  it("is invisible to the device side: no device command goes to it, a new device socket does not close it", async () => {
    const w = await setup();
    expect((await w.room.command(w.token, "mood", { mood: "happy" })).ok).toBe(true);
    expect(w.box!.control.filter((f) => f.t === "cmd")).toHaveLength(0);
    // a burst of unauthenticated device sockets: the room keeps at most 4 waiting, oldest first out -- never the box
    for (let i = 0; i < 6; i++) w.room.socketOpen(w.host.open());
    expect(w.box!.sock.closed).toBeUndefined();
    expect(w.box!.control[0]).toEqual({ t: "voice.welcome", device_id: DEVICE, mode: "auto", voice: null });
  });

  it("a new box connection replaces the old one (4409); the old one's in-flight say fails at once", async () => {
    const w = await setup({ box: { silent: true } });
    const started = await w.room.command(w.token, "say", { text: "Hello there." });
    if (!started.ok) throw new Error();
    await until(() => w.box!.control.some((f) => f.t === "tts"));
    const t0 = Date.now();
    await openBox(w.room, w.host, w.voiceToken);
    expect(w.box!.sock.closed?.code).toBe(4409);
    const out = await w.room.job(w.token, started.result.job as string, 2000);
    expect(out).toMatchObject({ ok: true, result: { status: "failed", error: "voice_box_offline" } });
    expect(Date.now() - t0).toBeLessThan(500);
  });

  describe("a closing socket the host still lists (Cloudflare getWebSockets() includes CLOSING ones)", () => {
    it("after a replacement, the NEW box serves; the old one is marked retired in its own metadata", async () => {
      const w = await setup({ keepClosing: true });
      const old = w.box!;
      const fresh = await openBox(w.room, w.host, w.voiceToken);
      expect(old.sock.closed?.code).toBe(4409);
      expect(w.host.sockets()).toContain(old.sock); // still listed, as on Cloudflare
      expect(old.sock.meta?.retired).toBe(true); // in the serialized attachment: survives hibernation
      const out = await w.room.talk(w.token, { say: "Hi there." });
      expect(out).toMatchObject({ ok: true, result: { heard: "box heard 3 chunks", timings: { say: { voice: "box" }, listen: { voice: "box" } } } });
      expect(fresh.tts).toHaveLength(1);
      expect(old.control.filter((f) => f.t === "tts" || f.t === "stt.start")).toHaveLength(0);
    });

    it("a late frame from the retired socket is ignored (it cannot re-greet itself back)", async () => {
      const w = await setup({ keepClosing: true });
      const old = w.box!;
      await openBox(w.room, w.host, w.voiceToken);
      old.sock.closed = undefined; // its close has not completed yet: it can still talk
      await old.hello();
      expect(old.sock.meta?.retired).toBe(true);
      expect(old.control.filter((f) => f.t === "voice.welcome")).toHaveLength(1); // only the first, before it was replaced
    });

    it("after a revoke, the closing box is never picked: auto uses the cloud, box mode says offline", async () => {
      const w = await setup({ keepClosing: true });
      await w.room.revoke(w.voiceToken);
      expect(w.host.sockets()).toContain(w.box!.sock);
      const out = await w.room.talk(w.token, { say: "Hi." });
      expect(out).toMatchObject({ ok: true, result: { timings: { say: { voice: "fake" }, listen: { voice: "fake" } } } });
      await w.room.voiceSettings(w.link, { action: "set", mode: "box" });
      expect(await w.room.talk(w.token, {})).toMatchObject({ ok: false, error: "voice_box_offline" });
      expect((await w.room.voiceSettings(w.link, { action: "get" })).ok && (await w.room.voiceSettings(w.link, { action: "get" }) as any).result.box.connected).toBe(false);
    });

    it("after the owner unpairs, the closing box is not shown as connected nor picked", async () => {
      const w = await setup({ keepClosing: true });
      const un = await w.room.voiceSettings(w.link, { action: "unpair" });
      expect(un).toMatchObject({ ok: true, result: { serving: { tts: "fake", stt: "fake" }, box: { connected: false } } });
    });
  });

  it("answers ping with pong", async () => {
    const w = await setup();
    await w.room.socketMessage(w.box!.sock, '{"t":"ping"}');
    expect(w.box!.sock.last("pong")).toEqual({ t: "pong" });
  });
});

describe("provider selection (voice_mode)", () => {
  it("auto + box connected: TTS and STT go to the box; timings say so; the agent's voice passes through", async () => {
    const w = await setup();
    const out = await w.room.talk(w.token, { say: "Hi there.", voice: "b.wav" });
    expect(out).toMatchObject({ ok: true, result: { status: "done", heard: "box heard 3 chunks", timings: { say: { voice: "box" }, listen: { voice: "box" } } } });
    expect(w.box!.tts[0]).toMatchObject({ t: "tts", text: "Hi there.", voice: "b.wav" });
    expect(w.voice.ttsCalls).toHaveLength(0);
    expect(w.voice.sttCalls).toHaveLength(0);
    const st = await w.room.command(w.token, "status", {});
    expect(st.ok && (st.result.relay as any).voice).toBe("box");
  });

  it("say / listen jobs report the provider too", async () => {
    const w = await setup();
    const say = await w.room.command(w.token, "say", { text: "One." });
    if (!say.ok) throw new Error();
    expect(await w.room.job(w.token, say.result.job as string, 2000)).toMatchObject({ ok: true, result: { status: "done", result: { timings: { voice: "box" } } } });
    const listen = await w.room.command(w.token, "listen", { ms: 2000 });
    if (!listen.ok) throw new Error();
    expect(await w.room.job(w.token, listen.result.job as string, 2000)).toMatchObject({ ok: true, result: { status: "done", result: { text: "box heard 3 chunks", timings: { voice: "box" } } } });
  });

  it("the owner's default box voice is used when the agent gives none", async () => {
    const w = await setup();
    expect((await w.room.voiceSettings(w.link, { action: "set", voice: "a.wav" })).ok).toBe(true);
    await w.room.talk(w.token, { say: "Hi.", listen: false });
    expect(w.box!.tts[0].voice).toBe("a.wav");
  });

  it("auto without a box: the cloud provider, as before", async () => {
    const w = await setup({ box: false });
    const out = await w.room.talk(w.token, { say: "Hi." });
    expect(out).toMatchObject({ ok: true, result: { heard: "hello molty", timings: { say: { voice: "fake" }, listen: { voice: "fake" } } } });
    expect(w.voice.ttsCalls).toHaveLength(1);
    expect(w.voice.sttCalls).toHaveLength(1);
  });

  it("cloud mode ignores a connected box", async () => {
    const w = await setup();
    await w.room.voiceSettings(w.link, { action: "set", mode: "cloud" });
    const out = await w.room.talk(w.token, { say: "Hi." });
    expect(out).toMatchObject({ ok: true, result: { timings: { say: { voice: "fake" }, listen: { voice: "fake" } } } });
    expect(w.box!.control.filter((f) => f.t === "tts" || f.t === "stt.start")).toHaveLength(0);
  });

  it("box mode without a box: voice_box_offline (REST 503), before anything reaches the device", async () => {
    const w = await setup({ box: false });
    await w.room.voiceSettings(w.link, { action: "set", mode: "box" });
    expect(await w.room.talk(w.token, { say: "Hi." })).toMatchObject({ ok: false, error: "voice_box_offline" });
    expect(await w.room.talk(w.token, {})).toMatchObject({ ok: false, error: "voice_box_offline" });
    const r = await rest(w.backend, "POST", `/v1/devices/${DEVICE}/talk`, {}, { authorization: `Bearer ${w.token}` });
    expect(r.status).toBe(503);
    expect(w.dev.log.filter((l) => l.name === "say" || l.name === "listen")).toHaveLength(0);
    expect(w.voice.ttsCalls).toHaveLength(0);
  });

  it("a box that offers only TTS: auto transcribes in the cloud", async () => {
    const w = await setup({ box: { stt: false } });
    const out = await w.room.talk(w.token, { say: "Hi." });
    expect(out).toMatchObject({ ok: true, result: { heard: "hello molty", timings: { say: { voice: "box" }, listen: { voice: "fake" } } } });
  });
});

describe("box drops mid-job", () => {
  it("auto: the box drops during the listen -> cloud STT on the audio already buffered", async () => {
    const w = await setup({ box: { silent: true } });
    const p = w.room.talk(w.token, { listen_ms: 3000 });
    await until(() => (w.box!.control.some((f) => f.t === "stt.start")));
    await new Promise((r) => setTimeout(r, 50));
    w.box!.sock.close(1006, "gone");
    const out = await p;
    expect(out).toMatchObject({ ok: true, result: { heard: "hello molty", timings: { listen: { voice: "fake", voice_fallback: "voice_box_offline" } } } });
    expect(w.voice.sttCalls).toHaveLength(1);
    expect(w.voice.sttCalls[0].length).toBe(3 * 1600); // the whole clip, buffered by the relay
  });

  it("auto: a box that never sends its final text -> cloud after boxSttFinalMs, not the 30 s STT timeout", async () => {
    const w = await setup({ box: { silent: true } });
    const t0 = Date.now();
    const out = await w.room.talk(w.token, {});
    expect(out).toMatchObject({ ok: true, result: { heard: "hello molty", timings: { listen: { voice: "fake", voice_fallback: "stt_timeout" } } } });
    expect(Date.now() - t0).toBeLessThan(2000);
  });

  it("box mode: the box drops during the listen -> the job fails voice_box_offline", async () => {
    const w = await setup({ box: { silent: true } });
    await w.room.voiceSettings(w.link, { action: "set", mode: "box" });
    const p = w.room.talk(w.token, {});
    await until(() => w.box!.control.some((f) => f.t === "stt.start"));
    w.box!.sock.close(1006, "gone");
    expect(await p).toMatchObject({ ok: false, error: "voice_box_offline" });
    expect(w.voice.sttCalls).toHaveLength(0);
  });

  it("the box drops before its TTS audio: the say fails cleanly and quickly (no hang, device untouched)", async () => {
    const w = await setup({ box: { silent: true } });
    const p = w.room.talk(w.token, { say: "Hello." });
    await until(() => w.box!.tts.length === 0 && w.box!.control.some((f) => f.t === "tts"));
    const t0 = Date.now();
    w.box!.sock.close(1006, "gone");
    expect(await p).toMatchObject({ ok: false, error: "voice_box_offline" });
    expect(Date.now() - t0).toBeLessThan(300);
    expect(w.dev.log.filter((l) => l.name === "say")).toHaveLength(0);
  });
});

describe("streaming STT", () => {
  it("every device chunk reaches the box as it arrives, before the listen ends; the transcript follows the end", async () => {
    const w = await setup();
    const out = await w.room.talk(w.token, {});
    expect(out).toMatchObject({ ok: true, result: { heard: "box heard 3 chunks", reason: "silence" } });
    const id = Object.keys(w.box!.got)[0];
    const chunks = w.box!.got[id];
    expect(chunks.map((c) => c.bytes)).toEqual([3200, 3200, 3200]);
    const end = w.dev.log.find((l) => l.kind === "end")!.at;
    // streamed: the first chunks were at the box well before the device's end frame
    expect(chunks[0].at).toBeLessThan(end - 40);
    expect(chunks[1].at).toBeLessThan(end - 10);
    // and stt.end follows the end frame
    expect(w.box!.control.findIndex((f) => f.t === "stt.end")).toBeGreaterThan(w.box!.control.findIndex((f) => f.t === "stt.start"));
  });

  it("no speech: the stream is cancelled, no transcript is asked for", async () => {
    const w = await setup();
    // this device's VAD hears nothing: rewrite its end frame
    const orig = w.room.socketMessage.bind(w.room);
    w.room.socketMessage = (s: any, d: any) => {
      if (s === w.sock && d instanceof Uint8Array) {
        const f = decodeAudioFrame(d);
        if (f?.header.end) d = new TextEncoder().encode(JSON.stringify({ ...f.header, reason: "no_speech", speech_ms: 0 }));
        else if (f) return Promise.resolve();
      }
      return orig(s, d);
    };
    const out = await w.room.talk(w.token, {});
    expect(out).toMatchObject({ ok: true, result: { heard: "", reason: "no_speech" } });
    expect(w.box!.control.some((f) => f.t === "stt.cancel")).toBe(true);
    expect(w.box!.control.some((f) => f.t === "stt.end")).toBe(false);
  });
});

describe("owner voice settings (console)", () => {
  it("needs the device's current link code", async () => {
    const w = await setup();
    expect(await w.room.voiceSettings(LINK, { action: "get" })).toMatchObject({ ok: false, error: "unauthorized" }); // rotated away
    expect(await w.room.voiceSettings("garbage", { action: "get" })).toMatchObject({ ok: false, error: "unauthorized" });
    const r = await rest(w.backend, "GET", `/v1/devices/${DEVICE}/voice`, undefined, { "x-molty-link-code": "ZZZZ2345" });
    expect(r.status).toBe(401);
    expect(r.headers.get("access-control-allow-origin")).toBe("*");
  });

  it("reads status, sets mode and voice, unpairs the box", async () => {
    const w = await setup();
    await w.room.talk(w.token, { say: "Hi." });
    const h = { "x-molty-link-code": w.link };
    const get = await rest(w.backend, "GET", `/v1/devices/${DEVICE}/voice`, undefined, h);
    expect(get.body).toMatchObject({
      ok: true,
      result: {
        mode: "auto",
        voice: null,
        cloud: "fake",
        serving: { tts: "box", stt: "box" },
        box: { paired: true, name: "gpu box", connected: true, reported_name: "gpu box", version: "0.1.0", voices: ["a.wav", "b.wav"], stt: true, tts: true, last: { stt_ms: expect.any(Number), tts_first_audio_ms: expect.any(Number) } },
      },
    });
    expect((await rest(w.backend, "POST", `/v1/devices/${DEVICE}/voice`, { mode: "loud" }, h)).body).toMatchObject({ ok: false, error: "invalid_args" });
    const set = await rest(w.backend, "POST", `/v1/devices/${DEVICE}/voice`, { mode: "box", voice: "b.wav" }, h);
    expect(set.body.result).toMatchObject({ mode: "box", voice: "b.wav" });
    expect((await rest(w.backend, "POST", `/v1/devices/${DEVICE}/voice`, { voice: null }, h)).body.result).toMatchObject({ mode: "box", voice: null });
    const un = await rest(w.backend, "DELETE", `/v1/devices/${DEVICE}/voice`, undefined, h);
    expect(un.body.result).toMatchObject({ unpaired: 1, serving: { tts: "voice_box_offline" }, box: { paired: false, connected: false } });
    expect(w.box!.sock.closed?.code).toBe(4401);
    expect(await w.room.voiceAuth(w.voiceToken)).toMatchObject({ ok: false, error: "unauthorized" });
    const pre = await rest(w.backend, "OPTIONS", `/v1/devices/${DEVICE}/voice`);
    expect(pre.status).toBe(204);
    expect(pre.headers.get("access-control-allow-headers")).toContain("x-molty-link-code");
  });

  it("forgetting the device drops the box and its settings", async () => {
    const w = await setup();
    await w.room.voiceSettings(w.link, { action: "set", mode: "box" });
    await w.room.socketMessage(w.sock, JSON.stringify({ t: "forget" }));
    expect(w.box!.sock.closed?.code).toBe(4401);
    expect(w.kv.data.has("voicecfg")).toBe(false);
  });
});
