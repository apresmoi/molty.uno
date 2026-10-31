// SPDX-License-Identifier: MIT
// Keep audio for debugging (docs/AGENT-LINK.md "Activity log" › "Keep audio"): an owner setting,
// OFF by default. While on (and transcripts are on), the raw PCM of each listen as the relay
// received it (seq order, the first AUDIO_KEEP.maxMs) is kept: the newest AUDIO_KEEP.turns
// listens, each deleted AUDIO_KEEP.maxAgeMs after capture. Downloadable by the owner only (link
// code), as WAV. Storage: `aud:m:<job>` {job, at, bytes, pieces} + `aud:p:<job>:<n>` pieces of
// <= pieceBytes (a Durable Object value stays far under its size limit).

import type { Clock, KV } from "./store.js";

export const AUDIO_KEEP = {
  turns: 5,
  maxMs: 30000,
  maxAgeMs: 24 * 60 * 60 * 1000,
  pieceBytes: 64 * 1024,
  rate: 16000,
} as const;

/** Every key of kept audio starts with this (forget wipes it). */
export const AUDIO_PREFIX = "aud:";
const META = "aud:m:";
const piece = (job: string, n: number) => `aud:p:${job}:${String(n).padStart(4, "0")}`;

interface Meta {
  job: string;
  at: number;
  bytes: number;
  pieces: number;
}

export class AudioKeep {
  /**
   * Bumped by wipe(). A store() started under an older generation (the setting was on when the
   * listen read it, then the owner turned it off) writes nothing more, and removes what it wrote.
   */
  generation = 0;

  constructor(
    private readonly kv: KV,
    private readonly clock: Clock,
  ) {}

  /**
   * Keeps one listen's PCM (16 kHz mono PCM16 bytes, seq order). False when there was nothing to
   * keep, or `gen` (the generation read before the setting was) is stale: a wipe came between.
   */
  async store(job: string, chunks: Uint8Array[], gen: number = this.generation): Promise<boolean> {
    if (gen !== this.generation) return false;
    const max = (AUDIO_KEEP.maxMs * AUDIO_KEEP.rate * 2) / 1000;
    let total = 0;
    for (const c of chunks) total += c.byteLength;
    const bytes = Math.min(total, max) & ~1;
    if (!bytes) return false;
    const all = new Uint8Array(bytes);
    let o = 0;
    for (const c of chunks) {
      if (o >= bytes) break;
      const n = Math.min(c.byteLength, bytes - o);
      all.set(c.subarray(0, n), o);
      o += n;
    }
    await this.drop(job);
    const pieces = Math.ceil(bytes / AUDIO_KEEP.pieceBytes);
    const stale = async () => {
      if (gen === this.generation) return false;
      await this.drop(job); // a wipe ran meanwhile: nothing of this listen may stay
      return true;
    };
    for (let i = 0; i < pieces; i++) {
      if (await stale()) return false;
      await this.kv.put(piece(job, i), all.slice(i * AUDIO_KEEP.pieceBytes, (i + 1) * AUDIO_KEEP.pieceBytes));
    }
    if (await stale()) return false;
    await this.kv.put<Meta>(META + job, { job, at: this.clock.now(), bytes, pieces });
    if (await stale()) return false;
    await this.purge();
    return true;
  }

  /** Deletes expired listens and all but the newest AUDIO_KEEP.turns. Returns the next expiry (unix ms), if any. */
  async purge(): Promise<number | undefined> {
    const now = this.clock.now();
    const metas = [...(await this.kv.list<Meta>(META)).values()].sort((a, b) => b.at - a.at);
    let next: number | undefined;
    for (const [i, m] of metas.entries()) {
      if (i >= AUDIO_KEEP.turns || now - m.at >= AUDIO_KEEP.maxAgeMs) await this.drop(m.job);
      else next = next === undefined ? m.at + AUDIO_KEEP.maxAgeMs : Math.min(next, m.at + AUDIO_KEEP.maxAgeMs);
    }
    return next;
  }

  /** The next expiry without deleting anything (alarm scheduling). */
  async nextExpiry(): Promise<number | undefined> {
    let next: number | undefined;
    for (const m of (await this.kv.list<Meta>(META)).values()) next = next === undefined ? m.at + AUDIO_KEEP.maxAgeMs : Math.min(next, m.at + AUDIO_KEEP.maxAgeMs);
    return next;
  }

  /** One kept listen's PCM, or undefined (never kept, or expired: expired audio is deleted here). */
  async get(job: string): Promise<Uint8Array | undefined> {
    await this.purge();
    const m = await this.kv.get<Meta>(META + job);
    if (!m) return undefined;
    const out = new Uint8Array(m.bytes);
    let o = 0;
    for (let i = 0; i < m.pieces; i++) {
      const p = await this.kv.get<Uint8Array>(piece(job, i));
      if (!p) return undefined;
      const b = p instanceof Uint8Array ? p : new Uint8Array(p as ArrayBuffer);
      out.set(b.subarray(0, Math.min(b.byteLength, m.bytes - o)), o);
      o += b.byteLength;
    }
    return out;
  }

  /** The jobs whose audio is kept, newest first. */
  async jobs(): Promise<string[]> {
    return [...(await this.kv.list<Meta>(META)).values()].sort((a, b) => b.at - a.at).map((m) => m.job);
  }

  /** Deletes everything kept. */
  async wipe(): Promise<void> {
    this.generation++;
    for (const key of (await this.kv.list(AUDIO_PREFIX)).keys()) await this.kv.delete(key);
  }

  private async drop(job: string) {
    for (const key of (await this.kv.list(`aud:p:${job}:`)).keys()) await this.kv.delete(key);
    await this.kv.delete(META + job);
  }
}

/** 16-bit mono PCM bytes as a WAV file. */
export function wav(pcm: Uint8Array, rate: number = AUDIO_KEEP.rate): Uint8Array {
  const out = new Uint8Array(44 + pcm.byteLength);
  const v = new DataView(out.buffer);
  const str = (o: number, s: string) => {
    for (let i = 0; i < s.length; i++) out[o + i] = s.charCodeAt(i);
  };
  str(0, "RIFF");
  v.setUint32(4, 36 + pcm.byteLength, true);
  str(8, "WAVE");
  str(12, "fmt ");
  v.setUint32(16, 16, true);
  v.setUint16(20, 1, true); // PCM
  v.setUint16(22, 1, true); // mono
  v.setUint32(24, rate, true);
  v.setUint32(28, rate * 2, true);
  v.setUint16(32, 2, true);
  v.setUint16(34, 16, true);
  str(36, "data");
  v.setUint32(40, pcm.byteLength, true);
  out.set(pcm, 44);
  return out;
}
