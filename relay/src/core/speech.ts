// Streaming speech out: split an utterance into sentence-sized segments,
// synthesise them in order with one segment of lookahead, and expose the PCM
// as a feed the say streamer drains while synthesis is still running. The
// device hears sentence 1 while sentence 2 is being synthesised.
//
// Pure TS (no Cloudflare imports). The provider may stream a segment itself
// (VoiceProvider.ttsStream, e.g. Workers AI's ReadableStream); otherwise each
// segment is one tts() call.

import type { TtsOptions, VoiceProvider } from "../voice/types.js";

/** First segment: if the first sentence is longer than this, cut it at its first clause boundary. */
const FIRST_MAX_CHARS = 80;
/** Later segments: sentences are merged up to about this many characters (fewer TTS calls, better prosody). */
const SEGMENT_CHARS = 220;

/**
 * Splits text into speakable segments, in order, losing no characters except
 * whitespace at the cuts. Segment 1 is short (first sentence, or its first
 * clause when long) so audio starts early; later sentences are grouped.
 */
export function splitSpeech(text: string): string[] {
  // A sentence ends at . ! ? … (plus closing quotes/brackets) followed by whitespace, or at a newline;
  // "3.5" and "molty.uno" stay whole.
  const sentences = text.split(/(?<=[.!?…]["')\]]*)\s+|\s*\n+\s*/).map((s) => s.trim()).filter(Boolean);
  if (sentences.length === 0) return [text];
  const out: string[] = [];
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
  let cur = "";
  for (const s of sentences) {
    if (cur && cur.length + 1 + s.length > SEGMENT_CHARS) {
      out.push(cur);
      cur = s;
    } else cur = cur ? `${cur} ${s}` : s;
  }
  if (cur) out.push(cur);
  return out;
}

/** A single-producer / single-consumer queue of PCM pieces with an end (or error) marker. */
export class PcmFeed {
  private readonly pieces: Int16Array[] = [];
  done = false;
  error: unknown = undefined;
  private wake: (() => void) | undefined;

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
export async function synthesize(voice: VoiceProvider, segments: string[], opts: TtsOptions, feed: PcmFeed, timer: SynthTimer, segmentTimeoutMs: number): Promise<void> {
  const subs: PcmFeed[] = [];
  const start = (i: number) => {
    if (i >= segments.length || subs[i]) return;
    const sub = (subs[i] = new PcmFeed());
    void runSegment(voice, segments[i], opts, sub, timer, segmentTimeoutMs);
  };
  try {
    for (let i = 0; i < segments.length; i++) {
      start(i);
      start(i + 1);
      const sub = subs[i];
      for (;;) {
        for (const p of sub.take()) feed.push(p);
        if (feed.done) return; // consumer gave up (device said done / went away / failed)
        if (sub.done) {
          if (sub.error !== undefined) throw sub.error;
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
    for (const s of subs) s?.end();
  }
}

async function runSegment(voice: VoiceProvider, text: string, opts: TtsOptions, sub: PcmFeed, timer: SynthTimer, timeoutMs: number) {
  const t = timer.setTimeout(() => sub.end(new TtsTimeout()), timeoutMs);
  try {
    if (voice.ttsStream) {
      for await (const p of voice.ttsStream(text, opts)) {
        if (sub.done) return;
        sub.push(p);
      }
    } else {
      sub.push(await voice.tts(text, opts));
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
