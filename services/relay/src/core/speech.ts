// Streaming speech out: split an utterance into sentence-sized segments,
// synthesise them in order with one segment of lookahead, and expose the PCM
// as a feed the say streamer drains while synthesis is still running. The
// device hears sentence 1 while sentence 2 is being synthesised.
//
// Pure TS (no Cloudflare imports). The provider may stream a segment itself
// (VoiceProvider.ttsStream, e.g. Workers AI's ReadableStream); otherwise each
// segment is one tts() call.

import type { TtsOptions, VoiceProvider } from "../voice/types.js";
import { ACTIONS, MAX_SAY_MARKS, MOODS } from "./commands.js";

/** First segment: if the first sentence is longer than this, cut it at its first clause boundary. */
const FIRST_MAX_CHARS = 80;
/** Later segments: sentences are merged up to about this many characters (fewer TTS calls, better prosody). */
const SEGMENT_CHARS = 220;

/**
 * Splits text into speakable segments, in order, losing no characters except
 * whitespace at the cuts. Segment 1 is short (first sentence, or its first
 * clause when long) so audio starts early; later sentences are grouped.
 *
 * `streaming`: the provider streams audio as it synthesises (the voice box), so its first
 * audio does not depend on the segment's length: the first sentence is grouped with the
 * next ones like any other (fewer seams of silence and prosody restarts).
 */
export function splitSpeech(text: string, opts: { streaming?: boolean } = {}): string[] {
  // A sentence ends at . ! ? … (plus closing quotes/brackets) followed by whitespace, or at a newline;
  // "3.5" and "molty.uno" stay whole.
  const sentences = text.split(/(?<=[.!?…]["')\]]*)\s+|\s*\n+\s*/).map((s) => s.trim()).filter(Boolean);
  if (sentences.length === 0) return [text];
  const out: string[] = [];
  let cur = "";
  if (opts.streaming) {
    for (const s of sentences) {
      if (cur && cur.length + 1 + s.length > SEGMENT_CHARS) {
        out.push(cur);
        cur = s;
      } else cur = cur ? `${cur} ${s}` : s;
    }
    if (cur) out.push(cur);
    return out;
  }
  let first = sentences.shift()!;
  if (first.length > FIRST_MAX_CHARS) {
    // Cut after a clause mark (, ; : — –) that leaves at least 3 words in the head.
    const re = /[,;:—–]\s+/g;
    let m: RegExpExecArray | null;
    while ((m = re.exec(first))) {
      const head = first.slice(0, m.index + 1).trim();
      if (head.split(/\s+/).length >= 3 && head.length <= FIRST_MAX_CHARS) {
        out.push(head);
        first = first.slice(m.index + m[0].length).trim();
        break;
      }
      if (m.index > FIRST_MAX_CHARS) break;
    }
  }
  out.push(first);
  for (const s of sentences) {
    if (cur && cur.length + 1 + s.length > SEGMENT_CHARS) {
      out.push(cur);
      cur = s;
    } else cur = cur ? `${cur} ${s}` : s;
  }
  if (cur) out.push(cur);
  return out;
}

/**
 * A single-producer / single-consumer queue of PCM pieces with an end (or error) marker.
 * `signal` aborts when the feed ends, for whatever reason: the producer's cue to stop.
 */
export class PcmFeed {
  private readonly pieces: Int16Array[] = [];
  private cues: Array<[number, number]> = [];
  done = false;
  error: unknown = undefined;
  private wake: (() => void) | undefined;
  private readonly ac = new AbortController();

  get signal(): AbortSignal {
    return this.ac.signal;
  }

  push(p: Int16Array) {
    if (this.done || p.length === 0) return;
    this.pieces.push(p);
    this.poke();
  }
  end(error?: unknown) {
    if (this.done) return;
    this.done = true;
    this.error = error;
    this.poke();
    this.ac.abort(error);
  }
  /** A word cue: word `word` is spoken from sample `sample` of this feed's audio. */
  cue(word: number, sample: number) {
    if (this.done) return;
    this.cues.push([word, sample]);
  }
  /** Removes and returns the cues queued so far, in order. */
  takeCues(): Array<[number, number]> {
    return this.cues.splice(0);
  }
  /** Removes and returns everything queued. */
  take(): Int16Array[] {
    return this.pieces.splice(0);
  }
  get queued(): boolean {
    return this.pieces.length > 0;
  }
  /** Resolves when something is pushed, the feed ends, or after `ms` (via the caller's timer). */
  waitFor(setTimer: (fn: () => void, ms: number) => unknown, clearTimer: (h: unknown) => void, ms: number): Promise<void> {
    if (this.pieces.length || this.done) return Promise.resolve();
    return new Promise((resolve) => {
      const timer = setTimer(() => {
        this.wake = undefined;
        resolve();
      }, Math.max(0, ms));
      this.wake = () => {
        clearTimer(timer);
        resolve();
      };
    });
  }
  private poke() {
    const w = this.wake;
    this.wake = undefined;
    w?.();
  }
}

export interface SynthTimer {
  setTimeout(fn: () => void, ms: number): unknown;
  clearTimeout(h: unknown): void;
}

class TtsTimeout extends Error {
  readonly code = "tts_timeout";
  constructor() {
    super("tts_timeout");
  }
}

/**
 * Synthesises `segments` into `feed`, in order, with one segment of lookahead
 * (segment i+1 starts when segment i starts). Each segment must finish within
 * `segmentTimeoutMs`. Resolves when the feed has ended (ok or error).
 */
/** Words the way the device's speed reader counts them (whitespace-split, ASCII whitespace). */
export function speechWords(text: string): string[] {
  return text.split(/[ \t\r\n\v\f]+/).filter(Boolean);
}

/** Longest cue name between the brackets (`[` + 1..24 chars + `]`). */
const CUE_MAX_CHARS = 24;
/** Built on first use: commands.ts imports this module too (the cycle is safe only lazily). */
let cueNames: ReadonlySet<string> | undefined;
const isWs = (c: string | undefined) => c === " " || c === "\t" || c === "\n" || c === "\r" || c === "\v" || c === "\f";

/**
 * Inline mood / action cues in say text: `[happy] Build passed! [nod] …`. Returns the text
 * as spoken and shown (cues removed) and the marks [word, name], in text order.
 *
 * - A cue is `[` + 1..24 chars without `[`, `]`, CR, LF (nor an escaped `\[` / `\]`) + `]`. Its name
 *   is the inside trimmed, lowercased, runs of spaces / tabs / hyphens → `_` (`[Shake Head]` →
 *   shake_head). A MOODS / ACTIONS name becomes a mark; any other cue is removed silently.
 * - An unescaped `[` or `]` that forms no cue is dropped (never spoken nor shown); the text
 *   after it stays. `\[` / `\]` are literal brackets; any other backslash is literal.
 * - Where something was removed, the whitespace on both sides collapses to what was there on
 *   one side (a line break wins over spaces); the result is trimmed. Nothing else changes.
 * - A mark's word = the number of words (speechWords) of the result that start before the
 *   cue: before word k → k, mid-word → the next word, after the last word → the word count
 *   (the device fires it when playback ends).
 * - At most MAX_SAY_MARKS marks (the first ones); an exact duplicate [word, name] is dropped.
 */
export function parseCues(text: string): { text: string; marks: Array<[number, string]> } {
  cueNames ??= new Set<string>([...MOODS, ...ACTIONS]);
  let out = "";
  /** Something was just removed: whitespace that would now double up is skipped. */
  let cut = false;
  const found: Array<[number, string]> = []; // [position in out, name]
  const remove = () => {
    cut = true;
  };
  const append = (c: string) => {
    if (cut && isWs(c) && (out === "" || isWs(out[out.length - 1]))) {
      if (c !== "\n" && c !== "\r") return; // a space after the gap that already has one
      // a line break after the gap: it replaces the spaces before it
      let end = out.length;
      while (end > 0 && (out[end - 1] === " " || out[end - 1] === "\t")) end--;
      out = out.slice(0, end);
      for (const f of found) if (f[0] > end) f[0] = end;
    }
    out += c;
    if (!isWs(c)) cut = false;
  };
  for (let i = 0; i < text.length; ) {
    const c = text[i];
    if (c === "\\" && (text[i + 1] === "[" || text[i + 1] === "]")) {
      append(text[i + 1]);
      i += 2;
      continue;
    }
    if (c === "]") {
      remove(); // a lone closing bracket
      i++;
      continue;
    }
    if (c !== "[") {
      append(c);
      i++;
      continue;
    }
    // `[`: a cue if an unescaped `]` closes it within CUE_MAX_CHARS, before any `[`, `]`, CR, LF
    let close = -1;
    for (let j = i + 1; j <= i + CUE_MAX_CHARS + 1 && j < text.length; j++) {
      const d = text[j];
      if (d === "]") {
        if (j > i + 1) close = j;
        break;
      }
      if (d === "[" || d === "\n" || d === "\r") break;
      if (d === "\\" && (text[j + 1] === "[" || text[j + 1] === "]")) break;
    }
    if (close < 0) {
      remove(); // a lone opening bracket: dropped, the text after it stays
      i++;
      continue;
    }
    const name = text.slice(i + 1, close).trim().toLowerCase().replace(/[ \t-]+/g, "_");
    if (cueNames.has(name)) found.push([out.length, name]);
    remove();
    i = close + 1;
  }
  // word starts of the RESULT (speechWords' ASCII whitespace), on the text as returned: trim()
  // also drops Unicode spaces (NBSP) that the ASCII count would take for a word
  const result = out.trim();
  const lead = out.length - out.trimStart().length;
  for (const f of found) f[0] = Math.max(0, f[0] - lead);
  const starts: number[] = [];
  for (let k = 0; k < result.length; k++) if (!isWs(result[k]) && (k === 0 || isWs(result[k - 1]))) starts.push(k);
  const marks: Array<[number, string]> = [];
  const seen = new Set<string>();
  for (const [pos, name] of found) {
    let word = 0;
    while (word < starts.length && starts[word] < pos) word++;
    const key = `${word}:${name}`;
    if (seen.has(key)) continue;
    seen.add(key);
    marks.push([word, name]);
    if (marks.length === MAX_SAY_MARKS) break;
  }
  return { text: result, marks };
}

/**
 * Synthesises `segments` into `feed`, in order, with one segment of lookahead
 * (segment i+1 starts when segment i starts). Each segment must finish within
 * `segmentTimeoutMs`. Resolves when the feed has ended (ok or error).
 *
 * Word cues: every word of the utterance (counted across segments, as the device's
 * reader counts them) gets one feed.cue(word, sample) on the feed's own sample
 * timeline. A backend with timings (opts.onWord) cues them as it goes; any word left
 * uncued when its segment ends is estimated from the segment's real duration, in
 * proportion to characters.
 */
export async function synthesize(voice: VoiceProvider, segments: string[], opts: TtsOptions, feed: PcmFeed, timer: SynthTimer, segmentTimeoutMs: number): Promise<void> {
  const subs: PcmFeed[] = [];
  // The consumer ending the feed (device done / gone / failed) stops every segment now,
  // not when a stalled provider next yields.
  const stopAll = () => {
    for (const s of subs) s?.end();
  };
  feed.signal.addEventListener("abort", stopAll, { once: true });
  const start = (i: number) => {
    if (i >= segments.length || subs[i]) return;
    const sub = (subs[i] = new PcmFeed());
    void runSegment(voice, segments[i], opts, sub, timer, segmentTimeoutMs);
  };
  let pushed = 0; // samples of the feed so far: the device's playback timeline
  let wordBase = 0; // words of the earlier segments
  try {
    for (let i = 0; i < segments.length; i++) {
      start(i);
      start(i + 1);
      const sub = subs[i];
      const segStart = pushed;
      const words = speechWords(segments[i]);
      const cued = new Set<number>();
      const moveCues = () => {
        for (const [w, s] of sub.takeCues()) {
          if (w < 0 || w >= words.length || cued.has(w)) continue;
          cued.add(w);
          feed.cue(wordBase + w, segStart + s);
        }
      };
      for (;;) {
        for (const p of sub.take()) {
          feed.push(p);
          pushed += p.length;
        }
        moveCues();
        if (feed.done) return; // consumer gave up (device said done / went away / failed)
        if (sub.done) {
          if (sub.error !== undefined) throw sub.error;
          // no timing from the backend for some (or all) words: from the segment's real
          // length, in proportion to characters (a word starts after the ones before it)
          const total = pushed - segStart;
          const chars = words.reduce((n, w) => n + w.length + 1, 0);
          let before = 0;
          for (let k = 0; k < words.length; k++) {
            if (!cued.has(k)) feed.cue(wordBase + k, segStart + Math.round((total * before) / chars));
            before += words[k].length + 1;
          }
          wordBase += words.length;
          break;
        }
        await sub.waitFor((f, ms) => timer.setTimeout(f, ms), (h) => timer.clearTimeout(h), segmentTimeoutMs);
      }
    }
    feed.end();
  } catch (e) {
    feed.end(e ?? new Error("tts failed"));
  } finally {
    // Free lookahead work nobody will read.
    feed.signal.removeEventListener("abort", stopAll);
    stopAll();
  }
}

async function runSegment(voice: VoiceProvider, text: string, opts: TtsOptions, sub: PcmFeed, timer: SynthTimer, timeoutMs: number) {
  // Ending the segment (timeout, or synthesize() stopping it) aborts sub.signal, which
  // the provider uses to cancel its request / stream: a stalled backend is torn down.
  const t = timer.setTimeout(() => sub.end(new TtsTimeout()), timeoutMs);
  const o: TtsOptions = { ...opts, signal: sub.signal, onWord: (i, startS) => sub.cue(i, Math.max(0, Math.round(startS * 16000))) };
  try {
    if (voice.ttsStream) {
      for await (const p of voice.ttsStream(text, o)) {
        if (sub.done) return;
        sub.push(p);
      }
    } else {
      sub.push(await voice.tts(text, o));
    }
    sub.end();
  } catch (e) {
    sub.end(e ?? new Error("tts failed"));
  } finally {
    timer.clearTimeout(t);
  }
}

/** Rough spoken length for the device's informational `ms` before synthesis is complete (~165 wpm). */
export function estimateSpeechMs(text: string): number {
  const words = text.trim().split(/\s+/).filter(Boolean).length;
  return Math.max(300, Math.round(words * 360));
}
