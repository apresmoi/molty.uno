// PCM utilities: everything the voice providers need to turn whatever a TTS
// backend returns into PCM16 mono @16 kHz, and to hand STT a WAV.

import { SAMPLE_RATE } from "./protocol.js";

export interface DecodedWav {
  rate: number;
  channels: number;
  /** Interleaved samples normalized to [-1, 1]. */
  samples: Float32Array;
}

const ascii = (b: Uint8Array, o: number) => String.fromCharCode(b[o], b[o + 1], b[o + 2], b[o + 3]);

export function isWav(bytes: Uint8Array): boolean {
  return bytes.byteLength >= 12 && ascii(bytes, 0) === "RIFF" && ascii(bytes, 8) === "WAVE";
}

export function isLikelyMp3(bytes: Uint8Array): boolean {
  if (bytes.byteLength < 3) return false;
  if (bytes[0] === 0x49 && bytes[1] === 0x44 && bytes[2] === 0x33) return true; // "ID3"
  return bytes[0] === 0xff && (bytes[1] & 0xe0) === 0xe0; // frame sync
}

/**
 * Parses RIFF/WAVE: PCM int 8/16/24/32, IEEE float 32, and WAVE_FORMAT_EXTENSIBLE.
 * Tolerates streaming headers whose data size is 0 or 0xFFFFFFFF by reading to EOF.
 */
export function parseWav(bytes: Uint8Array): DecodedWav {
  if (!isWav(bytes)) throw new Error("not a RIFF/WAVE file");
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let offset = 12;
  let format = 0;
  let channels = 0;
  let rate = 0;
  let bits = 0;
  while (offset + 8 <= bytes.byteLength) {
    const id = ascii(bytes, offset);
    let size = view.getUint32(offset + 4, true);
    const body = offset + 8;
    if (id === "fmt ") {
      format = view.getUint16(body, true);
      channels = view.getUint16(body + 2, true);
      rate = view.getUint32(body + 4, true);
      bits = view.getUint16(body + 14, true);
      if (format === 0xfffe && size >= 26) format = view.getUint16(body + 24, true);
    } else if (id === "data") {
      if (!format) throw new Error("WAV data chunk before fmt chunk");
      if (size === 0 || size === 0xffffffff || body + size > bytes.byteLength) size = bytes.byteLength - body;
      return { rate, channels, samples: decodeSamples(bytes.subarray(body, body + size), format, bits) };
    }
    offset = body + size + (size & 1);
  }
  throw new Error("WAV has no data chunk");
}

function decodeSamples(data: Uint8Array, format: number, bits: number): Float32Array {
  const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
  const width = bits / 8;
  const n = Math.floor(data.byteLength / width);
  const out = new Float32Array(n);
  if (format === 3 && bits === 32) {
    for (let i = 0; i < n; i++) out[i] = view.getFloat32(i * 4, true);
    return out;
  }
  if (format !== 1) throw new Error(`unsupported WAV format ${format}`);
  switch (bits) {
    case 8:
      for (let i = 0; i < n; i++) out[i] = (data[i] - 128) / 128;
      break;
    case 16:
      for (let i = 0; i < n; i++) out[i] = view.getInt16(i * 2, true) / 32768;
      break;
    case 24:
      for (let i = 0; i < n; i++) {
        const o = i * 3;
        let v = data[o] | (data[o + 1] << 8) | (data[o + 2] << 16);
        if (v & 0x800000) v -= 0x1000000;
        out[i] = v / 8388608;
      }
      break;
    case 32:
      for (let i = 0; i < n; i++) out[i] = view.getInt32(i * 4, true) / 2147483648;
      break;
    default:
      throw new Error(`unsupported WAV bit depth ${bits}`);
  }
  return out;
}

export function toMono(interleaved: Float32Array, channels: number): Float32Array {
  if (channels <= 1) return interleaved;
  const frames = Math.floor(interleaved.length / channels);
  const out = new Float32Array(frames);
  for (let f = 0; f < frames; f++) {
    let sum = 0;
    for (let c = 0; c < channels; c++) sum += interleaved[f * channels + c];
    out[f] = sum / channels;
  }
  return out;
}

/**
 * Resamples mono float audio. Downsampling first applies a box low-pass whose
 * width is the decimation ratio (cheap anti-aliasing adequate for speech),
 * then linear interpolation.
 */
export function resample(input: Float32Array, from: number, to: number): Float32Array {
  if (from === to || input.length === 0) return input;
  let src = input;
  const ratio = from / to;
  if (ratio > 1) {
    const width = Math.max(1, Math.round(ratio));
    if (width > 1) {
      src = new Float32Array(input.length);
      let acc = 0;
      for (let i = 0; i < input.length; i++) {
        acc += input[i];
        if (i >= width) acc -= input[i - width];
        src[i] = acc / Math.min(i + 1, width);
      }
    }
  }
  const outLen = Math.max(1, Math.floor((input.length * to) / from));
  const out = new Float32Array(outLen);
  for (let i = 0; i < outLen; i++) {
    const pos = i * ratio;
    const j = Math.floor(pos);
    const frac = pos - j;
    const a = src[Math.min(j, src.length - 1)];
    const b = src[Math.min(j + 1, src.length - 1)];
    out[i] = a + (b - a) * frac;
  }
  return out;
}

export function floatToPcm16(input: Float32Array): Int16Array {
  const out = new Int16Array(input.length);
  for (let i = 0; i < input.length; i++) {
    // Symmetric with pcm16ToFloat (/32768), so 16-bit input round-trips exactly.
    out[i] = Math.max(-32768, Math.min(32767, Math.round(input[i] * 32768)));
  }
  return out;
}

export function pcm16ToFloat(input: Int16Array): Float32Array {
  const out = new Float32Array(input.length);
  for (let i = 0; i < input.length; i++) out[i] = input[i] / 32768;
  return out;
}

/** Little-endian bytes -> Int16Array, regardless of byte offset alignment. A trailing odd byte is dropped. */
export function bytesToPcm16(bytes: Uint8Array): Int16Array {
  const n = bytes.byteLength >> 1;
  const out = new Int16Array(n);
  const view = new DataView(bytes.buffer, bytes.byteOffset, n * 2);
  for (let i = 0; i < n; i++) out[i] = view.getInt16(i * 2, true);
  return out;
}

export function pcm16ToBytes(pcm: Int16Array): Uint8Array {
  const out = new Uint8Array(pcm.length * 2);
  const view = new DataView(out.buffer);
  for (let i = 0; i < pcm.length; i++) view.setInt16(i * 2, pcm[i], true);
  return out;
}

/** Any WAV or raw PCM16 mono at `rawRate` -> PCM16 mono @16 kHz. */
export function toPcm16k(bytes: Uint8Array, rawRate = SAMPLE_RATE): Int16Array {
  if (isWav(bytes)) {
    const wav = parseWav(bytes);
    const mono = toMono(wav.samples, wav.channels);
    if (wav.rate === SAMPLE_RATE && wav.channels <= 1) return floatToPcm16(mono);
    return floatToPcm16(resample(mono, wav.rate, SAMPLE_RATE));
  }
  if (isLikelyMp3(bytes)) throw new Error("TTS returned MP3; no MP3 decoder is bundled (request linear16/wav)");
  const pcm = bytesToPcm16(bytes);
  if (rawRate === SAMPLE_RATE) return pcm;
  return floatToPcm16(resample(pcm16ToFloat(pcm), rawRate, SAMPLE_RATE));
}

export function encodeWav(pcm: Int16Array, rate = SAMPLE_RATE): Uint8Array {
  const data = pcm.length * 2;
  const out = new Uint8Array(44 + data);
  const v = new DataView(out.buffer);
  const w = (o: number, s: string) => {
    for (let i = 0; i < 4; i++) out[o + i] = s.charCodeAt(i);
  };
  w(0, "RIFF");
  v.setUint32(4, 36 + data, true);
  w(8, "WAVE");
  w(12, "fmt ");
  v.setUint32(16, 16, true);
  v.setUint16(20, 1, true);
  v.setUint16(22, 1, true);
  v.setUint32(24, rate, true);
  v.setUint32(28, rate * 2, true);
  v.setUint16(32, 2, true);
  v.setUint16(34, 16, true);
  w(36, "data");
  v.setUint32(40, data, true);
  out.set(pcm16ToBytes(pcm), 44);
  return out;
}

export function levels(pcm: Int16Array): { peak: number; rms: number } {
  if (pcm.length === 0) return { peak: 0, rms: 0 };
  let peak = 0;
  let sum = 0;
  for (let i = 0; i < pcm.length; i++) {
    const a = Math.abs(pcm[i]);
    if (a > peak) peak = a;
    sum += pcm[i] * pcm[i];
  }
  const round = (x: number) => Math.round(x * 10000) / 10000;
  return { peak: round(peak / 32768), rms: round(Math.sqrt(sum / pcm.length) / 32768) };
}

export const durationMs = (samples: number, rate = SAMPLE_RATE) => Math.round((samples * 1000) / rate);
