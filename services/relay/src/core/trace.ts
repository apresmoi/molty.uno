// SPDX-License-Identifier: MIT
// Turn trace (docs/AGENT-LINK.md "Turn trace"): everything the relay can tell about ONE listen —
// the device's own view (its frame headers: capture time, levels, clipping, send retries, its
// end-of-listen summary `tr`), the voice box's view (`stt.trace`: speech activity, the pause
// estimate, every word with its audio time, the end-of-turn / no-speech decision) and the
// relay's (arrival times, ordering, gaps, provider, stage timings, hints, utterance). Stored
// with the job in the activity log (`data.trace` of the job entry), capped at TRACE_LIMITS.maxBytes.
// Pure and host-neutral: room.ts feeds it.

import { type AudioHeader, type JsonObject, isRecord } from "./protocol.js";

export const TRACE_LIMITS = {
  /** JSON bytes of one stored trace (trimmed to fit, `trimmed: true`). */
  maxBytes: 4096,
  /** Frames held out of order per listen before the oldest gap is given up. */
  holdFrames: 64,
  /** A seq gap left open this long (with later frames waiting, or the end frame) is given up: lost. */
  gapMs: 1000,
  /** Frames noted per listen (a 300 s listen is 1000 frames; the stored trace keeps far fewer). */
  maxFrames: 1200,
  /** A box trace bigger than this (JSON) is ignored. */
  boxBytes: 8192,
  /** Hints forwarded per listen, noted in the trace. */
  hints: 16,
  /** Partials / drafts / reacts noted per listen (core/partials.ts: at most 8 partials, 3 drafts an agent). */
  turnNotes: 16,
} as const;

/**
 * Listen frames in `seq` order. The device resends a frame that failed to go out (same seq), so
 * frames can arrive late or twice; the uplink only skips a seq when the device dropped it.
 * offer(): the frames now in order (possibly none: held), or "dup". skip(): give the oldest gap up
 * (counted in `lost`) and release what waited behind it.
 */
export class SeqOrder<T> {
  next = 0;
  dup = 0;
  readonly lost: Array<[number, number]> = [];
  private readonly held = new Map<number, { item: T; at: number }>();

  constructor(private readonly maxHeld: number = TRACE_LIMITS.holdFrames) {}

  offer(seq: number, item: T, at: number): T[] | "dup" {
    if (seq < this.next || this.held.has(seq)) {
      this.dup++;
      return "dup";
    }
    if (seq !== this.next) {
      this.held.set(seq, { item, at });
      return this.held.size > this.maxHeld ? this.skip() : [];
    }
    this.next++;
    return [item, ...this.drain()];
  }

  /** Arrival time of the oldest frame waiting behind a gap (undefined: nothing waits). */
  waitingSince(): number | undefined {
    let min: number | undefined;
    for (const h of this.held.values()) min = min === undefined ? h.at : Math.min(min, h.at);
    return min;
  }

  skip(): T[] {
    if (!this.held.size) return [];
    const first = Math.min(...this.held.keys());
    this.lost.push([this.next, first - 1]);
    this.next = first;
    return this.drain();
  }

  private drain(): T[] {
    const out: T[] = [];
    for (let h = this.held.get(this.next); h; h = this.held.get(this.next)) {
      this.held.delete(this.next++);
      out.push(h.item);
    }
    return out;
  }
}

interface FrameNote {
  seq: number;
  at: number;
  ct?: number;
  rt?: number;
  wq?: number;
  lv?: string;
  cl?: number;
  samples: number;
}

/** What the relay keeps about one listen while it runs; build() makes the stored trace. */
export class ListenTrace {
  readonly frames: FrameNote[] = [];
  /** Frames dropped as duplicates / after the listen ended. */
  dup = 0;
  late = 0;
  lost: Array<[number, number]> = [];
  tr?: Record<string, number>;
  end?: string;
  speechMs?: number;
  shake?: JsonObject;
  box?: JsonObject;
  readonly hints: Array<[string, number]> = [];
  /** While the human talks (core/partials.ts), ms from t0: partials [seq, ms, words], drafts [based_on, ms], reacts [mood|action, ms, ok|error]. */
  readonly partials: Array<[number, number, number]> = [];
  readonly drafts: Array<[number, number]> = [];
  readonly reacts: Array<[string, number, string]> = [];
  /** Relay timings and facts (ms from t0), set by room.ts. */
  readonly relay: JsonObject = {};

  constructor(
    readonly t0: number,
    readonly trigger: "shake" | "agent",
  ) {}

  /** A frame of this listen, as it arrived (before ordering; duplicates are not noted). */
  frame(h: AudioHeader, payloadBytes: number, at: number): void {
    if (this.frames.length >= TRACE_LIMITS.maxFrames) return;
    const n: FrameNote = { seq: h.seq, at, samples: payloadBytes >> 1 };
    if (h.ct !== undefined) n.ct = h.ct;
    if (h.rt !== undefined) n.rt = h.rt;
    if (h.wq !== undefined) n.wq = h.wq;
    if (h.lv !== undefined) n.lv = h.lv;
    if (h.cl !== undefined) n.cl = h.cl;
    this.frames.push(n);
    if (h.end) {
      if (h.tr) this.tr = h.tr;
      if (h.reason) this.end = h.reason;
      if (h.speech_ms !== undefined) this.speechMs = h.speech_ms;
    }
  }

  hint(k: string, at: number): void {
    if (this.hints.length < TRACE_LIMITS.hints) this.hints.push([k, at - this.t0]);
  }

  /** An utterance.partial that went out (no words: only their count). */
  partial(seq: number, words: number, at: number): void {
    if (this.partials.length < TRACE_LIMITS.turnNotes) this.partials.push([seq, at - this.t0, words]);
  }

  /** A draft held for this turn (based on partial `basedOn`). */
  draft(basedOn: number, at: number): void {
    if (this.drafts.length < TRACE_LIMITS.turnNotes) this.drafts.push([basedOn, at - this.t0]);
  }

  /** A react asked for during this turn, and how it went ("ok" or the error code). */
  react(what: string, at: number, outcome: string): void {
    if (this.reacts.length < TRACE_LIMITS.turnNotes) this.reacts.push([what, at - this.t0, outcome]);
  }

  /** The whole trace (uncapped: capTrace() it before storing). */
  build(): JsonObject {
    const dev: JsonObject = {};
    if (this.shake) dev.shake = this.shake;
    const tr = this.tr ?? {};
    for (const k of ["set", "setc", "wall", "cap", "gain", "pk", "drop"] as const) if (tr[k] !== undefined) dev[k] = tr[k];
    // levels over the whole listen, in capture order; a hole between frames (lost audio) reads 00
    const byCt = this.frames.filter((f) => f.lv !== undefined && f.ct !== undefined).sort((a, b) => a.ct! - b.ct!);
    if (byCt.length) {
      let lv = "";
      let next = byCt[0].ct!;
      for (const f of byCt) {
        const pad = Math.round((f.ct! - next) / 100);
        if (pad > 0) lv += "00".repeat(Math.min(pad, 3000));
        lv += pad >= 0 ? f.lv! : f.lv!.slice(-pad * 2); // overlapping chunks (never expected) once
        next = Math.max(next, f.ct! + (f.lv!.length / 2) * 100);
      }
      dev.lv = lv;
      dev.lv0 = byCt[0].ct!;
    }
    const clip = this.frames.filter((f) => f.cl).map((f) => [Math.floor((f.ct ?? 0) / 100), f.cl!] as [number, number]);
    if (clip.length) {
      dev.clip = clip;
      dev.clip_n = clip.reduce((s, c) => s + c[1], 0);
    }
    dev.frames = this.frames.map((f) => [f.seq, f.ct ?? null, f.at - this.t0, f.rt ?? 0, f.wq ?? null]);
    dev.sent = tr.sent ?? this.frames.length;
    const retries = this.frames.reduce((s, f) => s + (f.rt ?? 0), 0);
    dev.fail = tr.fail ?? retries;
    dev.resent = this.frames.filter((f) => f.rt).length;
    if (this.lost.length) dev.lost = this.lost;
    if (tr.dropms !== undefined) dev.lost_ms = tr.dropms;
    if (this.late) dev.late = this.late;
    if (this.dup) dev.dup = this.dup;
    if (tr.r0 !== undefined || tr.r1 !== undefined) dev.rssi = [tr.r0 ?? null, tr.r1 ?? null];
    if (this.end) dev.end = this.end;
    if (this.speechMs !== undefined) dev.speech_ms = this.speechMs;
    const relay: JsonObject = { ...this.relay };
    if (this.hints.length) relay.hints = this.hints;
    if (this.partials.length) relay.partials = this.partials;
    if (this.drafts.length) relay.drafts = this.drafts;
    if (this.reacts.length) relay.reacts = this.reacts;
    return { v: 1, trigger: this.trigger, t0: this.t0, dev, ...(this.box ? { box: this.box } : {}), relay };
  }
}

const BOX_KEYS = ["v", "step_ms", "skip", "thr", "hold_s", "audio_s", "act", "pause", "pause_every", "words", "end", "ns", "final_ms"];

/** A voice box `stt.trace` payload: known keys only, bounded; undefined when unusable. */
export function cleanBoxTrace(v: unknown): JsonObject | undefined {
  if (!isRecord(v)) return undefined;
  const out: JsonObject = {};
  for (const k of BOX_KEYS) if (v[k] !== undefined) out[k] = v[k] as JsonObject[string];
  if (!Object.keys(out).length || jsonBytes(out) > TRACE_LIMITS.boxBytes) return undefined;
  return out;
}

const utf8 = new TextEncoder();
/** JSON size in BYTES (UTF-8), not UTF-16 units: a CJK word is 3 bytes a character. */
export const jsonBytes = (t: unknown) => utf8.encode(JSON.stringify(t)).byteLength;
const size = jsonBytes;

/** Every other byte pair of a hex level string merged by max: half the resolution. */
function halveHex(hex: string, merge: (a: number, b: number) => number): string {
  let out = "";
  for (let i = 0; i < hex.length; i += 4) {
    const a = parseInt(hex.slice(i, i + 2), 16);
    const b = i + 2 < hex.length ? parseInt(hex.slice(i + 2, i + 4), 16) : a;
    out += merge(a, b).toString(16).padStart(2, "0");
  }
  return out;
}

const keepEnds = <T>(a: T[], n: number): T[] => (a.length <= 2 * n ? a : [...a.slice(0, n), ...a.slice(-n)]);

/**
 * The trace as stored: at most `max` bytes of JSON. Trimmed in this order until it fits: the
 * frame list (first/last 8, then gone), the box's pause trace (halved, then gone), the box's
 * words (last 16, then gone), the device levels (halved by max: `lv_ms` per byte), the box's
 * activity spans (first/last 8, then gone); last resort: box and device detail dropped.
 */
export function capTrace(trace: JsonObject, max: number = TRACE_LIMITS.maxBytes): JsonObject {
  if (size(trace) <= max) return trace;
  const t = structuredClone(trace);
  t.trimmed = true;
  const dev = (isRecord(t.dev) ? t.dev : {}) as JsonObject;
  const box = (isRecord(t.box) ? t.box : undefined) as JsonObject | undefined;
  const steps: Array<() => boolean> = [];
  // frames
  steps.push(() => {
    const f = dev.frames as unknown[] | undefined;
    if (!Array.isArray(f) || f.length <= 16) return false;
    dev.frames_n = f.length;
    dev.frames = keepEnds(f, 8) as JsonObject[string];
    return true;
  });
  steps.push(() => (Array.isArray(dev.frames) ? (delete dev.frames, true) : false));
  // box pause
  for (let i = 0; i < 6; i++)
    steps.push(() => {
      if (!box || typeof box.pause !== "string" || box.pause.length <= 32) return false;
      box.pause = halveHex(box.pause, (a, b) => Math.round((a + b) / 2));
      box.pause_every = (typeof box.pause_every === "number" ? box.pause_every : 1) * 2;
      return true;
    });
  steps.push(() => (box && box.pause !== undefined ? (delete box.pause, delete box.pause_every, true) : false));
  // box words
  steps.push(() => {
    if (!box || !Array.isArray(box.words) || box.words.length <= 16) return false;
    box.words_n = box.words.length;
    box.words = box.words.slice(-16);
    return true;
  });
  steps.push(() => (box && box.words !== undefined ? (delete box.words, true) : false));
  // device levels
  for (let i = 0; i < 8; i++)
    steps.push(() => {
      if (typeof dev.lv !== "string" || dev.lv.length <= 64) return false;
      dev.lv = halveHex(dev.lv, Math.max);
      dev.lv_ms = (typeof dev.lv_ms === "number" ? dev.lv_ms : 100) * 2;
      return true;
    });
  // box activity
  steps.push(() => {
    if (!box || !Array.isArray(box.act) || box.act.length <= 16) return false;
    box.act_n = box.act.length;
    box.act = keepEnds(box.act, 8) as JsonObject[string];
    return true;
  });
  steps.push(() => (box && box.act !== undefined ? (delete box.act, true) : false));
  steps.push(() => (dev.clip !== undefined ? (delete dev.clip, true) : false));
  steps.push(() => (t.box !== undefined ? (delete t.box, true) : false));
  steps.push(() => (dev.lv !== undefined ? (delete dev.lv, delete dev.lv_ms, delete dev.lv0, true) : false));
  for (const step of steps) {
    if (size(t) <= max) break;
    step();
  }
  if (size(t) > max) return { v: t.v ?? 1, trigger: t.trigger ?? null, t0: t.t0 ?? null, trimmed: true };
  return t;
}
