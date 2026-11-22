// Was anything said? The relay's own check on a listen's audio, before the turn is committed:
// a press that recorded only the room (a bump, a breath, the button's click) never reaches STT,
// an opener, a filler or an agent. It ends as no_speech.
//
//   loud      a 40 ms window (20 ms hop, at 8 kHz) at least VOICE.aboveFloorDb over the clip's
//             noise floor (its quietest VOICE.floorPct of windows, never above VOICE.floorMaxDbfs:
//             a clip that is all speech has no floor of its own) and over VOICE.absMinDbfs
//   voiced    loud and periodic: a pitch between 70 and 400 Hz, the normalized autocorrelation
//             peak (after its first dip, so a rumble's slow drift is no pitch) ≥ VOICE.periodicity.
//             Breath, hiss, clicks and thumps are loud but not periodic.
//   voice     VOICE.minRunMs of consecutive voiced windows: one vowel of one short word. A bump
//             that rings for a moment stays under it.
//
// Whispered speech has no pitch: it counts as nothing said. The transcript is the second gate
// (junkTranscript): an STT that returns no words at all on a voiced clip is no speech either.

import { SAMPLE_RATE } from "./protocol.js";

export const VOICE = {
  absMinDbfs: -55,
  aboveFloorDb: 9,
  floorMaxDbfs: -50,
  floorPct: 0.2,
  minPitchHz: 70,
  maxPitchHz: 400,
  periodicity: 0.5,
  minRunMs: 100,
} as const;

const RATE = 8000;
const HOP = RATE / 50; // 20 ms
const WIN = 2 * HOP; // 40 ms: two periods of the lowest pitch

/** 16 kHz PCM16 → 8 kHz floats (pairs averaged: enough for pitch, half the work). */
function decimate(pcm: Int16Array, rate: number): Float32Array {
  const step = Math.max(1, Math.round(rate / RATE));
  const out = new Float32Array(Math.floor(pcm.length / step));
  for (let i = 0; i < out.length; i++) {
    let s = 0;
    for (let k = 0; k < step; k++) s += pcm[i * step + k];
    out[i] = s / step / 32768;
  }
  return out;
}

function windowDb(x: Float32Array, at: number): number {
  let e = 0;
  for (let i = at; i < at + WIN; i++) e += x[i] * x[i];
  e /= WIN;
  return e > 1e-12 ? 10 * Math.log10(e) : -120;
}

/** The window's pitch strength: its normalized autocorrelation peak in the pitch range, after the first dip. */
function periodicity(x: Float32Array, at: number): number {
  let mean = 0;
  for (let i = at; i < at + WIN; i++) mean += x[i];
  mean /= WIN;
  const w = new Float32Array(WIN);
  for (let i = 0; i < WIN; i++) w[i] = x[at + i] - mean;
  const lo = Math.floor(RATE / VOICE.maxPitchHz);
  const hi = Math.ceil(RATE / VOICE.minPitchHz);
  const r = new Float32Array(hi + 2);
  for (let lag = 1; lag <= hi + 1; lag++) {
    let xy = 0;
    let xx = 0;
    let yy = 0;
    for (let i = 0; i + lag < WIN; i++) {
      xy += w[i] * w[i + lag];
      xx += w[i] * w[i];
      yy += w[i + lag] * w[i + lag];
    }
    r[lag] = xx > 0 && yy > 0 ? xy / Math.sqrt(xx * yy) : 0;
  }
  // a peak (a period), after the first dip: a pitch below the range only climbs toward its edge
  let best = 0;
  let dipped = false;
  for (let lag = 1; lag <= hi; lag++) {
    if (!dipped) dipped = r[lag] < 0;
    else if (lag >= lo && r[lag] > best && r[lag] > r[lag - 1] && r[lag] >= r[lag + 1]) best = r[lag];
  }
  return best;
}

/** True when the clip holds a human voice (see the top of this file). Stops at the first word's worth. */
export function hasVoice(pcm: Int16Array, rate = SAMPLE_RATE): boolean {
  const x = decimate(pcm, rate);
  const n = x.length < WIN ? 0 : Math.floor((x.length - WIN) / HOP) + 1;
  if (!n) return false;
  const db = new Float32Array(n);
  for (let f = 0; f < n; f++) db[f] = windowDb(x, f * HOP);
  const sorted = Float32Array.from(db).sort();
  const floor = Math.min(sorted[Math.floor((n - 1) * VOICE.floorPct)], VOICE.floorMaxDbfs);
  const loud = Math.max(VOICE.absMinDbfs, floor + VOICE.aboveFloorDb);
  const need = Math.ceil(VOICE.minRunMs / 20);
  let run = 0;
  for (let f = 0; f < n; f++) {
    if (db[f] > loud && periodicity(x, f * HOP) >= VOICE.periodicity) {
      if (++run >= need) return true;
    } else run = 0;
  }
  return false;
}

/**
 * An STT result that holds no words: empty, punctuation, or only bracketed / starred annotations
 * ("[BLANK_AUDIO]", "(silence)", "*music*", "♪"). Real words, even one, are never junk.
 */
export function junkTranscript(text: string): boolean {
  const bare = text.replace(/\[[^\]]*\]|\([^)]*\)|\*[^*]*\*/g, " ");
  return !/[\p{L}\p{N}]/u.test(bare);
}
