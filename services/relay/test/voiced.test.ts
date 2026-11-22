import { describe, expect, it } from "vitest";
import { hasVoice, junkTranscript } from "../src/core/voiced.js";

const RATE = 16000;
const ms = (n: number) => Math.round((n * RATE) / 1000);
const dbAmp = (db: number) => 32768 * 10 ** (db / 20);

/** Seeded noise: the same clip every run. */
function rng(seed = 7) {
  let s = seed;
  return () => ((s = (s * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff) * 2 - 1;
}

/** A room: low noise for `total` ms, with `add` mixed in from `at` ms. */
function clip(total: number, add?: { at: number; pcm: Float32Array }, roomDb = -62): Int16Array {
  const r = rng();
  const out = new Int16Array(ms(total));
  const a = dbAmp(roomDb) * Math.sqrt(3);
  const off = add ? ms(add.at) : 0;
  for (let i = 0; i < out.length; i++) {
    const v = r() * a + (add && i >= off && i - off < add.pcm.length ? add.pcm[i - off] : 0);
    out[i] = Math.max(-32768, Math.min(32767, Math.round(v)));
  }
  return out;
}

/** A voiced sound: a glottal-like harmonic stack at `f0` (gliding a little), with an attack and release. */
function vowel(len: number, f0 = 140, db = -28): Float32Array {
  const n = ms(len);
  const out = new Float32Array(n);
  const a = dbAmp(db);
  let ph = 0;
  for (let i = 0; i < n; i++) {
    const f = f0 * (1 + 0.08 * Math.sin((2 * Math.PI * i) / n));
    ph += (2 * Math.PI * f) / RATE;
    let s = 0;
    for (let h = 1; h <= 12; h++) s += Math.sin(h * ph) / h;
    const env = Math.min(1, i / ms(20), (n - i) / ms(20));
    out[i] = a * 0.6 * s * env;
  }
  return out;
}

function noise(len: number, db: number, decayMs?: number): Float32Array {
  const r = rng(99);
  const out = new Float32Array(ms(len));
  const a = dbAmp(db) * Math.sqrt(3);
  for (let i = 0; i < out.length; i++) out[i] = r() * a * (decayMs ? Math.exp(-i / ms(decayMs)) : Math.min(1, i / ms(30), (out.length - i) / ms(30)));
  return out;
}

function tone(len: number, hz: number, db: number, decayMs: number): Float32Array {
  const out = new Float32Array(ms(len));
  for (let i = 0; i < out.length; i++) out[i] = dbAmp(db) * Math.sin((2 * Math.PI * hz * i) / RATE) * Math.exp(-i / ms(decayMs));
  return out;
}

describe("hasVoice", () => {
  it("hears a short word and a sentence, soft or loud, low or high", () => {
    expect(hasVoice(clip(2000, { at: 600, pcm: vowel(160) }))).toBe(true); // "yes"
    expect(hasVoice(clip(4000, { at: 300, pcm: vowel(1500, 110, -20) }))).toBe(true);
    expect(hasVoice(clip(3000, { at: 800, pcm: vowel(400, 230, -40) }))).toBe(true); // softer, higher
    expect(hasVoice(clip(800, { at: 0, pcm: vowel(800, 120, -25) }))).toBe(true); // all speech: no floor of its own
  });

  it("hears nothing in silence, the room, a breath, a click, a thump or a bump's ring", () => {
    expect(hasVoice(new Int16Array(ms(3000)))).toBe(false); // digital silence
    expect(hasVoice(clip(5000))).toBe(false); // the room
    expect(hasVoice(clip(5000, undefined, -40))).toBe(false); // a loud fan
    expect(hasVoice(clip(3000, { at: 1000, pcm: noise(600, -30) }))).toBe(false); // a breath / a rub
    expect(hasVoice(clip(3000, { at: 1000, pcm: noise(30, -10, 5) }))).toBe(false); // the button's click
    expect(hasVoice(clip(3000, { at: 1000, pcm: tone(400, 35, -15, 80) }))).toBe(false); // a thump: a slow rumble
    expect(hasVoice(clip(3000, { at: 1000, pcm: tone(400, 60, -15, 80) }))).toBe(false); // ...just under the pitch range
    expect(hasVoice(clip(3000, { at: 1000, pcm: tone(200, 250, -20, 15) }))).toBe(false); // a bump that rings a moment
    expect(hasVoice(new Int16Array(3200).fill(8000))).toBe(false); // DC
    expect(hasVoice(new Int16Array(100))).toBe(false); // too short to say anything
  });
});

describe("junkTranscript", () => {
  it("no words at all is junk", () => {
    for (const t of ["", "  ", ".", "...", "[BLANK_AUDIO]", "(silence)", "*music*", "♪ ♪", "[Music] (applause)"]) expect(junkTranscript(t)).toBe(true);
  });
  it("any word is not", () => {
    for (const t of ["you", "Thank you.", "no", "42", "¿qué?", "[laughs] okay"]) expect(junkTranscript(t)).toBe(false);
  });
});
