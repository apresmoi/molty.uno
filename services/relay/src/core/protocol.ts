// Device <-> relay wire protocol (docs/AGENT-LINK.md, "Device <-> relay wire").
// Pure TypeScript: no Cloudflare imports anywhere under src/core.

export const DEVICE_ID_RE = /^molty-[a-z2-9]{10}$/;
export const LINK_CODE_RE = /^[A-Z2-9]{8}$/;
export const LINK_CODE_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ23456789";

/** Binary audio frame limits: `<JSON header>\n<payload>`. */
export const MAX_AUDIO_HEADER_BYTES = 512;
export const MAX_AUDIO_PAYLOAD_BYTES = 16 * 1024;
/** Text (JSON) frame cap. Device frames are small; this bounds parsing work. */
export const MAX_TEXT_FRAME_BYTES = 64 * 1024;

export const SAMPLE_RATE = 16000;
/** The longest listen buffered: an unlimited one (LISTEN_CEILING_MS, 300 s) + 1 s, 16 kHz pcm16. */
export const MAX_LISTEN_BYTES = 301000 * 32;

/** WebSocket close codes. 4401 is the spec's auth failure. */
export const CLOSE_UNAUTHORIZED = 4401;
export const CLOSE_PROTOCOL = 4400;
export const CLOSE_REPLACED = 4409;
export const CLOSE_FORGOTTEN = 4410;

/** The exact ping text the host can answer without waking the room. */
export const PING_TEXT = '{"t":"ping"}';
export const PONG_TEXT = '{"t":"pong"}';

export type Json = null | boolean | number | string | Json[] | { [k: string]: Json };
export type JsonObject = { [k: string]: Json };

export interface HelloFrame {
  t: "hello";
  id: string;
  key: string;
  fw?: string;
  caps?: string[];
  /** device/firmware/boards/ name of the hardware, e.g. "waveshare-esp32-s3-touch-lcd-1.28". */
  board?: string;
  link_code?: string;
}

export const BOARD_RE = /^[a-z0-9][a-z0-9.-]{0,63}$/;

export interface AudioHeader {
  s: string;
  seq: number;
  kind: "pcm16";
  rate: number;
  end: boolean;
  /**
   * Optional, device -> relay on the final listen frame: why recording stopped
   * (`silence` = end of speech, `no_speech` = nothing said, `max` = window
   * full, `error` = capture failed). Firmware without on-device end-of-speech omits it.
   */
  reason?: string;
  /** Optional, with `reason`: milliseconds of detected speech in the clip (0 = none). */
  speech_ms?: number;
  /**
   * Optional, relay -> device on say frames (firmware cap `say_cues`): word cues
   * [word, sample] — the say text's word `word` (0-based, whitespace-split) is spoken
   * from sample `sample` of the stream (16 kHz, from its first sample).
   */
  w?: Array<[number, number]>;
  /**
   * Optional, device -> relay on listen frames (firmware cap `trace`, docs/AGENT-LINK.md "Turn
   * trace"): `ct` capture ms of the frame's first sample from the listen's mic start; `lv` one
   * level byte per 100 ms chunk (hex, = clamp(round(2·(dBFS+100)), 0, 200)); `cl` samples
   * saturated after the mic gain; `rt` failed send attempts before it went out; `wq` ms it
   * waited on the device between capture and send. Malformed values are dropped, never fatal.
   */
  ct?: number;
  lv?: string;
  cl?: number;
  rt?: number;
  wq?: number;
  /** Optional, on the end frame: the device's listen summary (DEVICE_TRACE_KEYS, ints). */
  tr?: Record<string, number>;
}

/** The end frame's `tr` keys and their accepted ranges (anything else is dropped). */
export const DEVICE_TRACE_KEYS: Record<string, [number, number]> = {
  wall: [0, 600000],
  cap: [0, 600000],
  sent: [0, 100000],
  fail: [0, 100000],
  drop: [0, 100000],
  dropms: [0, 600000],
  r0: [-127, 0],
  r1: [-127, 0],
  gain: [1, 64],
  pk: [0, 1000],
  set: [0, 5000],
  setc: [0, 1],
};

const intIn = (v: unknown, lo: number, hi: number): v is number => typeof v === "number" && Number.isInteger(v) && v >= lo && v <= hi;

/** The trace fields of a device listen frame header (AudioHeader ct/lv/cl/rt/wq/tr), validated. */
function traceFields(h: Record<string, unknown>, out: AudioHeader) {
  if (intIn(h.ct, 0, 600000)) out.ct = h.ct;
  if (typeof h.lv === "string" && /^(?:[0-9a-fA-F]{2}){1,16}$/.test(h.lv)) out.lv = h.lv.toLowerCase();
  if (intIn(h.cl, 1, 100000)) out.cl = h.cl;
  if (intIn(h.rt, 1, 1000)) out.rt = h.rt;
  if (intIn(h.wq, 0, 600000)) out.wq = h.wq;
  if (isRecord(h.tr)) {
    const tr: Record<string, number> = {};
    for (const [k, [lo, hi]] of Object.entries(DEVICE_TRACE_KEYS)) if (intIn(h.tr[k], lo, hi)) tr[k] = h.tr[k] as number;
    if (Object.keys(tr).length) out.tr = tr;
  }
}

export function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

/** Parses a device text frame. Returns undefined for anything that is not a JSON object with a string `t`. */
export function parseTextFrame(text: string): (Record<string, unknown> & { t: string }) | undefined {
  if (text.length > MAX_TEXT_FRAME_BYTES) return undefined;
  try {
    const value: unknown = JSON.parse(text);
    if (isRecord(value) && typeof value.t === "string") return value as Record<string, unknown> & { t: string };
  } catch {
    /* fallthrough */
  }
  return undefined;
}

export function parseHello(frame: Record<string, unknown>): HelloFrame | undefined {
  if (frame.t !== "hello" || typeof frame.id !== "string" || typeof frame.key !== "string") return undefined;
  if (!DEVICE_ID_RE.test(frame.id)) return undefined;
  if (frame.fw !== undefined && typeof frame.fw !== "string") return undefined;
  if (frame.caps !== undefined && !(Array.isArray(frame.caps) && frame.caps.every((c) => typeof c === "string"))) return undefined;
  if (frame.board !== undefined && (typeof frame.board !== "string" || !BOARD_RE.test(frame.board))) return undefined;
  if (frame.link_code !== undefined && (typeof frame.link_code !== "string" || !LINK_CODE_RE.test(frame.link_code))) return undefined;
  return frame as unknown as HelloFrame;
}

const encoder = new TextEncoder();
const decoder = new TextDecoder();

export function encodeAudioFrame(header: AudioHeader, payload: Uint8Array): Uint8Array {
  if (payload.byteLength > MAX_AUDIO_PAYLOAD_BYTES) throw new Error("audio payload exceeds 16 KB");
  const head = encoder.encode(JSON.stringify(header));
  if (head.byteLength > MAX_AUDIO_HEADER_BYTES) throw new Error("audio header exceeds 512 B");
  const out = new Uint8Array(head.byteLength + 1 + payload.byteLength);
  out.set(head, 0);
  out[head.byteLength] = 10;
  out.set(payload, head.byteLength + 1);
  return out;
}

export type DecodedAudio = { header: AudioHeader; payload: Uint8Array };

/** Decodes and validates an audio frame. Header-only frames (no newline) are valid, e.g. a bare `end`. */
export function decodeAudioFrame(bytes: Uint8Array): DecodedAudio | undefined {
  const limit = Math.min(bytes.byteLength, MAX_AUDIO_HEADER_BYTES + 1);
  let nl = -1;
  for (let i = 0; i < limit; i++) {
    if (bytes[i] === 10) {
      nl = i;
      break;
    }
  }
  if (nl < 0 && bytes.byteLength > MAX_AUDIO_HEADER_BYTES) return undefined;
  const headEnd = nl < 0 ? bytes.byteLength : nl;
  let header: unknown;
  try {
    header = JSON.parse(decoder.decode(bytes.subarray(0, headEnd)));
  } catch {
    return undefined;
  }
  if (!isRecord(header)) return undefined;
  const { s, seq, kind, rate, end } = header;
  if (typeof s !== "string" || s.length === 0 || s.length > 128) return undefined;
  if (typeof seq !== "number" || !Number.isInteger(seq) || seq < 0) return undefined;
  if (kind !== "pcm16" || rate !== SAMPLE_RATE) return undefined;
  if (end !== undefined && typeof end !== "boolean") return undefined;
  const payload = nl < 0 ? new Uint8Array(0) : bytes.subarray(nl + 1);
  if (payload.byteLength > MAX_AUDIO_PAYLOAD_BYTES) return undefined;
  const out: AudioHeader = { s, seq, kind, rate, end: end === true };
  // Unknown or malformed reasons are dropped, never fatal.
  if (typeof header.reason === "string" && /^[a-z_]{1,16}$/.test(header.reason)) out.reason = header.reason;
  if (typeof header.speech_ms === "number" && Number.isInteger(header.speech_ms) && header.speech_ms >= 0 && header.speech_ms <= 600000) out.speech_ms = header.speech_ms;
  traceFields(header, out);
  return { header: out, payload };
}
