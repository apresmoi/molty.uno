// Fast conversation: molty_talk (say + listen + transcript in one call),
// device end-of-speech (reason / speech_ms on the last mic frame), automatic
// moods, wait:true on say/listen, and streamed TTS (first audio before the
// whole utterance is synthesised).
import { describe, expect, it } from "vitest";
import type { RelayBackend } from "../src/core/api.js";
import { encodeWav } from "../src/core/audio.js";
import { decodeAudioFrame, encodeAudioFrame } from "../src/core/protocol.js";
import type { DeviceRoom } from "../src/core/room.js";
import { PcmFeed, splitSpeech, synthesize } from "../src/core/speech.js";
import type { TtsOptions } from "../src/voice/types.js";
import { handleMcp } from "../src/surfaces/mcp.js";
import { handleRest } from "../src/surfaces/rest.js";
import { WorkersAiVoice } from "../src/voice/workers-ai.js";
import { SKILL_VERSION } from "../src/core/version.js";
import { DEVICE, type FakeSocket, FakeVoice, LINK, connect, makeRoom } from "./helpers.js";

const FAST = { cmdTimeoutMs: 300, pairWindowMs: 400, sayChunkSamples: 1600, sayLeadFrames: 2, listenGraceMs: 200, sayDoneGraceMs: 300 };

interface Mic {
  /** ms between the say end frame and the device's say.done (default 5); null: never (playback failed). */
  sayDoneMs?: number | null;
  /** Samples streamed up before the end frame. */
  samples?: number;
  /** End-frame extras (firmware VAD): reason, speech_ms. */
  end?: Record<string, unknown>;
  /** The end-frame header verbatim as the firmware writes it (its "s"/"seq" are rewritten to this job's). */
  endRaw?: string;
  /** The device refuses a cmd with this error code (undefined: it accepts). */
  cmdError?: (name: string, args: any) => string | undefined;
}

/** A device that answers cmds, "plays" say streams (say.done when the end frame arrives) and streams mic audio for listen. */
function device(room: DeviceRoom, sock: FakeSocket, mic: Mic = {}) {
  const log: Array<{ at: number; kind: string; name?: string; args?: any; end?: boolean; bytes?: number }> = [];
  const origText = sock.sendText.bind(sock);
  const origBin = sock.sendBinary.bind(sock);
  sock.sendText = (t: string) => {
    origText(t);
    const f = JSON.parse(t);
    if (f.t === "cmd") {
      log.push({ at: Date.now(), kind: "cmd", name: f.name, args: f.args });
      const err = mic.cmdError?.(f.name, f.args);
      const res = err ? { t: "res", id: f.id, ok: false, error: err } : { t: "res", id: f.id, ok: true, result: {} };
      setTimeout(() => void room.socketMessage(sock, JSON.stringify(res)), 1);
      if (f.name === "listen") {
        setTimeout(() => {
          const n = mic.samples ?? 3200;
          const pcm = new Uint8Array(new Int16Array(n).fill(8000).buffer);
          if (n) void room.socketMessage(sock, encodeAudioFrame({ s: f.args.job, seq: 0, kind: "pcm16", rate: 16000, end: false }, pcm));
          const head = mic.endRaw
            ? mic.endRaw.replace(/"s":"[^"]*","seq":\d+/, `"s":"${f.args.job}","seq":${n ? 1 : 0}`)
            : JSON.stringify({ s: f.args.job, seq: n ? 1 : 0, kind: "pcm16", rate: 16000, end: true, ...(mic.end ?? {}) });
          void room.socketMessage(sock, new TextEncoder().encode(head));
        }, 20);
      }
    }
    if (f.t === "pair.request") setTimeout(() => void room.socketMessage(sock, JSON.stringify({ t: "pair.result", req: f.req, ok: true, link_code: "NEWC2345" })), 2);
  };
  sock.sendBinary = (b: Uint8Array) => {
    origBin(b);
    const fr = decodeAudioFrame(b)!;
    log.push({ at: Date.now(), kind: "frame", end: fr.header.end, bytes: fr.payload.byteLength });
    if (fr.header.end && mic.sayDoneMs !== null)
      setTimeout(() => {
        log.push({ at: Date.now(), kind: "say.done" });
        void room.socketMessage(sock, JSON.stringify({ t: "evt", name: "say.done", data: { job: fr.header.s } }));
      }, mic.sayDoneMs ?? 5);
  };
  const moods = () => log.filter((l) => l.kind === "cmd" && l.name === "mood").map((l) => l.args);
  return { log, moods };
}

async function setup(mic: Mic = {}, opts: { voice?: FakeVoice; timings?: Record<string, number>; caps?: string[] } = {}) {
  const ctx = makeRoom({ timings: { ...FAST, ...opts.timings }, voice: opts.voice });
  const sock = await connect(ctx.room, ctx.host, undefined, LINK, opts.caps ? { caps: opts.caps } : {});
  const dev = device(ctx.room, sock, mic);
  const out = await ctx.room.pair({ device_id: DEVICE, link_code: LINK, agent_name: "talker" });
  if (!out.ok || out.result.status !== "approved") throw new Error(JSON.stringify(out));
  const backend: RelayBackend = { device: () => ctx.room };
  return { ...ctx, sock, dev, token: out.result.agent_token, backend };
}

const until = async (cond: () => boolean, ms = 1000) => {
  const end = Date.now() + ms;
  while (!cond() && Date.now() < end) await new Promise((r) => setTimeout(r, 5));
};

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

describe("molty_talk", () => {
  it("happy path: speaks, listens, returns the transcript in ONE call; moods are automatic", async () => {
    const w = await setup({ samples: 3200, end: { reason: "silence", speech_ms: 180 } });
    w.voice.ttsSamples = 1600;
    const r = await mcp(w.backend, w.token, "molty_talk", { say: "Hi there." });
    expect(r.isError).toBe(false);
    expect(r.structuredContent).toMatchObject({
      ok: true,
      status: "done",
      job: expect.stringMatching(/^molty-k7m2q9xa4c\.j_/),
      said: "Hi there.",
      heard: "hello molty",
      reason: "silence",
      speech_ms: 180,
      ms: { tts_first_audio: expect.any(Number), speech: expect.any(Number), listen: expect.any(Number), stt: expect.any(Number), total: expect.any(Number) },
      timings: { say: expect.any(Object), listen: expect.any(Object) },
    });
    // device saw: say, its audio, then listen (after say.done), then the automatic thinking mood
    const order = w.dev.log.filter((l) => l.kind === "cmd").map((l) => l.name);
    expect(order.slice(0, 2)).toEqual(["say", "listen"]);
    const sayEnd = w.dev.log.findIndex((l) => l.kind === "frame" && l.end);
    const listenAt = w.dev.log.findIndex((l) => l.kind === "cmd" && l.name === "listen");
    expect(sayEnd).toBeGreaterThan(-1);
    expect(listenAt).toBeGreaterThan(sayEnd);
    expect(w.dev.moods()[0]).toEqual({ mood: "thinking", hold_s: 15 });
    expect(w.voice.sttCalls).toHaveLength(1);
  });

  it("ring_color: resolved to #rrggbb for talk and listen, sent only when given, invalid -> invalid_args", async () => {
    const w = await setup({ end: { reason: "silence", speech_ms: 100 } });
    expect((await w.room.talk(w.token, { ring_color: "Violet" })).ok).toBe(true);
    expect(w.dev.log.find((l) => l.name === "listen")!.args.ring_color).toBe("#b197fc");
    expect((await w.room.talk(w.token, {})).ok).toBe(true);
    expect(w.dev.log.filter((l) => l.name === "listen").at(-1)!.args).not.toHaveProperty("ring_color");
    const job = ((await w.room.command(w.token, "listen", { ms: 500, ring_color: "#ABCDEF" })) as any).result.job;
    await w.room.job(w.token, job, 2000);
    expect(w.dev.log.filter((l) => l.name === "listen").at(-1)!.args).toEqual({ job, ms: 500, rate: 16000, ring_color: "#abcdef" });
    for (const ring_color of ["orange", "#abc", "abcdef", 5, null]) {
      expect(await w.room.talk(w.token, { ring_color }), String(ring_color)).toMatchObject({ ok: false, error: "invalid_args" });
      expect(await w.room.command(w.token, "listen", { ring_color }), String(ring_color)).toMatchObject({ ok: false, error: "invalid_args" });
    }
    expect(await w.room.talk(w.token, { ring_color: "orange" })).toMatchObject({ message: expect.stringMatching(/ring_color must be one of green, red/) });
  });

  it("passes the device VAD knobs through to listen, validated to the firmware's ranges", async () => {
    const w = await setup({ end: { reason: "silence", speech_ms: 100 } });
    const ok = await w.room.talk(w.token, { end_silence_ms: 500, no_speech_ms: 3000, vad: true, listen_ms: 8000 });
    expect(ok.ok).toBe(true);
    expect(w.dev.log.find((l) => l.name === "listen")!.args).toMatchObject({ ms: 8000, end_silence_ms: 500, no_speech_ms: 3000, vad: true });
    for (const bad of [{ end_silence_ms: 299 }, { end_silence_ms: 3001 }, { no_speech_ms: 999 }, { no_speech_ms: 10001 }, { vad: "yes" }, { listen_ms: 10001 }, { listen: false }, { bogus: 1 }]) {
      expect(await w.room.talk(w.token, bad), JSON.stringify(bad)).toMatchObject({ ok: false, error: "invalid_args" });
    }
    // plain listen: same knobs, same ranges; absent knobs are not sent (older firmware sees the old cmd)
    const job = ((await w.room.command(w.token, "listen", { ms: 500, end_silence_ms: 300, no_speech_ms: 10000, vad: false })) as any).result.job;
    await w.room.job(w.token, job, 2000);
    expect(w.dev.log.filter((l) => l.name === "listen").at(-1)!.args).toEqual({ job, ms: 500, rate: 16000, end_silence_ms: 300, no_speech_ms: 10000, vad: false });
    expect(await w.room.command(w.token, "listen", { end_silence_ms: 5000 })).toMatchObject({ ok: false, error: "invalid_args" });
    const job2 = ((await w.room.command(w.token, "listen", { ms: 500 })) as any).result.job;
    await w.room.job(w.token, job2, 2000);
    expect(w.dev.log.filter((l) => l.name === "listen").at(-1)!.args).toEqual({ job: job2, ms: 500, rate: 16000 });
  });

  it("no_speech: heard is empty, STT is never called, no thinking mood", async () => {
    const w = await setup({ samples: 1600, end: { reason: "no_speech", speech_ms: 0 } });
    const r = await w.room.talk(w.token, { listen_ms: 4000 });
    expect(r).toMatchObject({ ok: true, result: { status: "done", heard: "", reason: "no_speech", speech_ms: 0, ms: { stt: 0 } } });
    expect(w.voice.sttCalls).toHaveLength(0);
    await new Promise((res) => setTimeout(res, 20));
    expect(w.dev.moods()).toEqual([]);
  });

  it("no_speech with zero audio frames still answers heard:'' (not no_audio)", async () => {
    const w = await setup({ samples: 0, end: { reason: "no_speech", speech_ms: 0 } });
    expect(await w.room.talk(w.token, {})).toMatchObject({ ok: true, result: { heard: "", reason: "no_speech" } });
    expect(w.voice.sttCalls).toHaveLength(0);
  });

  // The firmware's exact vad:false end-frame header (firmware/test/host/link_audio_pair_test.c
  // asserts this string): VAD off measured nothing, so there is no speech_ms.
  const FW_VAD_OFF_END = '{"s":"listen1","seq":16,"kind":"pcm16","rate":16000,"end":true,"reason":"max"}';

  it("vad:false (firmware end frame: reason max, no speech_ms) is transcribed", async () => {
    const w = await setup({ samples: 3200, endRaw: FW_VAD_OFF_END });
    const r = await w.room.talk(w.token, { vad: false, listen_ms: 5000 });
    expect(r).toMatchObject({ ok: true, result: { heard: "hello molty", reason: "max" } });
    expect((r as any).result.speech_ms).toBeUndefined();
    expect(w.voice.sttCalls).toHaveLength(1);
    expect(w.dev.moods()[0]).toEqual({ mood: "thinking", hold_s: 15 });
  });

  it("only reason no_speech skips STT: speech_ms 0 alone never does (older vad:false firmware sent it)", async () => {
    const w = await setup({ samples: 1600, end: { reason: "max", speech_ms: 0 } });
    expect(await w.room.talk(w.token, {})).toMatchObject({ ok: true, result: { heard: "hello molty", reason: "max", speech_ms: 0 } });
    expect(w.voice.sttCalls).toHaveLength(1);
  });

  it("older firmware (no reason on the end frame): STT runs as before, reason derived from the clip", async () => {
    const w = await setup({ samples: 3200 }); // 200 ms clip, no reason
    const full = await w.room.talk(w.token, { listen_ms: 200 });
    expect(full).toMatchObject({ ok: true, result: { heard: "hello molty", reason: "max" } });
    expect((full as any).result.speech_ms).toBeUndefined();
    const early = await w.room.talk(w.token, { listen_ms: 5000 });
    expect(early).toMatchObject({ ok: true, result: { heard: "hello molty", reason: "ended" } });
    w.voice.sttText = "";
    expect(await w.room.talk(w.token, { listen_ms: 5000 })).toMatchObject({ ok: true, result: { heard: "", reason: "no_speech" } });
    expect(w.voice.sttCalls).toHaveLength(3);
  });

  it("automatic thinking: sent at end of speech (before STT finishes), re-asserted after STT; idle when nothing was heard", async () => {
    const w = await setup({ samples: 3200, end: { reason: "silence", speech_ms: 150 } });
    let sttStarted = 0;
    const stt = w.voice.stt.bind(w.voice);
    w.voice.stt = async (pcm) => {
      sttStarted = Date.now();
      await new Promise((r) => setTimeout(r, 60));
      return stt(pcm);
    };
    const r = await w.room.talk(w.token, {});
    expect(r.ok).toBe(true);
    const moods = w.dev.log.filter((l) => l.name === "mood");
    expect(moods.map((m) => m.args)).toEqual([{ mood: "thinking", hold_s: 15 }, { mood: "thinking", hold_s: 15 }]);
    expect(moods[0].at).toBeLessThanOrEqual(sttStarted + 5); // first one does not wait for STT
    w.voice.stt = async () => "";
    await w.room.talk(w.token, {});
    expect(w.dev.moods().slice(2)).toEqual([{ mood: "thinking", hold_s: 15 }, { mood: "idle" }]);
    // explicit mood still works
    expect(await w.room.command(w.token, "mood", { mood: "happy" })).toEqual({ ok: true, result: {} });
    expect(w.dev.moods().at(-1)).toEqual({ mood: "happy" });
  });

  it("waits for the device's say.done (bounded) before opening the mic, so it never records its own voice", async () => {
    // playback is 100 ms; the device reports done 200 ms after the last frame (< 300 ms grace)
    const w = await setup({ sayDoneMs: 200, end: { reason: "silence", speech_ms: 90 } });
    w.voice.ttsSamples = 1600;
    expect(await w.room.talk(w.token, { say: "Hi." })).toMatchObject({ ok: true, result: { status: "done" } });
    const done = w.dev.log.find((l) => l.kind === "say.done")!.at;
    const listen = w.dev.log.find((l) => l.name === "listen")!.at;
    expect(listen).toBeGreaterThanOrEqual(done);
  });

  it("no say.done: the turn fails with playback_timeout and the mic never opens", async () => {
    const w = await setup({ sayDoneMs: null, end: { reason: "silence", speech_ms: 90 } });
    w.voice.ttsSamples = 1600; // 100 ms of speech; + 300 ms grace
    const t0 = Date.now();
    const r = await w.room.talk(w.token, { say: "Hi." });
    expect(r).toMatchObject({ ok: false, error: "playback_timeout" });
    expect(Date.now() - t0).toBeLessThan(2000);
    expect(w.dev.log.filter((l) => l.kind === "cmd").map((l) => l.name)).toEqual(["say"]);
    // REST: a clear gateway timeout, not a success
    const rr = await rest(w.backend, w.token, `/v1/devices/${DEVICE}/talk`, { say: "Hi." });
    expect(rr).toMatchObject({ status: 504, body: { ok: false, error: "playback_timeout" } });
    expect(w.dev.log.some((l) => l.name === "listen")).toBe(false);
  });

  it("an explicit mood during STT stands: the automatic thinking/idle does not overwrite it", async () => {
    const w = await setup({ samples: 3200, end: { reason: "silence", speech_ms: 150 } });
    const stt = w.voice.stt.bind(w.voice);
    w.voice.stt = async (pcm) => {
      await new Promise((r) => setTimeout(r, 80));
      return stt(pcm);
    };
    const turn = w.room.talk(w.token, {});
    await until(() => w.dev.moods().length > 0);
    expect(w.dev.moods()).toEqual([{ mood: "thinking", hold_s: 15 }]);
    // another paired agent, even: the device's mood belongs to whoever set it last
    expect(await w.room.command(w.token, "mood", { mood: "happy", hold_s: 60 })).toMatchObject({ ok: true });
    expect(await turn).toMatchObject({ ok: true, result: { heard: "hello molty" } });
    await new Promise((r) => setTimeout(r, 20));
    expect(w.dev.moods()).toEqual([{ mood: "thinking", hold_s: 15 }, { mood: "happy", hold_s: 60 }]);
    // nothing heard + explicit mood: no automatic idle either
    w.voice.stt = async () => {
      await new Promise((r) => setTimeout(r, 80));
      return "";
    };
    const turn2 = w.room.talk(w.token, {});
    await until(() => w.dev.moods().length > 2);
    expect(await w.room.command(w.token, "mood", { mood: "surprised" })).toMatchObject({ ok: true });
    await turn2;
    await new Promise((r) => setTimeout(r, 20));
    expect(w.dev.moods().slice(2)).toEqual([{ mood: "thinking", hold_s: 15 }, { mood: "surprised" }]);
    // the next listen starts clean: automatic moods are back
    w.voice.stt = stt;
    await w.room.talk(w.token, {});
    await new Promise((r) => setTimeout(r, 20));
    expect(w.dev.moods().slice(4)).toEqual([{ mood: "thinking", hold_s: 15 }, { mood: "thinking", hold_s: 15 }]);
  });

  it("an ask shown after a listen began is never cancelled by that listen's automatic moods", async () => {
    const w = await setup({ samples: 3200, end: { reason: "silence", speech_ms: 150 } }, { caps: ["say", "listen", "ask"] });
    const stt = w.voice.stt.bind(w.voice);
    w.voice.stt = async (pcm) => {
      await new Promise((r) => setTimeout(r, 80));
      return stt(pcm);
    };
    const asks = () => w.dev.log.filter((l) => l.name === "ask");
    const answer = (job: string) => w.room.socketMessage(w.sock, JSON.stringify({ t: "evt", name: "answer", data: { job, index: 0, label: "ok" } }));
    // the question goes out during STT: the re-asserted thinking (any mood cancels an ask) is not sent
    const turn = w.room.talk(w.token, {});
    await until(() => w.dev.moods().length > 0);
    const q = w.room.ask(w.token, { options: ["ok"] });
    await until(() => asks().length === 1);
    expect(await turn).toMatchObject({ ok: true, result: { heard: "hello molty" } });
    await new Promise((r) => setTimeout(r, 20));
    expect(w.dev.moods()).toEqual([{ mood: "thinking", hold_s: 15 }]);
    const after = w.dev.log.filter((l) => l.kind === "cmd").map((l) => l.name);
    expect(after.slice(after.indexOf("ask"))).toEqual(["ask"]);
    await answer(asks()[0].args.job);
    expect(await q).toMatchObject({ ok: true, result: { answer: "ok" } });
    // the question goes out before the end of speech: neither automatic mood is sent
    const turn2 = w.room.talk(w.token, {});
    await until(() => w.dev.log.filter((l) => l.name === "listen").length === 2);
    const q2 = w.room.ask(w.token, { options: ["ok"] });
    expect(await turn2).toMatchObject({ ok: true, result: { heard: "hello molty" } });
    await new Promise((r) => setTimeout(r, 20));
    expect(w.dev.moods()).toEqual([{ mood: "thinking", hold_s: 15 }]);
    await answer(asks()[1].args.job);
    expect(await q2).toMatchObject({ ok: true });
    // a listen that begins after the question: its automatic moods are back
    await w.room.talk(w.token, {});
    await new Promise((r) => setTimeout(r, 20));
    expect(w.dev.moods().slice(1)).toEqual([{ mood: "thinking", hold_s: 15 }, { mood: "thinking", hold_s: 15 }]);
  });

  it("a mood the device refuses does not count as explicit: the automatic thinking still lands", async () => {
    const w = await setup({
      samples: 3200,
      end: { reason: "silence", speech_ms: 150 },
      cmdError: (name, args) => (name === "mood" && args.mood === "happy" ? "invalid_state" : undefined),
    });
    const stt = w.voice.stt.bind(w.voice);
    w.voice.stt = async (pcm) => {
      await new Promise((r) => setTimeout(r, 80));
      return stt(pcm);
    };
    const turn = w.room.talk(w.token, {});
    await until(() => w.dev.moods().length > 0);
    expect(await w.room.command(w.token, "mood", { mood: "happy", hold_s: 60 })).toMatchObject({ ok: false });
    expect(await turn).toMatchObject({ ok: true, result: { heard: "hello molty" } });
    await new Promise((r) => setTimeout(r, 20));
    expect(w.dev.moods()).toEqual([{ mood: "thinking", hold_s: 15 }, { mood: "happy", hold_s: 60 }, { mood: "thinking", hold_s: 15 }]);
  });

  it("an explicit mood still awaiting the device's answer is not overtaken by an automatic one", async () => {
    const w = await setup({ samples: 3200, end: { reason: "silence", speech_ms: 150 } });
    const stt = w.voice.stt.bind(w.voice);
    let release!: () => void;
    const held = new Promise<void>((r) => (release = r));
    w.voice.stt = async (pcm) => {
      await held;
      return stt(pcm);
    };
    // hold the device's answer to the explicit mood until after STT finished
    const orig = w.sock.sendText.bind(w.sock);
    let parked: string | undefined;
    w.sock.sendText = (t: string) => {
      const f = JSON.parse(t);
      if (f.t === "cmd" && f.name === "mood" && f.args.mood === "surprised") {
        parked = f.id;
        w.dev.log.push({ at: Date.now(), kind: "cmd", name: f.name, args: f.args });
        return;
      }
      orig(t);
    };
    const turn = w.room.talk(w.token, {});
    await until(() => w.dev.moods().length > 0);
    const explicit = w.room.command(w.token, "mood", { mood: "surprised" });
    await until(() => parked !== undefined);
    release();
    expect(await turn).toMatchObject({ ok: true });
    await new Promise((r) => setTimeout(r, 20));
    expect(w.dev.moods()).toEqual([{ mood: "thinking", hold_s: 15 }, { mood: "surprised" }]);
    void w.room.socketMessage(w.sock, JSON.stringify({ t: "res", id: parked, ok: true, result: {} }));
    expect(await explicit).toMatchObject({ ok: true });
  });

  it("over budget: returns {job, status:'pending'} and molty_wait finishes it", async () => {
    const w = await setup({ samples: 1600, end: { reason: "silence", speech_ms: 90 } }, { timings: { talkBudgetMs: 30 } });
    w.voice.ttsSamples = 16000 * 0.4; // 400 ms of speech > 30 ms budget
    const r = await mcp(w.backend, w.token, "molty_talk", { say: "This takes a while." });
    expect(r.structuredContent).toMatchObject({ ok: true, status: "pending", job: expect.any(String), hint: expect.stringMatching(/molty_wait/) });
    const done = await mcp(w.backend, w.token, "molty_wait", { job: r.structuredContent.job, timeout_s: 3 });
    expect(done.structuredContent).toMatchObject({ ok: true, status: "done", heard: "hello molty", said: "This takes a while." });
  });

  it("REST /talk: 200 done, 202 pending, errors; say-only turn; audio_busy and unsupported", async () => {
    const w = await setup({ end: { reason: "silence", speech_ms: 90 } });
    w.voice.ttsSamples = 1600;
    const done = await rest(w.backend, w.token, `/v1/devices/${DEVICE}/talk`, { say: "Bye.", listen: false });
    expect(done).toMatchObject({ status: 200, body: { ok: true, result: { status: "done", said: "Bye.", reason: "spoken" } } });
    expect(done.body.result.heard).toBeUndefined();
    expect(await rest(w.backend, w.token, `/v1/devices/${DEVICE}/talk`, { listen: "no" })).toMatchObject({ status: 400, body: { error: "invalid_args" } });
    expect(await rest(w.backend, "nope", `/v1/devices/${DEVICE}/talk`, {})).toMatchObject({ status: 401 });
    w.voice.ttsSamples = 16000;
    const busy = await w.room.command(w.token, "say", { text: "long one" });
    expect(await w.room.talk(w.token, {})).toMatchObject({ ok: false, error: "audio_busy" });
    await w.room.job(w.token, (busy as any).result.job, 3000);
    const mute = await setup({}, { caps: ["listen"] });
    expect(await mute.room.talk(mute.token, { say: "x" })).toMatchObject({ ok: false, error: "unsupported" });
    expect(await mute.room.talk(mute.token, {})).toMatchObject({ ok: true });
  });
});

describe("wait:true on say / listen", () => {
  it("MCP: molty_say / molty_listen block until done (no molty_wait)", async () => {
    const w = await setup({ end: { reason: "silence", speech_ms: 90 } });
    w.voice.ttsSamples = 1600;
    const s = await mcp(w.backend, w.token, "molty_say", { text: "hi", wait: true });
    expect(s.structuredContent).toMatchObject({ ok: true, status: "done", ms: 100, text: "hi", timings: { tts_first_byte: expect.any(Number), first_audio: expect.any(Number), total: expect.any(Number) } });
    const l = await mcp(w.backend, w.token, "molty_listen", { ms: 500, wait: true });
    expect(l.structuredContent).toMatchObject({ ok: true, status: "done", text: "hello molty", reason: "silence", speech_ms: 90, timings: { record: expect.any(Number), stt: expect.any(Number) } });
    // without wait: the old async shape
    const a = await mcp(w.backend, w.token, "molty_say", { text: "hi" });
    expect(Object.keys(a.structuredContent).sort()).toEqual(["job", "ok"]);
    await w.room.job(w.token, a.structuredContent.job, 2000);
  });

  it("REST: a failing say {wait:true} still hands over the events and skill notice it consumed", async () => {
    const w = await setup();
    w.voice.tts = async () => {
      throw new Error("upstream 500");
    };
    await w.room.socketMessage(w.sock, JSON.stringify({ t: "evt", name: "tap", data: { x: 0.5, y: 0.25 } }));
    for (const [k, v] of await w.kv.list<Record<string, unknown>>("tok:")) await w.kv.put(k, { ...v, skill_seen: 2 });
    const r = await rest(w.backend, w.token, `/v1/devices/${DEVICE}/cmd`, { name: "say", args: { text: "hi" }, wait: true });
    expect(r.status).toBe(502);
    expect(r.body).toMatchObject({ ok: false, error: "upstream 500", skill_update: { version: SKILL_VERSION } });
    expect(r.body.events.map((e: any) => e.type)).toContain("tap");
    // consistent: what was handed over is marked read / seen, and is not handed over again
    const head = await w.kv.get<number>("evseq");
    const [[hash]] = [...(await w.kv.list("tok:")).keys()].map((k) => [k.slice(4)]);
    expect(await w.kv.get<number>(`cur:${hash}`)).toBe(head);
    expect([...(await w.kv.list<any>("tok:")).values()][0].skill_seen).toBe(SKILL_VERSION);
    const again = await rest(w.backend, w.token, `/v1/devices/${DEVICE}/cmd`, { name: "imu" });
    expect(again.body.events).toBeUndefined();
    expect(again.body.skill_update).toBeUndefined();
  });

  it("REST: every failure renders the ride-alongs its outcome carries (events + skill notice), like MCP", async () => {
    const w = await setup({ cmdError: (name) => (name === "volume" ? "busy" : undefined) });
    // A backend whose failed outcomes carry already-consumed ride-alongs: the REST
    // surface must hand them over on every failure path, never drop them.
    const ride = { inbox: { events: [{ id: "e9", type: "tap", at: 1, data: {} } as any], more: false, now: 2 }, notice: { version: 3, whats_new: ["v3: x"] } };
    const api: any = w.room;
    const decorated: RelayBackend = {
      device: () =>
        new Proxy(api, {
          get: (t, k) =>
            typeof t[k] === "function" ? async (...a: unknown[]) => {
              const out = k === "events" ? { ok: false, error: "internal" } : await t[k](...a);
              return out.ok ? out : { ...out, ...ride };
            } : t[k],
        }),
    };
    const call = async (method: string, path: string, body?: unknown) => {
      const r = await handleRest(
        new Request(`https://relay.test${path}`, { method, headers: { authorization: `Bearer ${w.token}` }, ...(body ? { body: JSON.stringify(body) } : {}) }),
        decorated,
      );
      return { status: r.status, body: (await r.json()) as any };
    };
    const cases = [
      await call("POST", `/v1/devices/${DEVICE}/cmd`, { name: "volume", args: { volume: 3 } }), // device refuses
      await call("POST", `/v1/devices/${DEVICE}/cmd`, { name: "nope" }), // relay refuses at once
      await call("POST", `/v1/devices/${DEVICE}/talk`, { listen_ms: -5 }), // failed /talk
      await call("GET", `/v1/jobs/${DEVICE}.j_nosuchjob`), // unknown job
      await call("PUT", `/v1/webhook`, { url: "http://not-https" }), // bad webhook
      await call("GET", `/v1/devices/${DEVICE}/events`), // failed events read
    ];
    for (const c of cases) {
      expect(c.body.ok).toBe(false);
      expect(c.body).toMatchObject({ events: [{ id: "e9", type: "tap" }], skill_update: { version: 3, url: "https://relay.test/skill.md" } });
    }
  });

  it("REST: cmd {wait:true} blocks; non-job commands ignore it; bad wait is invalid", async () => {
    const w = await setup();
    w.voice.ttsSamples = 1600;
    const r = await rest(w.backend, w.token, `/v1/devices/${DEVICE}/cmd`, { name: "say", args: { text: "hi" }, wait: true });
    expect(r).toMatchObject({ status: 200, body: { ok: true, result: { status: "done", ms: 100, text: "hi" } } });
    expect(await rest(w.backend, w.token, `/v1/devices/${DEVICE}/cmd`, { name: "imu", wait: true })).toMatchObject({ status: 200, body: { ok: true, result: {} } });
    expect(await rest(w.backend, w.token, `/v1/devices/${DEVICE}/cmd`, { name: "say", args: { text: "hi" }, wait: 1 })).toMatchObject({ status: 400 });
  });
});

/** Each segment takes `perSegMs` to synthesise and yields 1 s of audio. */
class SlowVoice extends FakeVoice {
  constructor(readonly perSegMs: number) {
    super();
  }
  async tts(text: string, opts: { voice?: string }) {
    this.ttsCalls.push({ text, voice: opts.voice });
    await new Promise((r) => setTimeout(r, this.perSegMs));
    return new Int16Array(16000).fill(500);
  }
}

describe("streamed speech", () => {
  it("first audio frame leaves before the whole utterance is synthesised", async () => {
    const voice = new SlowVoice(80);
    const w = await setup({}, { voice, timings: { sayChunkSamples: 5120, sayLeadFrames: 3 } });
    const text = "One. Two is a bit longer than one, so it gets its own segment. " + "Three keeps going for a while to fill up the second group of sentences nicely. ".repeat(4) + "Four is the end of this test, with some more words here to pad it out properly.";
    const segments = splitSpeech(text);
    expect(segments.length).toBeGreaterThanOrEqual(3);
    const t0 = Date.now();
    const out = await w.room.command(w.token, "say", { text });
    const job = (out as any).result.job;
    const done = await w.room.job(w.token, job, 8000);
    expect(done).toMatchObject({ ok: true, result: { status: "done" } });
    const firstFrame = w.dev.log.find((l) => l.kind === "frame")!.at - t0;
    const synthAll = segments.length * voice.perSegMs; // sequential cost if synthesised up front
    const t = (done as any).result.result.timings;
    expect(firstFrame).toBeLessThan(synthAll);
    expect(t.first_audio).toBeLessThan(t.tts_total);
    expect(voice.ttsCalls.map((c) => c.text)).toEqual(segments);
    // every sample arrives, in order, one end frame, frames within the cap
    const frames = w.sock.bins.map((b) => decodeAudioFrame(b)!);
    expect(frames.map((f) => f.header.seq)).toEqual(frames.map((_, i) => i));
    expect(frames.filter((f) => f.header.end)).toHaveLength(1);
    expect(frames.at(-1)!.header.end).toBe(true);
    expect(frames.reduce((n, f) => n + f.payload.byteLength, 0)).toBe(segments.length * 32000);
    expect((done as any).result.result.ms).toBe(segments.length * 1000);
  });

  it("synthesises the next segment while the current one is still being synthesised (lookahead)", async () => {
    const starts: number[] = [];
    const voice = new SlowVoice(80);
    const base = voice.tts.bind(voice);
    voice.tts = async (t, o) => {
      starts.push(Date.now());
      return base(t, o);
    };
    const w = await setup({}, { voice });
    const job = ((await w.room.command(w.token, "say", { text: "One. Two two two. Three three three." })) as any).result.job;
    await w.room.job(w.token, job, 5000);
    expect(starts.length).toBe(2);
    expect(starts[1] - starts[0]).toBeLessThan(40); // started together, not after segment 1's 80 ms
  });

  it("never holds audio back while the device runs dry: short pieces go out as they come", async () => {
    // A streaming provider yielding 200 ms pieces at t=0, 400, 1000 ms (slower than real time).
    const voice = new FakeVoice();
    const at = [0, 400, 1000];
    (voice as any).ttsStream = async function* () {
      const t0 = Date.now();
      for (const t of at) {
        const wait = t0 + t - Date.now();
        if (wait > 0) await new Promise((r) => setTimeout(r, wait));
        yield new Int16Array(3200).fill(700);
      }
    };
    const w = await setup({}, { voice, timings: { sayChunkSamples: 5120, sayLeadFrames: 3 } });
    const t0 = Date.now();
    const job = ((await w.room.command(w.token, "say", { text: "streamed" })) as any).result.job;
    await w.room.job(w.token, job, 5000);
    const frames = w.dev.log.filter((l) => l.kind === "frame").map((f) => ({ t: f.at - t0, bytes: f.bytes, end: f.end }));
    // first piece goes out within the first-frame hold (not at 400 ms with piece 2)
    expect(frames[0].bytes).toBe(6400);
    expect(frames[0].t).toBeLessThan(300);
    // piece 2 goes out when it arrives (~400 ms), not merged with piece 3 at ~1000 ms
    expect(frames[1].bytes).toBe(6400);
    expect(frames[1].t).toBeLessThan(800);
    expect(frames.reduce((n, f) => n + (f.bytes ?? 0), 0)).toBe(3 * 6400);
    expect(frames.at(-1)!.end).toBe(true);
  });

  it("a TTS failure mid-utterance fails the job and closes the device stream with an end frame", async () => {
    const voice = new SlowVoice(10);
    let n = 0;
    const base = voice.tts.bind(voice);
    voice.tts = async (t, o) => {
      if (n++ === 1) throw new Error("upstream 500");
      return base(t, o);
    };
    const w = await setup({}, { voice });
    const job = ((await w.room.command(w.token, "say", { text: "First sentence here. Second sentence fails." })) as any).result.job;
    expect(await w.room.job(w.token, job, 3000)).toMatchObject({ ok: true, result: { status: "failed", error: "upstream 500" } });
    await until(() => w.sock.bins.some((b) => decodeAudioFrame(b)!.header.end));
    expect(w.sock.bins.map((b) => decodeAudioFrame(b)!.header).filter((h) => h.end)).toHaveLength(1);
  });

  it("splitSpeech: short first segment, grouped rest, nothing lost", () => {
    expect(splitSpeech("Hi! How are you today? I am fine.")).toEqual(["Hi!", "How are you today? I am fine."]);
    expect(splitSpeech("Pi is 3.14, see molty.uno.")).toEqual(["Pi is 3.14, see molty.uno."]);
    const long = "Well, after thinking about it for quite a long time, I believe the answer is probably yes. Ok.";
    const parts = splitSpeech(long);
    expect(parts[0]).toBe("Well, after thinking about it for quite a long time,");
    expect(parts.join(" ").replace(/\s+/g, " ")).toBe(long);
    expect(splitSpeech("no punctuation at all")).toEqual(["no punctuation at all"]);
  });
});

/** A streaming provider that yields one piece, then stalls until its signal aborts. */
function stalledVoice() {
  const st = { aborted: false, finalized: false };
  const voice = new FakeVoice();
  (voice as any).ttsStream = async function* (_t: string, o: TtsOptions) {
    try {
      yield new Int16Array(1600).fill(300);
      await new Promise<void>((resolve) => {
        if (o.signal?.aborted) return resolve();
        o.signal?.addEventListener("abort", () => resolve());
      });
      st.aborted = !!o.signal?.aborted;
    } finally {
      st.finalized = true;
    }
  };
  return { voice, st };
}
const timer = { setTimeout: (f: () => void, ms: number) => setTimeout(f, ms), clearTimeout: (h: unknown) => clearTimeout(h as ReturnType<typeof setTimeout>) };

describe("TTS cancellation: a stalled provider is torn down", () => {
  it("segment timeout aborts the provider (its stream finishes), and the feed reports tts_timeout", async () => {
    const { voice, st } = stalledVoice();
    const feed = new PcmFeed();
    await synthesize(voice, ["Hello there."], {}, feed, timer, 50);
    expect((feed.error as any)?.code).toBe("tts_timeout");
    await until(() => st.finalized, 500);
    expect(st).toEqual({ aborted: true, finalized: true });
  });

  it("the consumer giving up aborts the provider at once (no wait for the segment timeout)", async () => {
    const { voice, st } = stalledVoice();
    const feed = new PcmFeed();
    const run = synthesize(voice, ["Hello there."], {}, feed, timer, 10_000);
    await until(() => feed.queued, 500);
    const t0 = Date.now();
    feed.end(); // the say streamer stopped reading (device done / gone)
    await run;
    await until(() => st.finalized, 500);
    expect(st).toEqual({ aborted: true, finalized: true });
    expect(Date.now() - t0).toBeLessThan(500);
  });

  it("Workers AI: a model stream that stalls mid-audio is cancelled on timeout", async () => {
    let cancelled: unknown = "no";
    const head = encodeWav(new Int16Array(800).fill(9), 16000);
    const ai = {
      async run() {
        return new ReadableStream<Uint8Array>({
          start(c) {
            c.enqueue(head.slice(0, 44 + 800)); // header + 400 samples, then nothing more
          },
          pull() {
            return new Promise<void>(() => {}); // stalled backend
          },
          cancel(reason) {
            cancelled = reason ?? "cancelled";
          },
        });
      },
    };
    const feed = new PcmFeed();
    await synthesize(new WorkersAiVoice(ai, {}), ["Hello there."], {}, feed, timer, 50);
    expect((feed.error as any)?.code).toBe("tts_timeout");
    await until(() => cancelled !== "no", 500);
    expect(cancelled).not.toBe("no");
  });

  it("say: a provider stalling mid-utterance fails the job with tts_timeout and is cancelled", async () => {
    const { voice, st } = stalledVoice();
    const w = await setup({}, { voice, timings: { ttsTimeoutMs: 100 } });
    const job = ((await w.room.command(w.token, "say", { text: "Hello there." })) as any).result.job;
    expect(await w.room.job(w.token, job, 3000)).toMatchObject({ ok: true, result: { status: "failed", error: "tts_timeout" } });
    await until(() => st.finalized, 500);
    expect(st.aborted).toBe(true);
  });
});

describe("Workers AI: streamed TTS and Nova-3 STT", () => {
  it("ttsStream yields PCM as the binding's stream delivers it, across odd byte boundaries", async () => {
    const pcm = new Int16Array(4000).map((_, i) => i);
    const wav = encodeWav(pcm, 16000);
    let pulled = 0;
    const cuts = [7, 45, 1001, 3333, 6000, wav.byteLength];
    const ai = {
      async run() {
        let i = 0;
        let from = 0;
        return new ReadableStream<Uint8Array>({
          pull(c) {
            pulled++;
            if (i >= cuts.length) return c.close();
            c.enqueue(wav.slice(from, cuts[i]));
            from = cuts[i++];
          },
        });
      },
    };
    const v = new WorkersAiVoice(ai, {});
    const pieces: Int16Array[] = [];
    let pulledAtFirst = 0;
    for await (const p of v.ttsStream("hi", {})) {
      if (!pieces.length) pulledAtFirst = pulled;
      pieces.push(p);
    }
    expect(pieces.length).toBeGreaterThan(1);
    expect(pulledAtFirst).toBeLessThan(cuts.length); // first piece before the stream ended
    const all = new Int16Array(pieces.reduce((n, p) => n + p.length, 0));
    let o = 0;
    for (const p of pieces) (all.set(p, o), (o += p.length));
    expect(Array.from(all)).toEqual(Array.from(pcm));
  });

  it("ttsStream converts a non-16k WAV whole, and rejects MP3 clearly", async () => {
    const v24 = new WorkersAiVoice({ run: async () => new Response(encodeWav(new Int16Array(2400).fill(100), 24000)).body }, {});
    const got: Int16Array[] = [];
    for await (const p of v24.ttsStream("x", {})) got.push(p);
    expect(got).toHaveLength(1);
    expect(got[0].length).toBe(1600);
    const mp3 = new WorkersAiVoice({ run: async () => new Response(Uint8Array.from([0x49, 0x44, 0x33, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0])).body }, {});
    await expect((async () => {
      for await (const _ of mp3.ttsStream("x", {})) void _;
    })()).rejects.toThrow(/MP3/);
  });

  it("Nova-3 (STT_MODEL=@cf/deepgram/nova-3): audio as {body, contentType}, transcript from results.channels", async () => {
    const calls: any[] = [];
    const ai = {
      async run(model: string, inputs: any) {
        calls.push({ model, inputs });
        return { results: { channels: [{ alternatives: [{ transcript: " hello there ", confidence: 0.9 }] }] } };
      },
    };
    const v = new WorkersAiVoice(ai, { sttModel: "@cf/deepgram/nova-3" });
    expect(await v.stt(new Int16Array(1600))).toBe("hello there");
    expect(calls[0].model).toBe("@cf/deepgram/nova-3");
    expect(calls[0].inputs.audio.contentType).toBe("audio/wav");
    expect(typeof calls[0].inputs.audio.body.getReader).toBe("function");
  });
});

describe("protocol: end-of-speech fields on the last mic frame", () => {
  it("decodes reason and speech_ms; drops malformed ones; older headers unchanged", () => {
    const enc = (h: object) => new TextEncoder().encode(JSON.stringify({ s: "j", seq: 3, kind: "pcm16", rate: 16000, end: true, ...h }));
    expect(decodeAudioFrame(enc({ reason: "silence", speech_ms: 1234 }))!.header).toEqual({ s: "j", seq: 3, kind: "pcm16", rate: 16000, end: true, reason: "silence", speech_ms: 1234 });
    expect(decodeAudioFrame(enc({ reason: "error" }))!.header.reason).toBe("error");
    expect(decodeAudioFrame(enc({ reason: "Bad Reason!", speech_ms: -1 }))!.header).toEqual({ s: "j", seq: 3, kind: "pcm16", rate: 16000, end: true });
    expect(decodeAudioFrame(enc({}))!.header).toEqual({ s: "j", seq: 3, kind: "pcm16", rate: 16000, end: true });
  });
});
