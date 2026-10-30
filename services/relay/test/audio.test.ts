import { describe, expect, it } from "vitest";
import { bytesToPcm16, encodeWav, floatToPcm16, isLikelyMp3, levels, parseWav, pcm16ToBytes, resample, toMono, toPcm16k } from "../src/core/audio.js";

function wav(opts: { rate: number; channels: number; bits: number; format?: number; frames: Float32Array[]; dataSize?: number }) {
  const { rate, channels, bits } = opts;
  const format = opts.format ?? (bits === 32 && opts.format === 3 ? 3 : 1);
  const n = opts.frames[0].length;
  const width = bits / 8;
  const data = new Uint8Array(n * channels * width);
  const v = new DataView(data.buffer);
  for (let i = 0; i < n; i++)
    for (let c = 0; c < channels; c++) {
      const x = opts.frames[c][i];
      const o = (i * channels + c) * width;
      if (format === 3) v.setFloat32(o, x, true);
      else if (bits === 16) v.setInt16(o, Math.round(x * 32767), true);
      else if (bits === 24) {
        const iv = Math.round(x * 8388607);
        data[o] = iv & 0xff;
        data[o + 1] = (iv >> 8) & 0xff;
        data[o + 2] = (iv >> 16) & 0xff;
      } else if (bits === 8) data[o] = Math.round(x * 127 + 128);
    }
  const out = new Uint8Array(44 + data.length);
  const h = new DataView(out.buffer);
  const w = (o: number, s: string) => [...s].forEach((ch, i) => (out[o + i] = ch.charCodeAt(0)));
  w(0, "RIFF");
  h.setUint32(4, 36 + data.length, true);
  w(8, "WAVE");
  w(12, "fmt ");
  h.setUint32(16, 16, true);
  h.setUint16(20, format, true);
  h.setUint16(22, channels, true);
  h.setUint32(24, rate, true);
  h.setUint32(28, rate * channels * width, true);
  h.setUint16(32, channels * width, true);
  h.setUint16(34, bits, true);
  w(36, "data");
  h.setUint32(40, opts.dataSize ?? data.length, true);
  out.set(data, 44);
  return out;
}

const sine = (n: number, rate: number, hz: number, amp = 0.5) => Float32Array.from({ length: n }, (_, i) => amp * Math.sin((2 * Math.PI * hz * i) / rate));

/** Dominant frequency by zero crossings. */
function freq(pcm: Int16Array, rate: number) {
  let z = 0;
  for (let i = 1; i < pcm.length; i++) if ((pcm[i - 1] < 0) !== (pcm[i] < 0)) z++;
  return (z / 2) * (rate / pcm.length);
}

describe("audio conversion", () => {
  it("round-trips PCM16 through WAV", () => {
    const pcm = Int16Array.from([0, 1, -1, 32767, -32768, 1234]);
    const parsed = parseWav(encodeWav(pcm));
    expect(parsed.rate).toBe(16000);
    expect(parsed.channels).toBe(1);
    expect(Array.from(floatToPcm16(parsed.samples))).toEqual(Array.from(pcm));
  });

  it("is lossless for 16 kHz mono 16-bit input", () => {
    const pcm = Int16Array.from({ length: 1000 }, (_, i) => ((i * 97) % 65536) - 32768);
    expect(Array.from(toPcm16k(encodeWav(pcm)))).toEqual(Array.from(pcm));
  });

  it("converts 24 kHz stereo 16-bit WAV to 16 kHz mono, preserving pitch and length", () => {
    const left = sine(24000, 24000, 440);
    const out = toPcm16k(wav({ rate: 24000, channels: 2, bits: 16, frames: [left, left] }));
    expect(out.length).toBe(16000);
    expect(freq(out, 16000)).toBeGreaterThan(430);
    expect(freq(out, 16000)).toBeLessThan(450);
    expect(levels(out).peak).toBeGreaterThan(0.45);
  });

  it("averages channels", () => {
    expect(Array.from(toMono(Float32Array.from([1, 0, 0.5, 0.5]), 2))).toEqual([0.5, 0.5]);
  });

  it("decodes 24-bit, 8-bit and float32 WAV", () => {
    const s = sine(4800, 48000, 1000);
    for (const [bits, format] of [[24, 1], [8, 1], [32, 3]] as const) {
      const out = toPcm16k(wav({ rate: 48000, channels: 1, bits, format, frames: [s] }));
      expect(out.length).toBe(1600);
      expect(freq(out, 16000)).toBeGreaterThan(950);
      expect(freq(out, 16000)).toBeLessThan(1050);
    }
  });

  it("reads streaming WAVs whose data size is 0xFFFFFFFF or 0", () => {
    const s = sine(1600, 16000, 500);
    expect(toPcm16k(wav({ rate: 16000, channels: 1, bits: 16, frames: [s], dataSize: 0xffffffff })).length).toBe(1600);
    expect(toPcm16k(wav({ rate: 16000, channels: 1, bits: 16, frames: [s], dataSize: 0 })).length).toBe(1600);
  });

  it("resamples raw 24 kHz PCM16 (OpenAI pcm) to 16 kHz", () => {
    const raw = pcm16ToBytes(floatToPcm16(sine(2400, 24000, 300)));
    const out = toPcm16k(raw, 24000);
    expect(out.length).toBe(1600);
    expect(freq(out, 16000)).toBeGreaterThan(290);
    expect(freq(out, 16000)).toBeLessThan(310);
  });

  it("anti-aliases on downsampling: a 7 kHz tone at 48 kHz is attenuated", () => {
    const out = resample(sine(4800, 48000, 7000, 0.8), 48000, 16000);
    const peak = Math.max(...Array.from(out, Math.abs));
    expect(peak).toBeLessThan(0.8);
  });

  it("rejects MP3 with an explanatory error", () => {
    expect(isLikelyMp3(Uint8Array.from([0x49, 0x44, 0x33, 4]))).toBe(true);
    expect(() => toPcm16k(Uint8Array.from([0xff, 0xfb, 0x90, 0x00]))).toThrow(/MP3/);
  });

  it("handles unaligned byte offsets and odd lengths", () => {
    const buf = new Uint8Array([9, 0x34, 0x12, 0xff, 0x7f, 5]);
    expect(Array.from(bytesToPcm16(buf.subarray(1)))).toEqual([0x1234, 0x7fff]);
  });

  it("computes normalized peak and rms", () => {
    expect(levels(Int16Array.from([16384, -16384]))).toEqual({ peak: 0.5, rms: 0.5 });
    expect(levels(new Int16Array(0))).toEqual({ peak: 0, rms: 0 });
  });
});
