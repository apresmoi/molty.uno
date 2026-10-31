// SPDX-License-Identifier: MIT
// Partial transcripts, drafts and reactions while the human is still talking (docs/AGENT-LINK.md
// "While the human speaks"). Host-neutral and pure: room.ts owns the open turn and calls these.
//
//   box stt.partial (every word) ─▶ PartialChunker ─▶ `utterance.partial` {turn_id, seq, text_so_far, new_text, elapsed_ms}
//                                  (by meaning: a sentence end, N new words or T seconds — whichever first —
//                                   never before the turn ran PartialPolicy.lead_ms, never token by token)
//   agent `draft` {turn_id, based_on, opener, max_new_words} ─▶ held + pre-synthesised (room.ts)
//   turn end: judgeDraft(based_on's text_so_far, final transcript) ─▶ play the opener at once, or `draft.dropped`

/** Relay-generated: words while the human is still talking. Context only: agents must not answer it. */
export const PARTIAL_EVENT = "utterance.partial";
/** Relay-generated, to the drafting agent only: its opener is being spoken and it owns the turn. */
export const DRAFT_USED_EVENT = "draft.used";
/** Relay-generated, to the drafting agent only: its draft will not be spoken (`reason`). */
export const DRAFT_DROPPED_EVENT = "draft.dropped";

/** Why a draft was not spoken. */
export const DROP_REASONS = ["new_words", "meaning", "revised", "turn_claimed", "replaced", "no_speech", "turn_failed", "synth_failed"] as const;
export type DropReason = (typeof DROP_REASONS)[number];

/** When partials go out during one turn (calibration.ts derives it from the human's habits). */
export interface PartialPolicy {
  /** No partial before the turn ran this long: a turn shorter than this gets none. */
  lead_ms: number;
  /** A partial once this many new words arrived... */
  every_words: number;
  /** ...or this long after the last one (with new words), whichever first; a sentence end always qualifies. */
  every_ms: number;
  /** At most this many partials per turn (a ramble stays bounded). */
  max_per_turn: number;
}

export const DEFAULT_PARTIAL_POLICY: PartialPolicy = { lead_ms: 3000, every_words: 8, every_ms: 4000, max_per_turn: 8 };

export interface PartialData {
  seq: number;
  text_so_far: string;
  new_text: string;
  elapsed_ms: number;
}

export { normWord, sentenceEnd, words } from "./words.js";
import { normWord, sentenceEnd, words } from "./words.js";

const norm = (ws: string[]) => ws.map(normWord).filter(Boolean);

function commonPrefix(a: string[], b: string[]): number {
  let i = 0;
  while (i < a.length && i < b.length && a[i] === b[i]) i++;
  return i;
}

/**
 * One turn's partials. `update` takes the box's whole text so far (Kyutai: every word) and
 * returns a partial when one is due; `check` re-evaluates on a timer (the human paused with
 * words not yet sent); `dueAt` says when a timer check could fire.
 */
export class PartialChunker {
  private latest = "";
  private sentWords: string[] = [];
  private lastAt: number;
  private readonly texts = new Map<number, string>();
  seq = 0;

  constructor(
    readonly policy: PartialPolicy,
    readonly startAt: number,
  ) {
    this.lastAt = startAt;
  }

  update(textSoFar: string, now: number): PartialData | undefined {
    this.latest = textSoFar.trim();
    return this.check(now);
  }

  /** New words not yet in a partial (against the last partial's words; an STT revision counts from where they differ). */
  private fresh(): { all: string[]; from: number } {
    const all = words(this.latest);
    const from = commonPrefix(norm(this.sentWords), norm(all));
    return { all, from: Math.min(from, all.length) };
  }

  check(now: number): PartialData | undefined {
    if (this.seq >= this.policy.max_per_turn) return undefined;
    const { all, from } = this.fresh();
    const added = all.length - from;
    if (added <= 0 || now - this.startAt < this.policy.lead_ms) return undefined;
    if (!(sentenceEnd(this.latest) || added >= this.policy.every_words || now - this.lastAt >= this.policy.every_ms)) return undefined;
    this.seq++;
    const text_so_far = all.join(" ");
    this.texts.set(this.seq, text_so_far);
    this.sentWords = all;
    this.lastAt = now;
    return { seq: this.seq, text_so_far, new_text: all.slice(from).join(" "), elapsed_ms: now - this.startAt };
  }

  /** When a timer should re-check (words are waiting), or undefined. */
  dueAt(): number | undefined {
    if (this.seq >= this.policy.max_per_turn) return undefined;
    const { all, from } = this.fresh();
    if (all.length - from <= 0) return undefined;
    return Math.max(this.startAt + this.policy.lead_ms, this.lastAt + this.policy.every_ms);
  }

  /** The text_so_far of partial `seq` (what a draft is based on). */
  textOf(seq: number): string | undefined {
    return this.texts.get(seq);
  }
}

/**
 * Words that can turn what came before around ("…but not today", "…actually, cancel it").
 * Any of them among the late words drops a draft: its opener was written for the words before.
 */
export const FLIP_WORDS: ReadonlySet<string> = new Set([
  "but", "not", "no", "nope", "never", "don't", "dont", "doesn't", "didn't", "isn't", "won't", "can't", "cannot",
  "shouldn't", "wouldn't", "except", "unless", "actually", "instead", "wait", "cancel", "although", "however", "rather",
]);

export type DraftVerdict = { ok: true; new_words: number } | { ok: false; reason: "new_words" | "meaning" | "revised"; new_words: number };

/**
 * May a draft written on `basedOn` (a partial's text_so_far) open the answer to `finalText`?
 * - the final transcript must still begin with those words (one revised last word is tolerated):
 *   otherwise `revised` (the STT changed what the draft was about);
 * - none of the added words may be a FLIP_WORD: otherwise `meaning`;
 * - it may add at most `maxNew` words: otherwise `new_words`.
 */
export function judgeDraft(basedOn: string, finalText: string, maxNew: number): DraftVerdict {
  const b = norm(words(basedOn));
  const f = norm(words(finalText));
  const common = commonPrefix(b, f);
  const newWords = f.length - common;
  if (common < b.length - 1 || (b.length > 0 && common === 0)) return { ok: false, reason: "revised", new_words: Math.max(0, newWords) };
  if (f.slice(common).some((w) => FLIP_WORDS.has(w))) return { ok: false, reason: "meaning", new_words: newWords };
  if (newWords > maxNew) return { ok: false, reason: "new_words", new_words: newWords };
  return { ok: true, new_words: newWords };
}

/** Pause habits from the box's word arrivals: a gap of at least this long between words is a pause. */
export const PAUSE_MS = 600;

/** Word-arrival bookkeeping of one listen (any listen the box streams: the calibration's pauses). */
export class PauseTracker {
  private n = 0;
  private lastAt: number | undefined;
  pauses = 0;
  max_pause_ms = 0;

  update(textSoFar: string, now: number) {
    const n = words(textSoFar).length;
    if (n <= this.n) return;
    if (this.lastAt !== undefined) {
      const gap = now - this.lastAt;
      if (gap >= PAUSE_MS) {
        this.pauses++;
        this.max_pause_ms = Math.max(this.max_pause_ms, gap);
      }
    }
    this.n = n;
    this.lastAt = now;
  }
}
