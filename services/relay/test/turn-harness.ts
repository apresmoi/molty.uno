// The shake-turn harness shared by the "while the human speaks" tests (partials.test.ts) and the
// reflex tests (reflex.test.ts): a fake device that adopts shake listens and streams one chunk per
// word, a fake voice box (stt.partial per word, stt.turn_end, the final; optional stt.reflex), agents.
import type { RelayBackend } from "../src/core/api.js";
import type { AgentEvent } from "../src/core/events.js";
import { decodeAudioFrame, encodeAudioFrame } from "../src/core/protocol.js";
import type { DeviceRoom } from "../src/core/room.js";
import { handleMcp } from "../src/surfaces/mcp.js";
import { handleRest } from "../src/surfaces/rest.js";
import { DEVICE, type FakeSocket, LINK, connect, makeRoom } from "./helpers.js";

export const FAST = { cmdTimeoutMs: 400, pairWindowMs: 400, sayChunkSamples: 1600, sayLeadFrames: 2, listenGraceMs: 300, sayDoneGraceMs: 400, boxSttFinalMs: 400, reactShowMs: 60, partialLeadMs: 0 };
export const CAPS = ["say", "say_cues", "listen", "listen_stop", "listen_window", "ack", "shake_talk", "mood", "act", "react"];
export const SHAKE = { peak_g: 2.4, reversals: 5, ms: 900 };
export const NEXT = ["NEWC2345", "NEWD2345", "NEWE2345", "NEWF2345", "NEWG2345"];

/** What the simulated human says in a shake turn, as the box hears it. */
export interface Speech {
  words: string[];
  /** The box's final text (default: the words). */
  final?: string;
  /** ms between mic chunks; one chunk = one more word heard. */
  chunkMs?: number;
}

export type Log = Array<{ at: number; kind: string; name?: string; args?: any; job?: string }>;

/** A device: answers cmds, adopts a shake's listen streaming one chunk per word, plays say streams (say.done 5 ms after the end). */
export function device(room: DeviceRoom, sock: FakeSocket, speech: () => Speech) {
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
export interface BoxOpts {
  ttsMs?: number;
  /** The box offers the reflex classifier (voice.hello reflex:true). */
  reflex?: boolean;
  /** Its stt.reflex for the text so far (called after each stt.partial of a listen started with reflex:true); undefined: none. */
  reflexFor?: (text: string) => Record<string, unknown> | undefined;
  /** The box offers first words: its line for the human's words (undefined: it never answers). */
  openerLine?: (text: string) => string | undefined;
  openerMs?: number;
}

export function box(room: DeviceRoom, sock: FakeSocket, speech: () => Speech, o: BoxOpts = {}) {
  const tts: Array<{ at: number; text: string }> = [];
  const got: Record<string, number> = {};
  const starts: Array<Record<string, unknown>> = [];
  const openers: Array<Record<string, any>> = [];
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
    if (f.t === "opener") {
      openers.push(f);
      const line = o.openerLine?.(f.text);
      if (line !== undefined) setTimeout(() => !sock.closed && say({ t: "opener.result", id: f.id, text: line, ms: 1 }), o.openerMs ?? 1);
    }
    if (f.t === "stt.start") {
      got[f.id] = 0;
      starts.push(f);
    }
    if (f.t === "stt.end") setTimeout(() => !sock.closed && say({ t: "stt.final", id: f.id, text: speech().final ?? speech().words.join(" "), ms: { final: 3 } }), 3);
  };
  sock.sendBinary = (b: Uint8Array) => {
    const fr = decodeAudioFrame(b)!;
    const id = fr.header.s;
    if (got[id] === undefined) return;
    const w = speech().words;
    const n = Math.min(w.length, ++got[id]);
    if (n <= w.length && got[id] <= w.length) {
      say({ t: "stt.partial", id, text: w.slice(0, n).join(" ") });
      const r = starts.find((x) => x.id === id)?.reflex ? o.reflexFor?.(w.slice(0, n).join(" ")) : undefined;
      if (r) setTimeout(() => !sock.closed && say({ t: "stt.reflex", id, ...r }), 1);
    }
    if (got[id] === w.length + 2 && !turned.has(id)) {
      turned.add(id);
      say({ t: "stt.turn_end", id });
    }
  };
  return { tts, starts, openers, hello: () => room.socketMessage(sock, JSON.stringify({ t: "voice.hello", name: "gpu box", version: "0.1.0", voices: ["a.wav"], ...(o.openerLine ? { opener: true } : {}), stt: true, tts: true, ...(o.reflex ? { reflex: true } : {}) })) };
}

export async function setup(
  o: {
    caps?: string[];
    ttsMs?: number;
    timings?: Record<string, number>;
    agents?: number;
    box?: Omit<BoxOpts, "ttsMs">;
    fetch?: (r: Request) => Promise<Response>;
    log?: (msg: string, data?: Record<string, unknown>) => void;
  } = {},
) {
  const sent: Array<{ url: string; body: string }> = [];
  const ctx = makeRoom({
    timings: { ...FAST, ...o.timings },
    scheduler: { setAlarm: async () => {} },
    log: o.log,
    fetch:
      o.fetch ??
      (async (r) => {
        sent.push({ url: r.url, body: await r.text() });
        return new Response("ok");
      }),
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
  const bx = box(ctx.room, bsock, () => speech, { ttsMs: o.ttsMs, ...o.box });
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

export const until = async (cond: () => boolean | Promise<boolean>, ms = 2000) => {
  const end = Date.now() + ms;
  while (!(await cond()) && Date.now() < end) await new Promise((r) => setTimeout(r, 4));
};
export const evs = async (room: DeviceRoom, token: string) => ((await room.events(token, { wait_ms: 0 })) as any).result.events as AgentEvent[];
/** Reads a token's inbox until `type` (or ms), keeping everything read. */
export async function readUntil(room: DeviceRoom, token: string, pred: (e: AgentEvent) => boolean, seen: AgentEvent[] = [], ms = 2500) {
  await until(async () => {
    seen.push(...(await evs(room, token)));
    return seen.some(pred);
  }, ms);
  return seen;
}
export const jobRec = (kv: { get<T>(k: string): Promise<T | undefined> }, id: string) => kv.get<any>(`job:${id}`);
export const rest = async (b: RelayBackend, token: string, method: string, path: string, body?: unknown, headers: Record<string, string> = {}) => {
  const r = await handleRest(new Request(`https://relay.test${path}`, { method, headers: { ...(token ? { authorization: `Bearer ${token}` } : {}), "content-type": "application/json", ...headers }, body: body === undefined ? undefined : JSON.stringify(body) }), b);
  return { status: r.status, body: r.status === 204 ? null : ((await r.json()) as any) };
};
let rid = 0;
export const mcp = async (b: RelayBackend, token: string, name: string, args: unknown) =>
  ((await (
    await handleMcp(new Request("https://relay.test/mcp", { method: "POST", headers: { "content-type": "application/json", authorization: `Bearer ${token}` }, body: JSON.stringify({ jsonrpc: "2.0", id: ++rid, method: "tools/call", params: { name, arguments: args } }) }), b)
  ).json()) as any).result.structuredContent;

