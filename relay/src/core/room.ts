// DeviceRoom: one per device_id. Owns TOFU device auth, pairing, agent tokens,
// command routing and say/listen jobs. Host-neutral: the host supplies
// sockets, storage, a clock and a voice provider.
//
// Hibernation rule: nothing that decides
// admission lives in instance fields. Which socket is the authenticated
// device is read from each socket's own metadata (the DO's serialized
// attachment); device key, link code, tokens, pairings and jobs live in KV.
// Instance fields below only hold work that is in flight -- and in-flight
// work (an open agent request, a pending timer, an incoming frame) keeps
// the host awake. If the host is evicted anyway, jobs left "running" with
// no in-memory task are reported as failed:"interrupted" instead of hanging.

import { type AgentApi, type JobView, type Outcome, type PairRequest, type PairResult, TERMINAL } from "./api.js";
import { durationMs, bytesToPcm16, levels, pcm16ToBytes } from "./audio.js";
import { CommandError, validateCommand } from "./commands.js";
import { b64Decode, b64urlEncode, randomBytes, randomDigits, safeEqual, sha256Hex } from "./crypto.js";
import {
  CLOSE_FORGOTTEN,
  CLOSE_PROTOCOL,
  CLOSE_REPLACED,
  CLOSE_UNAUTHORIZED,
  LINK_CODE_RE,
  MAX_LISTEN_BYTES,
  PONG_TEXT,
  SAMPLE_RATE,
  decodeAudioFrame,
  encodeAudioFrame,
  isRecord,
  parseHello,
  parseTextFrame,
  type JsonObject,
} from "./protocol.js";
import { type Clock, type KV, sleep } from "./store.js";
import type { VoiceProvider } from "../voice/types.js";

export interface SocketMeta {
  stage: "hello" | "live";
  opened: number;
  fw?: string;
  caps?: string[];
  since?: number;
}

export interface DeviceSocket {
  sendText(text: string): void;
  sendBinary(bytes: Uint8Array): void;
  close(code: number, reason: string): void;
  getMeta(): SocketMeta | undefined;
  setMeta(meta: SocketMeta): void;
  same(other: DeviceSocket): boolean;
}

export interface SocketHost {
  sockets(): DeviceSocket[];
}

export interface RoomTimings {
  cmdTimeoutMs: number;
  helloTimeoutMs: number;
  pairWindowMs: number;
  ttsTimeoutMs: number;
  sttTimeoutMs: number;
  listenGraceMs: number;
  jobTtlMs: number;
  /** say streaming: samples per frame (<= 8192 for the 16 KB payload cap). */
  sayChunkSamples: number;
  /** say streaming: frames sent ahead of real time to fill the device ring buffer. */
  sayLeadFrames: number;
}

export const DEFAULT_TIMINGS: RoomTimings = {
  cmdTimeoutMs: 6000, // under the Go CLI's 8 s per-call budget and the common 10 s agent tool-call cap
  helloTimeoutMs: 10000,
  pairWindowMs: 120000,
  ttsTimeoutMs: 20000,
  sttTimeoutMs: 30000,
  listenGraceMs: 5000,
  jobTtlMs: 15 * 60 * 1000,
  sayChunkSamples: 5120, // 320 ms, 10240 B
  sayLeadFrames: 3,
};

export interface RoomDeps {
  deviceId: string;
  kv: KV;
  clock: Clock;
  voice: VoiceProvider;
  host: SocketHost;
  timings?: Partial<RoomTimings>;
  log?: (msg: string, data?: Record<string, unknown>) => void;
}

interface DeviceRecord {
  key_hash: string;
  claimed_at: number;
}
interface TokenRecord {
  agent_name: string;
  created: number;
}
interface PairRecord {
  id: string;
  req: string;
  code: string;
  agent_name: string;
  created: number;
  expires: number;
  status: "pending" | "approved" | "rejected" | "timeout";
  token?: string;
}
interface JobRecord extends JobView {
  created: number;
  updated: number;
}

interface PendingCmd {
  resolve(res: Outcome<JsonObject>): void;
  timer: unknown;
}

interface ListenBuffer {
  job: string;
  chunks: Uint8Array[];
  bytes: number;
  ended: boolean;
  wake?: () => void;
}

const K = {
  device: "device",
  link: "link",
  pairCurrent: "paircur",
  token: (hash: string) => `tok:${hash}`,
  pair: (id: string) => `pair:${id}`,
  job: (id: string) => `job:${id}`,
};

const fail = (error: string, message?: string): { ok: false; error: string; message?: string } =>
  message === undefined ? { ok: false, error } : { ok: false, error, message };

export class DeviceRoom implements AgentApi {
  readonly deviceId: string;
  private readonly kv: KV;
  private readonly clock: Clock;
  private readonly voice: VoiceProvider;
  private readonly host: SocketHost;
  private readonly t: RoomTimings;
  private readonly log: NonNullable<RoomDeps["log"]>;

  // ---- in-flight work only (see header comment) ----
  private readonly pendingCmds = new Map<string, PendingCmd>();
  private readonly pairWaiters = new Map<string, Set<() => void>>();
  private readonly jobWaiters = new Map<string, Set<() => void>>();
  private readonly tasks = new Set<string>();
  private readonly sayDone = new Map<string, () => void>();
  private listenBuf: ListenBuffer | undefined;
  private audioJob: string | undefined;
  private readonly lastEvents = new Map<string, { data: unknown; at: number }>();
  private cmdSeq = 0;

  constructor(deps: RoomDeps) {
    this.deviceId = deps.deviceId;
    this.kv = deps.kv;
    this.clock = deps.clock;
    this.voice = deps.voice;
    this.host = deps.host;
    this.t = { ...DEFAULT_TIMINGS, ...deps.timings };
    this.log = deps.log ?? (() => {});
  }

  // =====================================================================
  // Device socket side
  // =====================================================================

  socketOpen(sock: DeviceSocket): void {
    const now = this.clock.now();
    sock.setMeta({ stage: "hello", opened: now });
    // Bound unauthenticated sockets: drop stale ones and keep at most 4 waiting.
    const waiting = this.host
      .sockets()
      .filter((s) => !s.same(sock) && s.getMeta()?.stage !== "live")
      .sort((a, b) => (a.getMeta()?.opened ?? 0) - (b.getMeta()?.opened ?? 0));
    for (const [i, s] of waiting.entries()) {
      const stale = now - (s.getMeta()?.opened ?? 0) > this.t.helloTimeoutMs;
      if (stale || waiting.length - i > 3) s.close(CLOSE_PROTOCOL, "hello timeout");
    }
  }

  async socketMessage(sock: DeviceSocket, data: string | Uint8Array): Promise<void> {
    const meta = sock.getMeta();
    if (!meta) return sock.close(CLOSE_PROTOCOL, "unknown socket");
    if (meta.stage === "hello") {
      if (this.clock.now() - meta.opened > this.t.helloTimeoutMs) return sock.close(CLOSE_PROTOCOL, "hello timeout");
      const frame = typeof data === "string" ? parseTextFrame(data) : undefined;
      if (!frame || frame.t !== "hello") return sock.close(CLOSE_PROTOCOL, "first frame must be hello");
      return this.onHello(sock, frame);
    }
    if (typeof data !== "string") return this.onAudio(data);
    const frame = parseTextFrame(data);
    if (!frame) return;
    switch (frame.t) {
      case "ping":
        sock.sendText(PONG_TEXT);
        return;
      case "pong":
        return;
      case "res":
        return this.onRes(frame);
      case "evt":
        return this.onEvt(frame);
      case "pair.result":
        return this.onPairResult(frame);
      case "link_code":
        return this.onLinkCode(frame.link_code);
      case "forget":
        return this.onForget(sock);
      default:
        return;
    }
  }

  socketClose(sock: DeviceSocket): void {
    if (sock.getMeta()?.stage !== "live") return;
    // The runtime may still list the closing socket; anything else live is a replacement.
    if (this.liveSocket(sock)) return;
    this.dropInFlight("device_offline");
  }

  private liveSocket(except?: DeviceSocket): DeviceSocket | undefined {
    return this.host.sockets().find((s) => (!except || !s.same(except)) && s.getMeta()?.stage === "live");
  }

  private dropInFlight(error: string) {
    for (const [id, p] of this.pendingCmds) {
      this.clock.clearTimeout(p.timer);
      this.pendingCmds.delete(id);
      p.resolve(fail(error));
    }
    if (this.listenBuf && !this.listenBuf.ended) {
      this.listenBuf.ended = true;
      this.listenBuf.wake?.();
    }
  }

  private async onHello(sock: DeviceSocket, frame: Record<string, unknown>) {
    const hello = parseHello(frame);
    if (!hello) return sock.close(CLOSE_PROTOCOL, "malformed hello");
    if (hello.id !== this.deviceId) return sock.close(CLOSE_UNAUTHORIZED, "device id mismatch");
    const key = b64Decode(hello.key);
    if (!key || key.byteLength < 16 || key.byteLength > 64) return sock.close(CLOSE_UNAUTHORIZED, "bad device key");
    const hash = await sha256Hex(key);
    const record = await this.kv.get<DeviceRecord>(K.device);
    if (!record) {
      await this.kv.put<DeviceRecord>(K.device, { key_hash: hash, claimed_at: this.clock.now() });
      this.log("device claimed (tofu)", { device: this.deviceId });
    } else if (!safeEqual(record.key_hash, hash)) {
      return sock.close(CLOSE_UNAUTHORIZED, "device key mismatch");
    }
    const previous = this.liveSocket(sock);
    sock.setMeta({ stage: "live", opened: sock.getMeta()?.opened ?? this.clock.now(), fw: hello.fw, caps: hello.caps, since: this.clock.now() });
    if (previous) {
      previous.close(CLOSE_REPLACED, "replaced by a newer connection");
      this.dropInFlight("device_offline");
    }
    if (hello.link_code) await this.kv.put(K.link, { code: hello.link_code, at: this.clock.now() });
    sock.sendText(JSON.stringify({ t: "welcome" }));
  }

  private onRes(frame: Record<string, unknown>) {
    if (typeof frame.id !== "string") return;
    const pending = this.pendingCmds.get(frame.id);
    if (!pending) return;
    this.pendingCmds.delete(frame.id);
    this.clock.clearTimeout(pending.timer);
    if (frame.ok === true) {
      pending.resolve({ ok: true, result: isRecord(frame.result) ? (frame.result as JsonObject) : {} });
    } else {
      pending.resolve(fail(typeof frame.error === "string" && frame.error ? frame.error : "device_error"));
    }
  }

  private async onEvt(frame: Record<string, unknown>) {
    if (typeof frame.name !== "string" || frame.name.length > 64) return;
    const data = isRecord(frame.data) ? frame.data : {};
    this.lastEvents.set(frame.name, { data, at: this.clock.now() });
    if (this.lastEvents.size > 32) this.lastEvents.delete(this.lastEvents.keys().next().value!);
    if (frame.name === "say.done" && typeof data.job === "string") this.sayDone.get(data.job)?.();
    if (frame.name === "link_code") await this.onLinkCode(data.link_code);
  }

  private async onLinkCode(code: unknown) {
    if (typeof code === "string" && LINK_CODE_RE.test(code)) await this.kv.put(K.link, { code, at: this.clock.now() });
  }

  private async onForget(sock: DeviceSocket) {
    for (const prefix of ["tok:", "pair:", "job:"]) {
      for (const key of (await this.kv.list(prefix)).keys()) await this.kv.delete(key);
    }
    for (const key of [K.device, K.link, K.pairCurrent]) await this.kv.delete(key);
    this.log("device forgotten", { device: this.deviceId });
    sock.sendText(JSON.stringify({ t: "forgotten" }));
    sock.close(CLOSE_FORGOTTEN, "forgotten");
    this.dropInFlight("device_offline");
  }

  // =====================================================================
  // Device commands
  // =====================================================================

  private sendCmd(name: string, args: JsonObject, timeoutMs = this.t.cmdTimeoutMs): Promise<Outcome<JsonObject>> {
    const sock = this.liveSocket();
    if (!sock) return Promise.resolve(fail("device_offline"));
    const id = `c${++this.cmdSeq}${b64urlEncode(randomBytes(3))}`;
    return new Promise((resolve) => {
      const timer = this.clock.setTimeout(() => {
        this.pendingCmds.delete(id);
        resolve(fail("device_timeout"));
      }, timeoutMs);
      this.pendingCmds.set(id, { resolve, timer });
      try {
        sock.sendText(JSON.stringify({ t: "cmd", id, name, args }));
      } catch {
        this.clock.clearTimeout(timer);
        this.pendingCmds.delete(id);
        resolve(fail("device_offline"));
      }
    });
  }

  // =====================================================================
  // Agent API
  // =====================================================================

  private async authorize(token: string): Promise<TokenRecord | undefined> {
    if (typeof token !== "string" || !token.startsWith(`${this.deviceId}.`)) return undefined;
    return this.kv.get<TokenRecord>(K.token(await sha256Hex(token)));
  }

  async revoke(token: string): Promise<Outcome> {
    if (!(await this.authorize(token))) return fail("unauthorized");
    await this.kv.delete(K.token(await sha256Hex(token)));
    return { ok: true, result: { revoked: true } };
  }

  async command(token: string, name: string, args: unknown): Promise<Outcome> {
    const agent = await this.authorize(token);
    if (!agent) return fail("unauthorized");
    let cmd;
    try {
      cmd = validateCommand(name, args);
    } catch (e) {
      if (e instanceof CommandError) return fail(e.code, e.message);
      throw e;
    }
    if (!this.liveSocket()) return fail("device_offline");
    if (cmd.job) return this.startJob(cmd.name as "say" | "listen", cmd.args);
    const res = await this.sendCmd(cmd.name, cmd.args);
    if (!res.ok || cmd.name !== "status") return res;
    const sock = this.liveSocket();
    const meta = sock?.getMeta();
    const events: JsonObject = {};
    for (const [k, v] of this.lastEvents) events[k] = { data: v.data as JsonObject, at: v.at };
    return {
      ok: true,
      result: {
        ...res.result,
        relay: { device_id: this.deviceId, online: !!sock, fw: meta?.fw ?? null, caps: meta?.caps ?? [], connected_since: meta?.since ?? null, voice: this.voice.name, events },
      },
    };
  }

  // ---------------- pairing ----------------

  async pair(req: PairRequest): Promise<Outcome<PairResult>> {
    const waitMs = Math.max(0, Math.min(req.wait_ms ?? this.t.pairWindowMs, this.t.pairWindowMs));
    if (req.pair_id !== undefined) {
      if (typeof req.pair_id !== "string" || !/^p_[A-Za-z0-9_-]{22}$/.test(req.pair_id)) return fail("invalid_args", "bad pair_id");
      const rec = await this.kv.get<PairRecord>(K.pair(req.pair_id));
      if (!rec) return fail("not_found", "unknown or already-claimed pair_id");
      return this.awaitPair(rec.id, waitMs);
    }
    const name = req.agent_name;
    if (typeof name !== "string" || name.trim().length === 0 || name.length > 64 || /[\u0000-\u001f\u007f]/.test(name)) {
      return fail("invalid_args", "agent_name must be 1..64 printable characters");
    }
    if (typeof req.link_code !== "string" || !LINK_CODE_RE.test(req.link_code)) return fail("bad_link_code");
    const sock = this.liveSocket();
    if (!sock) return fail("device_offline");
    const link = await this.kv.get<{ code: string }>(K.link);
    if (!link || !safeEqual(link.code, req.link_code)) return fail("bad_link_code");
    const now = this.clock.now();
    const currentId = await this.kv.get<string>(K.pairCurrent);
    if (currentId) {
      const current = await this.kv.get<PairRecord>(K.pair(currentId));
      if (current && current.status === "pending" && current.expires > now) return fail("pair_busy");
    }
    const rec: PairRecord = {
      id: `p_${b64urlEncode(randomBytes(16))}`,
      req: b64urlEncode(randomBytes(6)),
      code: randomDigits(4),
      agent_name: name.trim(),
      created: now,
      expires: now + this.t.pairWindowMs,
      status: "pending",
    };
    await this.kv.put(K.pair(rec.id), rec);
    await this.kv.put(K.pairCurrent, rec.id);
    sock.sendText(JSON.stringify({ t: "pair.request", req: rec.req, agent_name: rec.agent_name, code: rec.code }));
    return this.awaitPair(rec.id, waitMs);
  }

  private async awaitPair(id: string, waitMs: number): Promise<Outcome<PairResult>> {
    const deadline = this.clock.now() + waitMs;
    for (;;) {
      const rec = await this.kv.get<PairRecord>(K.pair(id));
      if (!rec) return fail("not_found", "unknown or already-claimed pair_id");
      const now = this.clock.now();
      if (rec.status === "pending" && now >= rec.expires) rec.status = "timeout";
      if (rec.status === "approved" && rec.token) {
        await this.kv.delete(K.pair(id)); // the token is handed out exactly once
        return { ok: true, result: { status: "approved", agent_token: rec.token, device_id: this.deviceId, agent_name: rec.agent_name, code: rec.code } };
      }
      if (rec.status !== "pending") {
        await this.kv.delete(K.pair(id));
        if ((await this.kv.get<string>(K.pairCurrent)) === id) await this.kv.delete(K.pairCurrent);
        return fail(rec.status === "rejected" ? "rejected" : "timeout");
      }
      const remaining = Math.min(deadline, rec.expires) - now;
      if (remaining <= 0) {
        return { ok: true, result: { status: "pending", pair_id: rec.id, code: rec.code, expires_in_s: Math.ceil((rec.expires - now) / 1000) } };
      }
      await this.waitOn(this.pairWaiters, id, remaining);
    }
  }

  private async onPairResult(frame: Record<string, unknown>) {
    const id = await this.kv.get<string>(K.pairCurrent);
    if (!id) return;
    const rec = await this.kv.get<PairRecord>(K.pair(id));
    if (!rec || rec.status !== "pending" || typeof frame.req !== "string" || !safeEqual(rec.req, frame.req)) return;
    if (this.clock.now() >= rec.expires) {
      rec.status = "timeout";
    } else if (frame.ok === true) {
      const token = `${this.deviceId}.${b64urlEncode(randomBytes(32))}`;
      await this.kv.put<TokenRecord>(K.token(await sha256Hex(token)), { agent_name: rec.agent_name, created: this.clock.now() });
      rec.status = "approved";
      rec.token = token;
    } else {
      rec.status = frame.reason === "timeout" ? "timeout" : "rejected";
    }
    await this.kv.put(K.pair(id), rec);
    await this.kv.delete(K.pairCurrent);
    // The device rotates its link code after a pairing; the old one is dead
    // until the device reports the new one (here, in hello, or link_code).
    if (rec.status === "approved") await this.kv.delete(K.link);
    if (typeof frame.link_code === "string") await this.onLinkCode(frame.link_code);
    this.notify(this.pairWaiters, id);
  }

  // ---------------- jobs ----------------

  async job(token: string, jobId: string, waitMs: number): Promise<Outcome<JobView>> {
    if (!(await this.authorize(token))) return fail("unauthorized");
    if (typeof jobId !== "string" || !jobId.startsWith(`${this.deviceId}.j_`)) return fail("not_found");
    const deadline = this.clock.now() + Math.max(0, Math.min(waitMs, 8000));
    for (;;) {
      const rec = await this.kv.get<JobRecord>(K.job(jobId));
      if (!rec) return fail("not_found");
      if (!TERMINAL.includes(rec.status) && !this.tasks.has(jobId)) {
        await this.finishJob(jobId, { status: "failed", error: "interrupted" });
        continue;
      }
      const remaining = deadline - this.clock.now();
      if (TERMINAL.includes(rec.status) || remaining <= 0) return { ok: true, result: viewOf(rec) };
      await this.waitOn(this.jobWaiters, jobId, remaining);
    }
  }

  private async startJob(kind: "say" | "listen", args: JsonObject): Promise<Outcome> {
    if (this.audioJob) return fail("audio_busy", `job ${this.audioJob} is still running`);
    const id = `${this.deviceId}.j_${b64urlEncode(randomBytes(12))}`;
    const now = this.clock.now();
    this.audioJob = id;
    this.tasks.add(id);
    await this.kv.put<JobRecord>(K.job(id), { job: id, kind, status: "queued", created: now, updated: now });
    void this.purgeJobs(now);
    const run = kind === "say" ? this.runSay(id, args) : this.runListen(id, args);
    run
      .catch((e: unknown) => this.finishJob(id, { status: "failed", error: errorCode(e) }))
      .finally(() => {
        this.tasks.delete(id);
        if (this.audioJob === id) this.audioJob = undefined;
      });
    return { ok: true, result: { job: id } };
  }

  private async setJob(id: string, patch: Partial<JobRecord>) {
    const rec = await this.kv.get<JobRecord>(K.job(id));
    if (!rec || TERMINAL.includes(rec.status)) return;
    await this.kv.put(K.job(id), { ...rec, ...patch, updated: this.clock.now() });
  }

  private async finishJob(id: string, patch: Partial<JobRecord> & { status: "done" | "failed" }) {
    await this.setJob(id, patch);
    this.tasks.delete(id);
    if (this.audioJob === id) this.audioJob = undefined;
    this.notify(this.jobWaiters, id);
  }

  private async purgeJobs(now: number) {
    for (const [key, rec] of await this.kv.list<JobRecord>("job:")) {
      if (now - rec.created > this.t.jobTtlMs && !this.tasks.has(rec.job)) await this.kv.delete(key);
    }
  }

  private async runSay(id: string, args: JsonObject) {
    await this.setJob(id, { status: "running" });
    const text = args.text as string;
    const voice = typeof args.voice === "string" ? args.voice : undefined;
    const pcm = await withTimeout(this.clock, this.voice.tts(text, { voice }), this.t.ttsTimeoutMs, "tts_timeout");
    const ms = durationMs(pcm.length);
    const sock = this.liveSocket();
    if (!sock) return this.finishJob(id, { status: "failed", error: "device_offline" });
    const deviceArgs: JsonObject = { job: id, text, ms, rate: SAMPLE_RATE };
    if (voice) deviceArgs.voice = voice;
    const res = await this.sendCmd("say", deviceArgs);
    if (!res.ok) return this.finishJob(id, { status: "failed", error: res.error });

    let done = false;
    const doneSignal = new Promise<void>((resolve) => this.sayDone.set(id, () => ((done = true), resolve())));
    try {
      const chunk = this.t.sayChunkSamples;
      const frames = Math.max(1, Math.ceil(pcm.length / chunk));
      const chunkMs = (chunk * 1000) / SAMPLE_RATE;
      const start = this.clock.now();
      for (let seq = 0; seq < frames; seq++) {
        if (done) break;
        const due = start + (seq - this.t.sayLeadFrames) * chunkMs;
        const wait = due - this.clock.now();
        if (wait > 0) await sleep(this.clock, wait);
        const live = this.liveSocket();
        if (!live || !live.same(sock)) return this.finishJob(id, { status: "failed", error: "device_offline" });
        const payload = pcm16ToBytes(pcm.subarray(seq * chunk, (seq + 1) * chunk));
        live.sendBinary(encodeAudioFrame({ s: id, seq, kind: "pcm16", rate: SAMPLE_RATE, end: seq === frames - 1 }, payload));
      }
      // Playback ends ~ when the stream would have, unless the device says so first.
      const left = start + ms - this.clock.now();
      if (left > 0 && !done) await Promise.race([doneSignal, sleep(this.clock, left)]);
    } finally {
      this.sayDone.delete(id);
    }
    await this.finishJob(id, { status: "done", result: { ms, text } });
  }

  private async runListen(id: string, args: JsonObject) {
    const ms = args.ms as number;
    await this.setJob(id, { status: "running" });
    const buf: ListenBuffer = { job: id, chunks: [], bytes: 0, ended: false };
    this.listenBuf = buf;
    try {
      const res = await this.sendCmd("listen", { job: id, ms, rate: SAMPLE_RATE });
      if (!res.ok) return this.finishJob(id, { status: "failed", error: res.error });
      if (!buf.ended) {
        await new Promise<void>((resolve) => {
          const timer = this.clock.setTimeout(resolve, ms + this.t.listenGraceMs);
          buf.wake = () => {
            this.clock.clearTimeout(timer);
            resolve();
          };
        });
      }
    } finally {
      if (this.listenBuf === buf) this.listenBuf = undefined;
    }
    if (!this.liveSocket() && buf.bytes === 0) return this.finishJob(id, { status: "failed", error: "device_offline" });
    if (buf.bytes === 0) return this.finishJob(id, { status: "failed", error: "no_audio" });
    const all = new Uint8Array(buf.bytes);
    let o = 0;
    for (const c of buf.chunks) {
      all.set(c, o);
      o += c.byteLength;
    }
    const pcm = bytesToPcm16(all);
    const text = await withTimeout(this.clock, this.voice.stt(pcm), this.t.sttTimeoutMs, "stt_timeout");
    await this.finishJob(id, { status: "done", result: { text, duration_ms: durationMs(pcm.length), ...levels(pcm) } });
  }

  private onAudio(bytes: Uint8Array) {
    const frame = decodeAudioFrame(bytes);
    const buf = this.listenBuf;
    if (!frame || !buf || buf.ended || frame.header.s !== buf.job) return;
    if (buf.bytes + frame.payload.byteLength <= MAX_LISTEN_BYTES && frame.payload.byteLength > 0) {
      buf.chunks.push(frame.payload.slice());
      buf.bytes += frame.payload.byteLength;
    }
    if (frame.header.end) {
      buf.ended = true;
      buf.wake?.();
    }
  }

  // ---------------- waiter plumbing ----------------

  private waitOn(map: Map<string, Set<() => void>>, key: string, ms: number): Promise<void> {
    return new Promise((resolve) => {
      let set = map.get(key);
      if (!set) map.set(key, (set = new Set()));
      const wake = () => {
        this.clock.clearTimeout(timer);
        set!.delete(wake);
        if (set!.size === 0) map.delete(key);
        resolve();
      };
      const timer = this.clock.setTimeout(wake, ms);
      set.add(wake);
    });
  }

  private notify(map: Map<string, Set<() => void>>, key: string) {
    for (const wake of [...(map.get(key) ?? [])]) wake();
  }
}

function viewOf(rec: JobRecord): JobView {
  const view: JobView = { job: rec.job, kind: rec.kind, status: rec.status };
  if (rec.result) view.result = rec.result;
  if (rec.error) view.error = rec.error;
  return view;
}

class CodedError extends Error {
  constructor(readonly code: string) {
    super(code);
  }
}

function errorCode(e: unknown): string {
  if (e instanceof CodedError) return e.code;
  const msg = e instanceof Error ? e.message : String(e);
  return msg.slice(0, 200) || "internal";
}

function withTimeout<T>(clock: Clock, p: Promise<T>, ms: number, code: string): Promise<T> {
  return new Promise<T>((resolve, reject) => {
    const timer = clock.setTimeout(() => reject(new CodedError(code)), ms);
    p.then(
      (v) => {
        clock.clearTimeout(timer);
        resolve(v);
      },
      (e) => {
        clock.clearTimeout(timer);
        reject(e);
      },
    );
  });
}
