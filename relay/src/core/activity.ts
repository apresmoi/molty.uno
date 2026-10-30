// SPDX-License-Identifier: MIT
// Activity log: one timeline of everything that happens on a Molty, and which agent did it
// (docs/AGENT-LINK.md "Activity log"). Host-neutral, storage-backed (survives hibernation).
//
// What lands here: one entry per authenticated agent call (op, args summary with secrets
// stripped, outcome), one per job end (stage timings, voice provider + fallback, reason,
// heard / said), device events (connect / disconnect with close codes, refused hellos,
// shake / tap / button), pairing changes, voice box connect / disconnect / unpair, owner
// setting changes, and the device's own log lines (device -> relay `log` frames), which
// carry the job id of the work they belong to. Device uptime becomes wall time here.
//
// Storage: entries are appended into chunks `al:c:<n>` (<= CHUNK_ENTRIES entries and
// <= CHUNK_BYTES of JSON each); `al:h` points at the head and tail chunk. An append is
// one read of the pointer, one read and one write of the head chunk (+ a pointer write
// and the trim only when a chunk fills). Retention: MAX_AGE (24 h, enforced on read and
// trimmed on write) and at most MAX_CHUNKS chunks, whichever bites first. Costs, see
// LIMITS below.
//
// Privacy: transcripts (what Molty said, what the human said, text shown) are kept unless
// the owner turns "log transcripts" off (`al:cfg`); then they are removed BEFORE the write,
// so they never reach storage. Secrets (tokens, keys, webhook headers / bodies / full URLs,
// link codes) are never written. An agent reads only its own entries, the device lines of
// its own jobs and the device's connects / disconnects around them (`visible`).

import type { JsonObject } from "./protocol.js";
import { isRecord } from "./protocol.js";
import type { Clock, KV } from "./store.js";
import { redactUrl } from "./webhook.js";

export const ACTIVITY_LIMITS = {
  /** Entries older than this are never returned and are trimmed on write. */
  maxAgeMs: 24 * 60 * 60 * 1000,
  /**
   * Chunk size. A Durable Object (SQLite-backed) bills a write per row, whatever its size
   * (up to 2 MB): one append = 1 row written; bigger chunks would only cost CPU on every
   * rewrite of the head. 16 KB / 64 entries keeps that rewrite well under a millisecond.
   */
  chunkBytes: 16 * 1024,
  chunkEntries: 64,
  /**
   * 48 chunks: <= 768 KB and <= 3072 entries per device. A busy device (500 turns a day,
   * ~10 entries each with its device lines) stays inside 24 h; storage <= 0.75 MB/device
   * ($0.20/GB-month: ~$0.0002), writes ~1 row per entry (a few thousand a day at most).
   */
  maxChunks: 48,
  /** Page sizes for GET .../activity. */
  page: 200,
  maxPage: 500,
  /** Device `log` frames: lines per frame the welcome asks for, lines accepted per frame (slack), per minute (in memory), line length. */
  deviceLinesAdvertised: 48,
  deviceLinesPerFrame: 64,
  deviceLinesPerMinute: 240,
  lineMax: 200,
  /** Device events (connect / disconnect, gestures) recorded per minute (in memory). */
  deviceEventsPerMinute: 60,
  /** Refused hellos per minute: unauthenticated, so anyone who knows the id can cause them. */
  refusedPerMinute: 2,
  /** heard / said / text kept per entry (when transcripts are on). */
  textMax: 500,
  /** Other strings in an args summary. */
  argMax: 120,
} as const;

export const ACTIVITY_KINDS = ["call", "job", "device", "device_log", "pair", "voice_box", "settings"] as const;
export type ActivityKind = (typeof ACTIVITY_KINDS)[number];

/** One timeline entry. `who` (the agent token's hash prefix) is internal: never returned. */
export interface ActivityEntry {
  id: string;
  ts: number;
  kind: ActivityKind;
  ev?: string;
  agent?: string;
  who?: string;
  job?: string;
  op?: string;
  args?: JsonObject;
  ok?: boolean;
  error?: string;
  message?: string;
  ms?: number;
  data?: JsonObject;
  redacted?: true;
}
export type ActivityDraft = Omit<ActivityEntry, "id">;

/** Who started something: the agent's name and its token hash prefix (isolation key). */
export interface Who {
  who: string;
  agent: string;
}

export interface ActivityQuery {
  /** "a<seq>": entries after it (oldest first); a number: entries at or after that unix ms. */
  since?: string | number;
  limit?: number;
  agent?: string;
  kind?: string[];
  job?: string;
  errors?: boolean;
}

export interface ActivityPage {
  entries: JsonObject[];
  next: string;
  more: boolean;
}

interface Head {
  head: number;
  tail: number;
}
interface Chunk {
  n: number;
  /** Last entry seq assigned (in this chunk or, when it is empty, before it). */
  seq: number;
  bytes: number;
  entries: ActivityEntry[];
}
interface Cfg {
  transcripts: boolean;
}
/** Highest device line seq stored, per boot (the last few boots). */
type DevCursor = Record<string, number>;

const KEY = {
  head: "al:h",
  chunk: (n: number) => `al:c:${String(n).padStart(8, "0")}`,
  cfg: "al:cfg",
  dev: "al:dev",
};
/** Every activity key starts with this (onForget wipes the prefix). */
export const ACTIVITY_PREFIX = "al:";

const SECRET_KEY = /secret|token|key|auth|passw|cookie|signature|credential|link_code|^headers$|^code$/i;
/** An agent token, or anything shaped like a bearer credential. */
const SECRET_VALUE = /^(?:Bearer\s+\S+|molty-[a-z2-9]{10}\.[A-Za-z0-9_-]{20,}|[A-Za-z0-9_-]{40,})$/;
/** Transcript-bearing fields: of call args (say / talk / ask / show) and of job data. */
const TRANSCRIPT_ARGS = ["text", "say"];
const TRANSCRIPT_DATA = ["heard", "said", "text"];
/** Polls: logged only when they fail (a long-polling agent would wash the log out). */
const QUIET_OPS = new Set(["wait", "events", "activity"]);
const JOB_TAG_RE = /^[A-Za-z0-9_-]{1,32}$/;
const clip = (s: string, n: number) => (s.length > n ? `${s.slice(0, n - 1)}…` : s);
const printable = (s: string) => s.replace(/[\u0000-\u001f\u007f]/g, " ");

/**
 * Args summary: scalars and short arrays, depth <= 3, strings clipped, every secret-looking
 * key or value replaced by "[secret]". Webhook specs keep only their redacted shape.
 */
export function summarizeArgs(op: string, args: unknown): JsonObject | undefined {
  if (!isRecord(args)) return undefined;
  if (op === "webhook") {
    const spec = isRecord(args.spec) ? args.spec : {};
    const out: JsonObject = { action: typeof args.action === "string" ? args.action : "get" };
    if (typeof spec.url === "string") out.url = redactUrl(spec.url);
    if (typeof spec.method === "string") out.method = clip(spec.method, 8);
    if (isRecord(spec.headers)) out.headers = Object.keys(spec.headers).length;
    if (spec.body !== undefined) out.custom_body = true;
    if (Array.isArray(spec.events)) out.events = spec.events.filter((e): e is string => typeof e === "string").slice(0, 8).map((e) => clip(e, 32));
    return out;
  }
  const walk = (v: unknown, key: string, depth: number): JsonObject[string] | undefined => {
    if (SECRET_KEY.test(key)) return "[secret]";
    if (v === null || typeof v === "boolean") return v;
    if (typeof v === "number") return Number.isFinite(v) ? v : null;
    if (typeof v === "string") {
      if (SECRET_VALUE.test(v.trim())) return "[secret]";
      return clip(printable(v), TRANSCRIPT_ARGS.includes(key) ? ACTIVITY_LIMITS.textMax : ACTIVITY_LIMITS.argMax);
    }
    if (depth >= 3) return "[…]";
    if (Array.isArray(v)) return v.slice(0, 8).map((x) => walk(x, "", depth + 1) ?? null);
    if (isRecord(v)) {
      const o: JsonObject = {};
      for (const [k, x] of Object.entries(v).slice(0, 16)) {
        const w = walk(x, k, depth + 1);
        if (w !== undefined) o[clip(k, 32)] = w;
      }
      return o;
    }
    return undefined;
  };
  return walk(args, "", 0) as JsonObject;
}

/** Removes transcript text from an entry (in place). True when something was removed. */
function redact(e: ActivityDraft): boolean {
  let did = false;
  const strip = (o: JsonObject | undefined, fields: string[]) => {
    if (!o) return;
    for (const f of fields) {
      if (typeof o[f] === "string" && o[f] !== "") {
        o[f] = "";
        did = true;
      }
    }
  };
  if (e.kind === "call") strip(e.args, TRANSCRIPT_ARGS);
  if (e.kind === "job") strip(e.data, TRANSCRIPT_DATA);
  if (e.kind === "device" && e.ev === "utterance") strip(e.data, ["text"]); // shake to talk: what the human said
  return did;
}

/** Device log line level: "E" / "W" / "I" / "D" / "V" (ESP_LOG, after a colour escape), "EVT", else "?". */
export function lineLevel(text: string): string {
  if (text.startsWith("EVT ")) return "EVT";
  const m = /^(?:\x1b\[[0-9;]*m)?([EWIDV]) \(/.exec(text);
  return m ? m[1] : "?";
}

/** A job entry's view of a finished job record (core/room.ts JobRecord). */
export function jobDraft(rec: { job: string; kind: string; status: string; created: number; updated: number; result?: JsonObject; error?: string; detail?: unknown; phase?: unknown }, by: Who | undefined): ActivityDraft {
  const r = rec.result ?? {};
  const t = isRecord(r.timings) ? (r.timings as JsonObject) : undefined;
  const data: JsonObject = { status: rec.status };
  if (t) data.timings = t;
  // provider per stage: say/listen keep it in timings.voice, talk/ask nest it per stage
  const voice: JsonObject = {};
  const stage = (s: unknown, k: "tts" | "stt") => {
    if (!isRecord(s)) return;
    if (typeof s.voice === "string") voice[k] = s.voice;
    if (typeof s.voice_fallback === "string") voice.fallback = s.voice_fallback;
  };
  if (rec.kind === "say") stage(t, "tts");
  else if (rec.kind === "listen") stage(t, "stt");
  else if (t) {
    stage(t.say, "tts");
    stage(t.listen, "stt");
  }
  if (Object.keys(voice).length) data.voice = voice;
  for (const k of ["reason", "answer", "index", "outcome", "speech_ms", "duration_ms"]) if (r[k] !== undefined) data[k] = r[k];
  const text = (v: unknown) => (typeof v === "string" ? clip(v, ACTIVITY_LIMITS.textMax) : undefined);
  const heard = rec.kind === "listen" ? text(r.text) : text(r.heard);
  const said = rec.kind === "say" ? text(r.text) : text(r.said);
  if (heard !== undefined) data.heard = heard;
  if (said !== undefined) data.said = said;
  if (typeof rec.phase === "string") data.phase = rec.phase;
  return {
    ts: rec.created,
    kind: "job",
    job: rec.job,
    op: rec.kind,
    ...(by ? { agent: by.agent, who: by.who } : {}),
    ok: rec.status === "done",
    ...(rec.error ? { error: rec.error } : {}),
    ...(typeof rec.detail === "string" ? { message: clip(rec.detail, 200) } : {}),
    ms: Math.max(0, rec.updated - rec.created),
    data,
  };
}

export interface ActivityDeps {
  deviceId: string;
  kv: KV;
  clock: Clock;
  log?: (msg: string, data?: Record<string, unknown>) => void;
}

export class ActivityLog {
  private readonly deviceId: string;
  private readonly kv: KV;
  private readonly clock: Clock;
  private readonly log: NonNullable<ActivityDeps["log"]>;
  /** Appends are serialized: concurrent requests must not race the head chunk. */
  private chain: Promise<void> = Promise.resolve();
  // soft intake caps (in memory: an eviction resets them, which is fine for a cap)
  private devLines = { start: 0, n: 0 };
  private devEvents = { start: 0, n: 0 };
  private refused = { start: 0, n: 0 };

  constructor(deps: ActivityDeps) {
    this.deviceId = deps.deviceId;
    this.kv = deps.kv;
    this.clock = deps.clock;
    this.log = deps.log ?? (() => {});
  }

  // ---------------- writing ----------------

  /** Appends entries (never throws: the log must not break what it observes). */
  record(drafts: ActivityDraft | ActivityDraft[]): Promise<void> {
    const list = Array.isArray(drafts) ? drafts : [drafts];
    if (!list.length) return Promise.resolve();
    const run = this.chain.then(() => this.append(list));
    this.chain = run.catch((e: unknown) => this.log("activity append failed", { device: this.deviceId, error: e instanceof Error ? e.message : String(e) }));
    return this.chain;
  }

  /** A device-scope event, under the per-minute cap. */
  device(ev: string, data: JsonObject = {}): Promise<void> {
    if (ev === "refused" ? !this.admit(this.refused, 1, ACTIVITY_LIMITS.refusedPerMinute) : !this.admit(this.devEvents, 1, ACTIVITY_LIMITS.deviceEventsPerMinute)) return Promise.resolve();
    return this.record({ ts: this.clock.now(), kind: "device", ev, data });
  }

  /** One authenticated agent call: `t0` its start, `out` its outcome (returned unchanged). */
  async call<T extends { ok: boolean; error?: string; message?: string; result?: unknown; details?: JsonObject }>(by: Who, op: string, args: unknown, t0: number, out: T): Promise<T> {
    if (QUIET_OPS.has(op) && out.ok) return out;
    const result = out.ok && isRecord(out.result) ? out.result : undefined;
    const job = typeof result?.job === "string" ? result.job : typeof out.details?.job === "string" ? out.details.job : op === "wait" && isRecord(args) && typeof args.job === "string" ? args.job : undefined;
    const data: JsonObject = {};
    if (result) {
      if (typeof result.status === "string") data.status = result.status;
      // status: what the device reported about its link (rssi) and itself
      if (op === "status") for (const k of ["rssi", "uptime", "fps", "mood", "state", "fw"]) if (typeof result[k] === "number" || typeof result[k] === "string") data[k] = result[k] as string | number;
    }
    const summary = summarizeArgs(op, args);
    await this.record({
      ts: t0,
      kind: "call",
      op,
      agent: by.agent,
      who: by.who,
      ...(job ? { job } : {}),
      ...(summary && Object.keys(summary).length ? { args: summary } : {}),
      ok: out.ok,
      ...(!out.ok && out.error ? { error: out.error } : {}),
      ...(!out.ok && out.message ? { message: clip(printable(out.message), 200) } : {}),
      ms: Math.max(0, this.clock.now() - t0),
      ...(Object.keys(data).length ? { data } : {}),
    });
    return out;
  }

  /**
   * Device -> relay `{"t":"log", boot, up, lines:[[seq, ms, tag, text]...], prev?, sync?}`.
   * Malformed frames and lines are dropped; lines already stored (by boot + seq) are skipped.
   */
  deviceLog(frame: Record<string, unknown>): Promise<void> {
    const recvAt = this.clock.now();
    const boot = frame.boot;
    if (!Number.isInteger(boot) || (boot as number) < 0 || !Array.isArray(frame.lines)) return Promise.resolve();
    const prev = frame.prev === true;
    const up = frame.up;
    if (!prev && (typeof up !== "number" || !Number.isFinite(up) || up < 0)) return Promise.resolve();
    let sync: [number, number] | undefined;
    if (Array.isArray(frame.sync) && frame.sync.length === 2 && frame.sync.every((v) => typeof v === "number" && Number.isFinite(v) && v >= 0)) {
      sync = [frame.sync[0] as number, frame.sync[1] as number];
    }
    const lines: Array<{ seq: number; ms: number; tag: string; text: string }> = [];
    for (const l of frame.lines.slice(0, ACTIVITY_LIMITS.deviceLinesPerFrame)) {
      if (!Array.isArray(l) || l.length !== 4) continue;
      const [seq, ms, tag, text] = l;
      if (!Number.isInteger(seq) || seq < 0 || !Number.isInteger(ms) || ms < 0 || typeof tag !== "string" || typeof text !== "string" || !text) continue;
      if (tag !== "" && !JOB_TAG_RE.test(tag)) continue;
      lines.push({ seq, ms, tag, text: clip(printable(text.replace(/\x1b\[[0-9;]*m/g, "")), ACTIVITY_LIMITS.lineMax) });
    }
    if (!lines.length) return Promise.resolve();
    const run = this.chain.then(async () => {
      const cur = (await this.kv.get<DevCursor>(KEY.dev)) ?? {};
      const key = String(boot);
      const after = cur[key] ?? -1;
      const fresh = lines.filter((l) => l.seq > after).sort((a, b) => a.seq - b.seq);
      if (!fresh.length) return;
      if (!this.admit(this.devLines, fresh.length, ACTIVITY_LIMITS.deviceLinesPerMinute)) return;
      const drafts: ActivityDraft[] = fresh.map((l) => {
        let ts: number;
        let approx = false;
        if (!prev) ts = recvAt - Math.max(0, (up as number) - l.ms);
        else if (sync) ts = sync[1] + (l.ms - sync[0]);
        else {
          ts = recvAt;
          approx = true;
        }
        const level = lineLevel(l.text);
        return {
          ts: Math.min(ts, recvAt),
          kind: "device_log" as const,
          ...(l.tag ? { job: `${this.deviceId}.j_${l.tag}` } : {}),
          ...(level === "E" ? { ok: false } : {}),
          data: { text: l.text, level, boot: boot as number, seq: l.seq, ...(prev ? { prev: true } : {}), ...(approx ? { approx: true } : {}) },
        };
      });
      cur[key] = fresh[fresh.length - 1].seq;
      // keep the last 4 boots' cursors
      const boots = Object.keys(cur).map(Number).sort((a, b) => b - a);
      for (const b of boots.slice(4)) delete cur[String(b)];
      await this.kv.put(KEY.dev, cur);
      await this.append(drafts);
    });
    this.chain = run.catch((e: unknown) => this.log("activity device log failed", { device: this.deviceId, error: e instanceof Error ? e.message : String(e) }));
    return this.chain;
  }

  private admit(c: { start: number; n: number }, n: number, max: number): boolean {
    const now = this.clock.now();
    if (now - c.start >= 60000) Object.assign(c, { start: now, n: 0 });
    if (c.n + n > max) return false;
    c.n += n;
    return true;
  }

  private async append(drafts: ActivityDraft[]) {
    const now = this.clock.now();
    const cfg = await this.settings();
    const h = (await this.kv.get<Head>(KEY.head)) ?? { head: 0, tail: 0 };
    let c = (await this.kv.get<Chunk>(KEY.chunk(h.head))) ?? { n: h.head, seq: 0, bytes: 0, entries: [] };
    // the head of a quiet device may hold expired entries: they go now
    const fresh = c.entries.filter((e) => now - e.ts <= ACTIVITY_LIMITS.maxAgeMs);
    if (fresh.length !== c.entries.length) c = { ...c, entries: fresh, bytes: fresh.reduce((s, e) => s + JSON.stringify(e).length, 0) };
    let rolled = false;
    for (const d of drafts) {
      const draft: ActivityDraft = structuredClone(d);
      if (!cfg.transcripts && redact(draft)) draft.redacted = true;
      const e: ActivityEntry = { id: `a${c.seq + 1}`, ...draft };
      const size = JSON.stringify(e).length;
      if (c.entries.length && (c.entries.length >= ACTIVITY_LIMITS.chunkEntries || c.bytes + size > ACTIVITY_LIMITS.chunkBytes)) {
        await this.kv.put(KEY.chunk(c.n), c);
        c = { n: c.n + 1, seq: c.seq, bytes: 0, entries: [] };
        h.head = c.n;
        rolled = true;
      }
      c.seq++;
      c.entries.push(e);
      c.bytes += size;
    }
    await this.kv.put(KEY.chunk(c.n), c);
    if (rolled) {
      // Trim: at most maxChunks chunks, and none whose newest entry expired.
      while (h.tail < h.head) {
        if (h.head - h.tail >= ACTIVITY_LIMITS.maxChunks) {
          await this.kv.delete(KEY.chunk(h.tail++));
          continue;
        }
        const old = await this.kv.get<Chunk>(KEY.chunk(h.tail));
        const newest = old?.entries.reduce((m, e) => Math.max(m, e.ts), 0) ?? 0;
        if (old && now - newest <= ACTIVITY_LIMITS.maxAgeMs) break;
        await this.kv.delete(KEY.chunk(h.tail++));
      }
      await this.kv.put(KEY.head, h);
    }
  }

  // ---------------- settings ----------------

  async settings(): Promise<Cfg> {
    return (await this.kv.get<Cfg>(KEY.cfg)) ?? { transcripts: true };
  }

  async setTranscripts(on: boolean): Promise<Cfg> {
    const cfg = { ...(await this.settings()), transcripts: on };
    await this.chain; // entries queued before the change are written under the old setting
    await this.kv.put(KEY.cfg, cfg);
    return cfg;
  }

  /** Forget: every activity key (the room deletes the `al:` prefix itself; this is for direct use). */
  async wipe(): Promise<void> {
    await this.chain;
    for (const key of (await this.kv.list(ACTIVITY_PREFIX)).keys()) await this.kv.delete(key);
  }

  // ---------------- reading ----------------

  /**
   * The merged timeline, ts ascending. `viewer`: the owner (everything; `agent` filters by
   * name) or one agent (by token hash: its own entries, its jobs' device lines, and device
   * connects / disconnects within its jobs' spans — never another agent's entries).
   */
  async query(q: ActivityQuery, viewer: { owner: true } | { who: string }): Promise<ActivityPage> {
    await this.chain; // read your own writes
    const now = this.clock.now();
    const all: ActivityEntry[] = [];
    for (const c of (await this.kv.list<Chunk>("al:c:")).values()) for (const e of c.entries) if (now - e.ts <= ACTIVITY_LIMITS.maxAgeMs) all.push(e);
    const seqOf = (e: ActivityEntry) => Number(e.id.slice(1));
    all.sort((a, b) => seqOf(a) - seqOf(b));
    const head = all.length ? seqOf(all[all.length - 1]) : 0;

    let visible = all;
    if ("who" in viewer) {
      const mine = new Set<string>();
      const spans: Array<[number, number]> = [];
      for (const e of all) {
        if (e.who !== viewer.who || !e.job) continue;
        mine.add(e.job);
        spans.push([e.ts - 5000, e.ts + (e.ms ?? 0) + 30000]);
      }
      visible = all.filter((e) => {
        if (e.who !== undefined) return e.who === viewer.who;
        if (e.agent !== undefined) return false; // attributed to an agent but no hash: never someone else's
        if (e.kind === "device_log") return !!e.job && mine.has(e.job);
        if (e.kind === "device" && (e.ev === "connect" || e.ev === "disconnect")) return spans.some(([a, b]) => e.ts >= a && e.ts <= b);
        return false;
      });
    }

    const kinds = q.kind?.length ? new Set(q.kind) : undefined;
    const isError = (e: ActivityEntry) => e.ok === false || e.error !== undefined;
    let out = visible.filter(
      (e) =>
        (!kinds || kinds.has(e.kind)) &&
        (q.job === undefined || e.job === q.job) &&
        (q.agent === undefined || e.agent === q.agent) &&
        (!q.errors || isError(e)),
    );
    const limit = Math.max(1, Math.min(q.limit ?? ACTIVITY_LIMITS.page, ACTIVITY_LIMITS.maxPage));
    let more = false;
    if (typeof q.since === "string") {
      const after = Number(q.since.slice(1));
      out = out.filter((e) => seqOf(e) > after);
      more = out.length > limit;
      out = out.slice(0, limit);
    } else {
      if (typeof q.since === "number") out = out.filter((e) => e.ts >= (q.since as number));
      more = out.length > limit;
      out = out.slice(-limit);
    }
    // next: resume after what was returned; a page cut short resumes inside the range
    const next = more && typeof q.since === "string" && out.length ? seqOf(out[out.length - 1]) : Math.max(head, typeof q.since === "string" ? Number(q.since.slice(1)) : 0);
    const shown = out
      .slice()
      // same ms: the call that started a job before the job's own entries
      .sort((a, b) => a.ts - b.ts || (a.kind === "call" ? 0 : 1) - (b.kind === "call" ? 0 : 1) || seqOf(a) - seqOf(b))
      .map(({ who: _who, ...e }) => e as unknown as JsonObject);
    return { entries: shown, next: `a${next}`, more };
  }
}

/** Parses REST / MCP query params into an ActivityQuery; a string error on bad input. */
export function parseActivityQuery(p: { since?: unknown; limit?: unknown; agent?: unknown; kind?: unknown; job?: unknown; errors?: unknown }): ActivityQuery | string {
  const q: ActivityQuery = {};
  if (p.since !== undefined && p.since !== null && p.since !== "") {
    if (typeof p.since === "string" && /^a\d{1,12}$/.test(p.since)) q.since = p.since;
    else if ((typeof p.since === "number" || (typeof p.since === "string" && /^\d{1,15}$/.test(p.since))) && Number(p.since) >= 0) q.since = Number(p.since);
    else return "since must be an entry id like a42 or a unix time in ms";
  }
  if (p.limit !== undefined && p.limit !== null && p.limit !== "") {
    const n = Number(p.limit);
    if (!Number.isInteger(n) || n < 1 || n > ACTIVITY_LIMITS.maxPage) return `limit must be 1..${ACTIVITY_LIMITS.maxPage}`;
    q.limit = n;
  }
  if (p.agent !== undefined && p.agent !== null && p.agent !== "") {
    if (typeof p.agent !== "string" || p.agent.length > 64) return "agent must be an agent name";
    q.agent = p.agent;
  }
  if (p.kind !== undefined && p.kind !== null && p.kind !== "") {
    const list = Array.isArray(p.kind) ? p.kind : typeof p.kind === "string" ? p.kind.split(",") : null;
    if (!list || !list.every((k) => typeof k === "string" && (ACTIVITY_KINDS as readonly string[]).includes(k))) return `kind must be one or more of ${ACTIVITY_KINDS.join(", ")}`;
    q.kind = list as string[];
  }
  if (p.job !== undefined && p.job !== null && p.job !== "") {
    if (typeof p.job !== "string" || !/^molty-[a-z2-9]{10}\.j_[A-Za-z0-9_-]{1,32}$/.test(p.job)) return "job must be a job id";
    q.job = p.job;
  }
  if (p.errors !== undefined && p.errors !== null && p.errors !== "") {
    if (p.errors === true || p.errors === "1" || p.errors === "true") q.errors = true;
    else if (p.errors === false || p.errors === "0" || p.errors === "false") q.errors = false;
    else return "errors must be true or false";
  }
  return q;
}
