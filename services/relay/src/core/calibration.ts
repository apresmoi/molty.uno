// SPDX-License-Identifier: MIT
// Calibration from usage (docs/AGENT-LINK.md "Calibration"): the relay keeps one small record of
// how THIS Molty's human talks (KV `speech`) and derives from it (a) when partials go out and
// (b) how many late words a draft may survive. Host-neutral and pure: room.ts stores the record.
//
//   heard turn ──▶ addTurn {words, ms, pauses, max_pause_ms}  (last KEEP_TURNS)
//   draft end  ──▶ addOutcome used | new_words | meaning | revised | misfire …  (last KEEP_OUTCOMES)
//                     └─ every TUNE_EVERY judged outcomes: tuneThreshold (± 1, within bounds)
//   policyFor(stats) ──▶ PartialPolicy: points at ~1/3 and ~2/3 of the median turn

import { DEFAULT_PARTIAL_POLICY, type PartialPolicy } from "./partials.js";

export const CALIBRATION = {
  /** Fewer heard turns than this: the default policy (sane defaults until the data says otherwise). */
  minTurns: 8,
  keepTurns: 50,
  keepOutcomes: 30,
  /** Partial points: every_words = median words / 3 within these bounds, every_ms likewise. */
  everyWords: [4, 20] as const,
  everyMs: [2000, 8000] as const,
  /** Draft acceptance: at most this many words added after the draft's partial. */
  threshold: { initial: 4, min: 1, max: 10 },
  /** Re-tune after this many judged outcomes (used / misfire / new_words / meaning). */
  tuneEvery: 6,
  /** Lower the threshold when more than this share of used openers were misfires... */
  misfireHigh: 0.25,
  /** ...raise it when more than this share of judged drafts died of new_words and misfires stay at most misfireLow. */
  newWordsHigh: 0.4,
  misfireLow: 0.1,
  /** A shake while an opener (or its follow-up) plays, or this soon after it: the opener was likely wrong. */
  misfireMs: 2000,
} as const;

export interface TurnSample {
  at: number;
  words: number;
  ms: number;
  pauses: number;
  max_pause_ms: number;
}

/** Outcomes the threshold is tuned on; the rest are only counted. */
const JUDGED = ["used", "misfire", "new_words", "meaning"] as const;
export type DraftOutcome = (typeof JUDGED)[number] | "revised" | "turn_claimed" | "replaced" | "no_speech" | "turn_failed" | "synth_failed";

export interface SpeechStats {
  turns: TurnSample[];
  outcomes: Array<{ o: DraftOutcome; at: number }>;
  totals: { turns: number; partials: number; drafts: number; used: number; misfires: number; reacts: number; dropped: Record<string, number> };
  threshold: number;
  /** Judged outcomes since the last tune. */
  since_tune: number;
  updated: number;
}

export function emptyStats(now = 0): SpeechStats {
  return {
    turns: [],
    outcomes: [],
    totals: { turns: 0, partials: 0, drafts: 0, used: 0, misfires: 0, reacts: 0, dropped: {} },
    threshold: CALIBRATION.threshold.initial,
    since_tune: 0,
    updated: now,
  };
}

const clamp = (v: number, lo: number, hi: number) => Math.min(hi, Math.max(lo, v));

export function median(xs: number[]): number | undefined {
  if (!xs.length) return undefined;
  const s = [...xs].sort((a, b) => a - b);
  const m = s.length >> 1;
  return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2;
}

function quantile(xs: number[], p: number): number | undefined {
  if (!xs.length) return undefined;
  const s = [...xs].sort((a, b) => a - b);
  const k = (s.length - 1) * p;
  const lo = Math.floor(k);
  const hi = Math.ceil(k);
  return s[lo] + (s[hi] - s[lo]) * (k - lo);
}

/** A heard turn (immutably: returns the new record). Turns with no words are not samples. */
export function addTurn(s: SpeechStats, t: TurnSample): SpeechStats {
  if (t.words <= 0 || t.ms <= 0) return s;
  const turns = [...s.turns, t].slice(-CALIBRATION.keepTurns);
  return { ...s, turns, totals: { ...s.totals, turns: s.totals.turns + 1 }, updated: t.at };
}

/** A draft's end; may re-tune the threshold. */
export function addOutcome(s: SpeechStats, o: DraftOutcome, at: number): SpeechStats {
  const outcomes = [...s.outcomes, { o, at }].slice(-CALIBRATION.keepOutcomes);
  const totals = { ...s.totals, dropped: { ...s.totals.dropped } };
  if (o === "used") totals.used++;
  else if (o === "misfire") totals.misfires++;
  else totals.dropped[o] = (totals.dropped[o] ?? 0) + 1;
  let next: SpeechStats = { ...s, outcomes, totals, updated: at };
  if ((JUDGED as readonly string[]).includes(o)) {
    next.since_tune = s.since_tune + 1;
    if (next.since_tune >= CALIBRATION.tuneEvery) next = { ...next, threshold: tuneThreshold(next), since_tune: 0 };
  }
  return next;
}

/** A used opener turned out wrong (the human shook Molty over it): its `used` becomes `misfire`. */
export function markMisfire(s: SpeechStats, usedAt: number, at: number): SpeechStats {
  let i = s.outcomes.length - 1;
  while (i >= 0 && !(s.outcomes[i].o === "used" && s.outcomes[i].at === usedAt)) i--;
  if (i < 0) return s;
  const outcomes = s.outcomes.map((x, k) => (k === i ? { o: "misfire" as const, at: x.at } : x));
  return { ...s, outcomes, totals: { ...s.totals, used: Math.max(0, s.totals.used - 1), misfires: s.totals.misfires + 1 }, updated: at };
}

/**
 * The threshold from the recent judged outcomes, one step at a time within bounds:
 * too many misfires → stricter (−1); many drafts lost to late words while openers land → looser (+1).
 */
export function tuneThreshold(s: SpeechStats): number {
  const recent = s.outcomes.filter((x) => (JUDGED as readonly string[]).includes(x.o)).slice(-20);
  const used = recent.filter((x) => x.o === "used").length;
  const misfire = recent.filter((x) => x.o === "misfire").length;
  const late = recent.filter((x) => x.o === "new_words").length;
  const spoken = used + misfire;
  const t = CALIBRATION.threshold;
  if (spoken >= 3 && misfire / spoken > CALIBRATION.misfireHigh) return clamp(s.threshold - 1, t.min, t.max);
  if (recent.length >= 3 && late / recent.length > CALIBRATION.newWordsHigh && (spoken === 0 || misfire / spoken <= CALIBRATION.misfireLow)) return clamp(s.threshold + 1, t.min, t.max);
  return clamp(s.threshold, t.min, t.max);
}

export type CalibratedPolicy = PartialPolicy & { source: "defaults" | "calibrated" };

/**
 * When partials go out for the next turn. With enough turns: about every third of the human's
 * median turn (words and time), so a typical turn gets partials at ~1/3 and ~2/3 and then its
 * final utterance. lead_ms stays 3 s: a turn shorter than that gets no partials at all.
 */
export function policyFor(s: SpeechStats | undefined): CalibratedPolicy {
  if (!s || s.turns.length < CALIBRATION.minTurns) return { ...DEFAULT_PARTIAL_POLICY, source: "defaults" };
  const w = median(s.turns.map((t) => t.words))!;
  const ms = median(s.turns.map((t) => t.ms))!;
  return {
    ...DEFAULT_PARTIAL_POLICY,
    every_words: clamp(Math.round(w / 3), CALIBRATION.everyWords[0], CALIBRATION.everyWords[1]),
    every_ms: clamp(Math.round(ms / 3 / 100) * 100, CALIBRATION.everyMs[0], CALIBRATION.everyMs[1]),
    source: "calibrated",
  };
}

const round = (v: number | undefined, d = 0) => (v === undefined ? null : Math.round(v * 10 ** d) / 10 ** d);

/** What the owner sees (owner API, console card): habits, the policy in force and the draft record. */
export function speechSummary(s: SpeechStats | undefined) {
  const st = s ?? emptyStats();
  const ws = st.turns.map((t) => t.words);
  const ms = st.turns.map((t) => t.ms);
  const wpm = st.turns.map((t) => (t.words * 60000) / t.ms);
  const judged = st.outcomes.filter((x) => (JUDGED as readonly string[]).includes(x.o));
  const spoken = judged.filter((x) => x.o === "used" || x.o === "misfire").length;
  return {
    turns: { sampled: st.turns.length, total: st.totals.turns, needed: CALIBRATION.minTurns },
    words: { median: round(median(ws)), p25: round(quantile(ws, 0.25)), p75: round(quantile(ws, 0.75)) },
    seconds: { median: round(median(ms) !== undefined ? median(ms)! / 1000 : undefined, 1), p25: round(quantile(ms, 0.25) !== undefined ? quantile(ms, 0.25)! / 1000 : undefined, 1), p75: round(quantile(ms, 0.75) !== undefined ? quantile(ms, 0.75)! / 1000 : undefined, 1) },
    wpm: round(median(wpm)),
    pauses: { per_turn: round(median(st.turns.map((t) => t.pauses)), 1), max_ms: round(median(st.turns.filter((t) => t.pauses > 0).map((t) => t.max_pause_ms))) },
    policy: policyFor(st),
    drafts: {
      made: st.totals.drafts,
      used: st.totals.used,
      misfires: st.totals.misfires,
      dropped: st.totals.dropped,
      use_rate: judged.length ? round(spoken / judged.length, 2) : null,
      misfire_rate: spoken ? round(judged.filter((x) => x.o === "misfire").length / spoken, 2) : null,
      threshold: st.threshold,
      threshold_bounds: [CALIBRATION.threshold.min, CALIBRATION.threshold.max],
    },
    partials: st.totals.partials,
    reacts: st.totals.reacts,
    updated: st.updated || null,
  };
}
