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
//
// Agent events (shake/tap/button) are storage-backed: an `ev:` ring, a
// per-token read cursor `cur:<token hash>` and a webhook delivery queue
// `whq:` driven by the host's alarm. All survive hibernation.

import { type AgentApi, type EventsRequest, type JobView, type Outcome, type PairRequest, type PairResult, TERMINAL, type WebhookRequest } from "./api.js";
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
import { AGENT_EVENT_TYPES, type AgentEvent, INBOX_LIMITS, SAMPLE_EVENTS, agentEventData, eventId } from "./events.js";
import {
  type DeliveryError,
  type GuardResult,
  type RenderedRequest,
  type Resolver,
  WEBHOOK_LIMITS,
  WebhookFailure,
  categorize,
  type WebhookRecord,
  guardWebhookUrl,
  newWebhookSecret,
  parseWebhookSpec,
  redactRendered,
  redactUrl,
  redactWebhook,
  renderRequest,
  signatureHeader,
} from "./webhook.js";
import type { VoiceProvider } from "../voice/types.js";

export interface SocketMeta {
  stage: "hello" | "live";
  opened: number;
  fw?: string;
  caps?: string[];
  board?: string;
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
  /** Host alarm: run `room.alarm()` at (or soon after) `at`. Replaces any earlier schedule. Without it webhooks are not delivered. */
  scheduler?: { setAlarm(at: number): Promise<void> };
  /** Outbound fetch for webhooks (default: global fetch). */
  fetch?: (req: Request) => Promise<Response>;
  /** Resolves a webhook host to its addresses for the SSRF guard; absent = literal checks only. */
  resolveHost?: Resolver;
}

interface DeviceRecord {
  key_hash: string;
  claimed_at: number;
}
interface TokenRecord {
  agent_name: string;
  created: number;
  webhook?: WebhookRecord;
  /**
   * Webhook configuration revision: +1 on every successful set and delete, so
   * "never configured" and "deleted" differ. Registrations compare it inside
   * one KV.update (atomic read-check-write). Revocation deletes the record.
   */
  wh_rev?: number;
}

type WebhookWrite = "ok" | "gone" | "conflict";
interface Delivery {
  tok: string;
  seq: number;
  attempt: number;
  due: number;
}
interface RateRecord {
  start: number;
  n: number;
  dropped: number;
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
  evSeq: "evseq",
  evRate: "evrate",
  ev: (seq: number) => `ev:${String(seq).padStart(12, "0")}`,
  cur: (hash: string) => `cur:${hash}`,
  whq: (hash: string, seq: number) => `whq:${hash}:${String(seq).padStart(12, "0")}`,
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
  private readonly scheduler: RoomDeps["scheduler"];
  private readonly fetcher: NonNullable<RoomDeps["fetch"]>;
  private readonly resolveHost: RoomDeps["resolveHost"];

  // ---- in-flight work only (see header comment) ----
  private readonly pendingCmds = new Map<string, PendingCmd>();
  private readonly pairWaiters = new Map<string, Set<() => void>>();
  private readonly jobWaiters = new Map<string, Set<() => void>>();
  private readonly tasks = new Set<string>();
  private readonly sayDone = new Map<string, () => void>();
  private listenBuf: ListenBuffer | undefined;
  private audioJob: string | undefined;
  private readonly lastEvents = new Map<string, { data: unknown; at: number }>();
  private readonly inboxWaiters = new Map<string, Set<() => void>>();
  /** Outstanding long-polls per token hash (admission control). */
  private readonly polls = new Map<string, number>();
  private evChain: Promise<void> = Promise.resolve();
  private cmdSeq = 0;

  constructor(deps: RoomDeps) {
    this.deviceId = deps.deviceId;
    this.kv = deps.kv;
    this.clock = deps.clock;
    this.voice = deps.voice;
    this.host = deps.host;
    this.t = { ...DEFAULT_TIMINGS, ...deps.timings };
    this.log = deps.log ?? (() => {});
    this.scheduler = deps.scheduler;
    this.fetcher = deps.fetch ?? ((req) => globalThis.fetch(req));
    this.resolveHost = deps.resolveHost;
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
    sock.setMeta({ stage: "live", opened: sock.getMeta()?.opened ?? this.clock.now(), fw: hello.fw, caps: hello.caps, board: hello.board, since: this.clock.now() });
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
    const clean = agentEventData(frame.name, data);
    if (clean) await this.appendEvent(frame.name, clean);
  }

  private async onLinkCode(code: unknown) {
    if (typeof code === "string" && LINK_CODE_RE.test(code)) await this.kv.put(K.link, { code, at: this.clock.now() });
  }

  private async onForget(sock: DeviceSocket) {
    for (const prefix of ["tok:", "pair:", "job:", "ev:", "cur:", "whq:"]) {
      for (const key of (await this.kv.list(prefix)).keys()) await this.kv.delete(key);
    }
    for (const key of [K.device, K.link, K.pairCurrent, K.evSeq, K.evRate]) await this.kv.delete(key);
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

  private async auth(token: string): Promise<{ rec: TokenRecord; hash: string } | undefined> {
    if (typeof token !== "string" || !token.startsWith(`${this.deviceId}.`)) return undefined;
    const hash = await sha256Hex(token);
    const rec = await this.kv.get<TokenRecord>(K.token(hash));
    return rec ? { rec, hash } : undefined;
  }

  async revoke(token: string): Promise<Outcome> {
    const a = await this.auth(token);
    if (!a) return fail("unauthorized");
    await this.kv.delete(K.token(a.hash));
    await this.kv.delete(K.cur(a.hash));
    await this.dropDeliveries(a.hash);
    return { ok: true, result: { revoked: true } };
  }

  async command(token: string, name: string, args: unknown): Promise<Outcome> {
    const a = await this.auth(token);
    if (!a) return fail("unauthorized");
    return this.withInbox(a.hash, await this.runCommand(a.rec, name, args));
  }

  private async runCommand(agent: TokenRecord, name: string, args: unknown): Promise<Outcome> {
    let cmd;
    try {
      cmd = validateCommand(name, args);
    } catch (e) {
      if (e instanceof CommandError) return fail(e.code, e.message);
      throw e;
    }
    const live = this.liveSocket();
    if (!live) return fail("device_offline");
    if (cmd.job) {
      // A device that lists its caps and leaves say/listen out has no speaker/mic
      // (firmware/boards/): refuse before spending a TTS call. No caps = older firmware.
      const caps = live.getMeta()?.caps;
      if (caps && !caps.includes(cmd.name)) {
        return fail("unsupported", `this device has no ${cmd.name === "say" ? "speaker" : "microphone"}`);
      }
      return this.startJob(cmd.name as "say" | "listen", cmd.args);
    }
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
        relay: {
          device_id: this.deviceId,
          online: !!sock,
          fw: meta?.fw ?? null,
          board: meta?.board ?? null,
          caps: meta?.caps ?? [],
          connected_since: meta?.since ?? null,
          voice: this.voice.name,
          events,
          webhook: agent.webhook ? (redactWebhook(agent.webhook) as JsonObject) : null,
        },
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
      const hash = await sha256Hex(token);
      await this.kv.put<TokenRecord>(K.token(hash), { agent_name: rec.agent_name, created: this.clock.now() });
      // A new agent starts at the head: it hears what happens from now on.
      await this.kv.put(K.cur(hash), (await this.kv.get<number>(K.evSeq)) ?? 0);
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
    const a = await this.auth(token);
    if (!a) return fail("unauthorized");
    const out = await this.runJob(jobId, waitMs);
    // A failed job is an error to every surface: nothing rides along on it.
    if (out.ok && out.result.status === "failed") return out;
    return this.withInbox(a.hash, out);
  }

  private async runJob(jobId: string, waitMs: number): Promise<Outcome<JobView>> {
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

  // ---------------- agent events: inbox ----------------

  /** Serialized so concurrent frames cannot race the sequence counter. */
  private appendEvent(type: string, data: JsonObject): Promise<void> {
    const run = this.evChain.then(() => this.doAppend(type, data));
    this.evChain = run.catch(() => {});
    return run;
  }

  private async doAppend(type: string, data: JsonObject) {
    const now = this.clock.now();
    const rate = (await this.kv.get<RateRecord>(K.evRate)) ?? { start: now, n: 0, dropped: 0 };
    if (now - rate.start >= 60000) Object.assign(rate, { start: now, n: 0 });
    if (rate.n >= INBOX_LIMITS.perMinute) {
      rate.dropped++;
      await this.kv.put(K.evRate, rate);
      return;
    }
    rate.n++;
    await this.kv.put(K.evRate, rate);
    const seq = ((await this.kv.get<number>(K.evSeq)) ?? 0) + 1;
    await this.kv.put(K.evSeq, seq);
    const ev: AgentEvent = { id: eventId(seq), type, ts: now, data };
    await this.kv.put(K.ev(seq), ev);
    for (const [key, old] of await this.kv.list<AgentEvent>("ev:")) {
      const oldSeq = Number(key.slice(3));
      if (oldSeq <= seq - INBOX_LIMITS.keep || now - old.ts > INBOX_LIMITS.maxAgeMs) await this.kv.delete(key);
    }
    this.notify(this.inboxWaiters, "inbox");
    let queued = false;
    for (const [key, tok] of await this.kv.list<TokenRecord>("tok:")) {
      if (!tok.webhook || tok.webhook.disabled || (tok.webhook.events && !tok.webhook.events.includes(type))) continue;
      const hash = key.slice(4);
      await this.kv.put<Delivery>(K.whq(hash, seq), { tok: hash, seq, attempt: 0, due: now });
      queued = true;
    }
    if (queued) await this.scheduleDeliveries();
  }

  /** Unread events after `since` (default: the agent's cursor), oldest first, at most `limit`. */
  private async readInbox(hash: string, since: number | undefined, limit: number) {
    const head = (await this.kv.get<number>(K.evSeq)) ?? 0;
    let cursor = await this.kv.get<number>(K.cur(hash));
    if (cursor === undefined) {
      cursor = head; // tokens minted before the inbox existed start at the head
      await this.kv.put(K.cur(hash), cursor);
    }
    const from = Math.min(since ?? cursor, head);
    if (from >= head) return { events: [] as AgentEvent[], next: from, more: false, cursor };
    const now = this.clock.now();
    const unread = [...(await this.kv.list<AgentEvent>("ev:")).entries()]
      .map(([key, ev]) => ({ seq: Number(key.slice(3)), ev }))
      .filter((e) => e.seq > from && now - e.ev.ts <= INBOX_LIMITS.maxAgeMs);
    const page = unread.slice(0, limit);
    return { events: page.map((e) => e.ev), next: page.length ? page[page.length - 1].seq : from, more: unread.length > page.length, cursor };
  }

  private async advance(hash: string, cursor: number, next: number) {
    if (next > cursor) await this.kv.put(K.cur(hash), next);
  }

  /**
   * Ride-along: attach this agent's unread events to a SUCCESSFUL outcome and
   * mark them read. Errors carry nothing and move nothing: clients often drop
   * error bodies, and a read cursor must only move when the events were
   * actually handed over. Surfaces must keep `inbox` through any reshaping.
   */
  private async withInbox<T>(hash: string, out: Outcome<T>): Promise<Outcome<T>> {
    if (!out.ok) return out;
    const r = await this.readInbox(hash, undefined, INBOX_LIMITS.piggyback);
    if (r.events.length === 0) return out;
    await this.advance(hash, r.cursor, r.next);
    return { ...out, inbox: { events: r.events, more: r.more, now: this.clock.now() } };
  }

  async events(token: string, req: EventsRequest): Promise<Outcome> {
    const a = await this.auth(token);
    if (!a) return fail("unauthorized");
    if (req.since !== undefined && (!Number.isInteger(req.since) || req.since < 0)) return fail("invalid_args", "since must be an event id like e42");
    const waitMs = Math.max(0, Math.min(req.wait_ms, INBOX_LIMITS.restWaitS * 1000));
    // Admission: bounded concurrent long-polls per token and per device.
    let admitted = false;
    if (waitMs > 0) {
      const mine = this.polls.get(a.hash) ?? 0;
      let total = 0;
      for (const n of this.polls.values()) total += n;
      if (mine >= INBOX_LIMITS.pollsPerToken || total >= INBOX_LIMITS.pollsPerDevice) {
        return fail("too_many_polls", `at most ${INBOX_LIMITS.pollsPerToken} concurrent waits per agent and ${INBOX_LIMITS.pollsPerDevice} per device; wait for one to return`);
      }
      this.polls.set(a.hash, mine + 1);
      admitted = true;
    }
    try {
      const deadline = this.clock.now() + waitMs;
      for (;;) {
        const r = await this.readInbox(a.hash, req.since, INBOX_LIMITS.page);
        const remaining = deadline - this.clock.now();
        if (r.events.length > 0 || remaining <= 0 || req.signal?.aborted) {
          if (req.signal?.aborted) return fail("cancelled"); // nobody is listening: hand over nothing
          await this.advance(a.hash, r.cursor, r.next);
          return { ok: true, result: { events: r.events as unknown as JsonObject[], next: eventId(r.next), more: r.more } };
        }
        await this.waitOn(this.inboxWaiters, "inbox", remaining, req.signal);
      }
    } finally {
      if (admitted) {
        const n = (this.polls.get(a.hash) ?? 1) - 1;
        if (n > 0) this.polls.set(a.hash, n);
        else this.polls.delete(a.hash);
      }
    }
  }

  // ---------------- agent events: webhooks ----------------

  async webhook(token: string, req: WebhookRequest): Promise<Outcome> {
    const a = await this.auth(token);
    if (!a) return fail("unauthorized");
    const key = K.token(a.hash);
    if (req.action === "get") {
      return this.withInbox(a.hash, { ok: true, result: { webhook: a.rec.webhook ? (redactWebhook(a.rec.webhook) as JsonObject) : null } });
    }
    if (req.action === "delete") {
      const had = await this.kv.update<TokenRecord, boolean | undefined>(key, (cur) => {
        if (!cur) return { result: undefined };
        const { webhook, ...rest } = cur;
        return { next: { ...rest, wh_rev: (cur.wh_rev ?? 0) + 1 }, result: !!webhook };
      });
      if (had === undefined) return fail("unauthorized");
      await this.dropDeliveries(a.hash);
      return this.withInbox(a.hash, { ok: true, result: { removed: had } });
    }
    const parsed = parseWebhookSpec(req.spec);
    if (!parsed.ok) return fail("invalid_args", parsed.message);
    // Revision this registration was based on; compared atomically when writing.
    const revision = a.rec.wh_rev ?? 0;
    // Dry run against every event type the webhook will receive, to URLs that pass the guard.
    const types = parsed.value.events ?? AGENT_EVENT_TYPES;
    const rendered: RenderedRequest[] = [];
    for (const type of types) {
      try {
        rendered.push(renderRequest(parsed.value, this.deviceId, SAMPLE_EVENTS[type]));
      } catch (e) {
        return fail("invalid_args", `for a ${type} event: ${e instanceof Error ? e.message : String(e)}`);
      }
    }
    // One deadline over the whole validation; each host resolved once.
    const resolve = this.resolveHost;
    const cache = new Map<string, Promise<string[]>>();
    const cached: Resolver | undefined = resolve && ((host, signal) => {
      if (!cache.has(host)) cache.set(host, resolve(host, signal));
      return cache.get(host)!;
    });
    const refused = await this.withDeadline(async (signal): Promise<GuardResult | undefined> => {
      for (const url of new Set(rendered.map((r) => r.url))) {
        const g = await guardWebhookUrl(url, cached, signal);
        if (!g.ok) return g;
      }
      return undefined;
    }).catch((): GuardResult => ({ ok: false, message: "url host did not resolve in time", category: "timeout" }));
    if (refused && !refused.ok) return fail("invalid_args", refused.message);
    // Atomic compare-and-write on the revision: a revoke (record gone), another
    // registration or a delete since `revision` was read makes this one lose.
    const secret = newWebhookSecret();
    const webhook: WebhookRecord = { ...parsed.value, secret, created: this.clock.now(), fails: 0 };
    const outcome = await this.kv.update<TokenRecord, WebhookWrite>(key, (cur) => {
      if (!cur) return { result: "gone" };
      if ((cur.wh_rev ?? 0) !== revision) return { result: "conflict" };
      return { next: { ...cur, webhook, wh_rev: revision + 1 }, result: "ok" };
    });
    if (outcome === "gone") return fail("unauthorized");
    if (outcome === "conflict") return fail("conflict", "the webhook changed while this registration was being validated; read it and retry");
    await this.dropDeliveries(a.hash);
    return this.withInbox(a.hash, {
      ok: true,
      result: {
        webhook: redactWebhook(webhook) as JsonObject,
        secret,
        secret_note: "Shown once. Verify Molty-Signature: t=<unix ms>,v2=hex(hmac_sha256(secret, t + '\\n' + METHOD + '\\n' + path?query + '\\n' + body)).",
        dry_run: redactRendered(rendered[0]) as unknown as JsonObject,
      },
    });
  }

  private async dropDeliveries(hash: string) {
    for (const key of (await this.kv.list(`whq:${hash}:`)).keys()) await this.kv.delete(key);
  }

  private async scheduleDeliveries() {
    if (!this.scheduler) return;
    let next = Infinity;
    for (const d of (await this.kv.list<Delivery>("whq:")).values()) next = Math.min(next, d.due);
    if (next !== Infinity) await this.scheduler.setAlarm(Math.max(next, this.clock.now()));
  }

  /** Host alarm handler: delivers due webhook requests, then reschedules. */
  async alarm(): Promise<void> {
    const now = this.clock.now();
    const byToken = new Map<string, Array<[string, Delivery]>>();
    for (const [key, d] of await this.kv.list<Delivery>("whq:")) {
      if (d.due > now) continue;
      const list = byToken.get(d.tok) ?? [];
      if (list.length < 10) list.push([key, d]);
      byToken.set(d.tok, list);
    }
    // One agent's deliveries in order (they share a failure counter); agents in parallel.
    await Promise.all([...byToken.values()].map(async (list) => {
      for (const [key, d] of list) await this.deliver(key, d);
    }));
    await this.scheduleDeliveries();
  }

  /**
   * One delivery attempt. Webhook acks are separate from the read cursor: a
   * delivered event stays readable by pull/ride-along (the inbox is the source
   * of truth; agents dedupe by id), so an event that exhausts its retries is
   * never lost to the agent.
   */
  private async deliver(key: string, d: Delivery) {
    const tokKey = K.token(d.tok);
    const before = await this.kv.get<TokenRecord>(tokKey);
    const ev = await this.kv.get<AgentEvent>(K.ev(d.seq));
    if (!before?.webhook || before.webhook.disabled || !ev || (before.webhook.events && !before.webhook.events.includes(ev.type))) return this.kv.delete(key);
    const hook = before.webhook;
    let error: DeliveryError | undefined;
    try {
      // One deadline over DNS, signing and the request itself.
      await this.withDeadline(async (signal) => {
        let r;
        try {
          r = renderRequest(hook, this.deviceId, ev);
        } catch {
          throw new WebhookFailure("render_failed");
        }
        const guard = await guardWebhookUrl(r.url, this.resolveHost, signal);
        if (!guard.ok) throw new WebhookFailure(guard.category);
        const headers = { ...r.headers, "molty-signature": await signatureHeader(hook.secret, r.method, r.url, r.body, this.clock.now()) };
        const res = await this.fetcher(new Request(r.url, { method: r.method, headers, body: r.method === "GET" ? undefined : r.body, redirect: "manual", signal }));
        try {
          await res.body?.cancel();
        } catch {
          /* ignore */
        }
        if (res.status < 200 || res.status > 299) throw new WebhookFailure(`http_${res.status}`);
      });
    } catch (e) {
      error = categorize(e);
    }
    // Delivery stats are written atomically onto the CURRENT record, and only if
    // it still holds this webhook: never resurrect or overwrite a newer config.
    const now = this.clock.now();
    const w = await this.kv.update<TokenRecord, WebhookRecord | undefined>(tokKey, (cur) => {
      if (!cur?.webhook || cur.webhook.secret !== hook.secret) return { result: undefined };
      const next = { ...cur.webhook };
      if (!error) {
        next.fails = 0;
        next.last_ok = now;
        delete next.last_error;
      } else {
        next.fails++;
        next.last_error = error;
        if (next.fails >= WEBHOOK_LIMITS.maxFails) next.disabled = `disabled after ${next.fails} consecutive failed deliveries; register it again to re-enable`;
      }
      return { next: { ...cur, webhook: next }, result: next };
    });
    if (!w || !error) return this.kv.delete(key); // removed/replaced meanwhile, or delivered
    this.log("webhook delivery failed", { device: this.deviceId, url: redactUrl(w.url), error, fails: w.fails });
    if (w.disabled) await this.dropDeliveries(d.tok);
    else if (d.attempt < WEBHOOK_LIMITS.retryMs.length) await this.kv.put<Delivery>(key, { ...d, attempt: d.attempt + 1, due: now + WEBHOOK_LIMITS.retryMs[d.attempt] });
    else await this.kv.delete(key);
  }

  /** Runs `fn` under one deadline: on expiry its signal aborts and the call fails as `timeout`. */
  private withDeadline<T>(fn: (signal: AbortSignal) => Promise<T>): Promise<T> {
    const ctl = new AbortController();
    return new Promise<T>((resolve, reject) => {
      const timer = this.clock.setTimeout(() => {
        ctl.abort();
        reject(new WebhookFailure("timeout"));
      }, WEBHOOK_LIMITS.timeoutMs);
      fn(ctl.signal).then(
        (v) => {
          this.clock.clearTimeout(timer);
          resolve(v);
        },
        (e: unknown) => {
          this.clock.clearTimeout(timer);
          reject(e);
        },
      );
    });
  }

  // ---------------- waiter plumbing ----------------

  private waitOn(map: Map<string, Set<() => void>>, key: string, ms: number, signal?: AbortSignal): Promise<void> {
    return new Promise((resolve) => {
      let set = map.get(key);
      if (!set) map.set(key, (set = new Set()));
      const wake = () => {
        this.clock.clearTimeout(timer);
        signal?.removeEventListener("abort", wake);
        set!.delete(wake);
        if (set!.size === 0) map.delete(key);
        resolve();
      };
      const timer = this.clock.setTimeout(wake, ms);
      set.add(wake);
      // A caller that went away frees its waiter now, not at the deadline.
      signal?.addEventListener("abort", wake, { once: true });
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
