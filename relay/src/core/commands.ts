// Agent-facing command validation, shared by every surface (REST, MCP, and
// later the OpenClaw node adapter). Produces the args sent to the device.

import { isRecord, type JsonObject } from "./protocol.js";

export const MOODS = ["idle", "listening", "thinking", "speaking", "happy", "sleepy", "surprised"] as const;
export const COMMANDS = ["status", "state", "mood", "show", "say", "listen", "volume", "imu", "diag", "log"] as const;
export type CommandName = (typeof COMMANDS)[number];

export const MAX_SHOW_BYTES = 16384;
export const MAX_SAY_BYTES = 4096;
export const MAX_LISTEN_MS = 10000;
export const DEFAULT_LISTEN_MS = 4000;
export const DEFAULT_WPM = 300;
export const MAX_DIAG_TIMELINE_S = 180;
export const MAX_LOG_BYTES = 32768;
/** mood hold: seconds before the device reverts (quick moods revert by default). */
export const MAX_MOOD_HOLD_S = 3600;
/** talk: default listen window. With device end-of-speech this is a ceiling, not a fixed length. */
export const TALK_DEFAULT_LISTEN_MS = MAX_LISTEN_MS;
/**
 * Longest a blocking call (talk, say/listen with wait) holds the request open.
 * Under the ~30 s tool-call timeouts common in MCP clients; past it the call
 * returns {job, status:"pending"} and molty_wait / GET /v1/jobs finishes it.
 */
export const TALK_BUDGET_MS = 24000;
/** Automatic `thinking` after end of speech reverts to idle after this long without a next command. */
export const THINK_HOLD_S = 15;

export class CommandError extends Error {
  constructor(
    readonly code: string,
    message: string,
  ) {
    super(message);
  }
}

const invalid = (message: string) => new CommandError("invalid_args", message);

function only(args: Record<string, unknown>, allowed: string[]) {
  for (const key of Object.keys(args)) {
    if (!allowed.includes(key)) throw invalid(`unknown argument ${key}`);
    if (args[key] === null) throw invalid(`${key} must not be null`);
  }
}

function text(value: unknown, field: string, maxBytes: number): string {
  if (typeof value !== "string" || value.length === 0) throw invalid(`${field} must be a nonempty string`);
  if (value.includes("\u0000")) throw invalid(`${field} must not contain NUL`);
  if (new TextEncoder().encode(value).byteLength > maxBytes) throw invalid(`${field} exceeds ${maxBytes} bytes`);
  return value;
}

function int(value: unknown, field: string, min: number, max: number, fallback?: number): number {
  if (value === undefined && fallback !== undefined) return fallback;
  if (typeof value !== "number" || !Number.isInteger(value) || value < min || value > max) {
    throw invalid(`${field} must be an integer ${min}..${max}`);
  }
  return value;
}

function voiceId(v: unknown): string {
  if (typeof v !== "string" || !/^[A-Za-z0-9_.:-]{1,64}$/.test(v)) throw invalid("voice must be a short identifier");
  return v;
}

/** Device end-of-speech knobs for listen (firmware VAD); passed through only when given. */
export const VAD_KEYS = ["end_silence_ms", "no_speech_ms", "vad"] as const;
export const VAD_LIMITS = { end_silence_ms: [300, 3000], no_speech_ms: [1000, 10000] } as const;

function vadArgs(args: Record<string, unknown>): JsonObject {
  const out: JsonObject = {};
  for (const k of ["end_silence_ms", "no_speech_ms"] as const) {
    if (args[k] !== undefined) out[k] = int(args[k], k, VAD_LIMITS[k][0], VAD_LIMITS[k][1]);
  }
  if (args.vad !== undefined) {
    if (typeof args.vad !== "boolean") throw invalid("vad must be a boolean");
    out.vad = args.vad;
  }
  return out;
}

export interface Validated {
  name: CommandName;
  /** Args forwarded to the device. */
  args: JsonObject;
  /** say/listen are jobs: the agent gets `{job}` immediately. */
  job: boolean;
}

export function validateCommand(name: unknown, rawArgs: unknown): Validated {
  if (typeof name !== "string" || !(COMMANDS as readonly string[]).includes(name)) {
    throw new CommandError("unknown_command", `unknown command ${String(name)}`);
  }
  const args = rawArgs === undefined || rawArgs === null ? {} : rawArgs;
  if (!isRecord(args)) throw invalid("args must be an object");
  const n = name as CommandName;
  switch (n) {
    case "status":
    case "imu":
      only(args, []);
      return { name: n, args: {}, job: false };
    case "state": {
      // Empty = read. `{state:{...}}` or flat fields = patch (passed through
      // flat, like the firmware's POST /api/creature/state body).
      if (Object.keys(args).length === 0) return { name: n, args: {}, job: false };
      const keys = Object.keys(args);
      const patch = keys.length === 1 && keys[0] === "state" ? args.state : args;
      if (!isRecord(patch)) throw invalid("state must be an object");
      if (JSON.stringify(patch).length > 8192) throw invalid("state patch exceeds 8192 bytes");
      return { name: n, args: patch as JsonObject, job: false };
    }
    case "mood":
      only(args, ["mood", "hold_s"]);
      if (typeof args.mood !== "string" || !(MOODS as readonly string[]).includes(args.mood)) {
        throw invalid(`mood must be one of ${MOODS.join(", ")}`);
      }
      if (args.hold_s !== undefined) {
        const h = args.hold_s;
        if (typeof h !== "number" || !Number.isFinite(h) || h < 0 || h > MAX_MOOD_HOLD_S) throw invalid(`hold_s must be a number 0 <= hold_s <= ${MAX_MOOD_HOLD_S} (0 = until the next mood)`);
        return { name: n, args: { mood: args.mood, hold_s: h }, job: false };
      }
      return { name: n, args: { mood: args.mood }, job: false };
    case "show":
      only(args, ["text", "wpm"]);
      return {
        name: n,
        args: { text: text(args.text, "text", MAX_SHOW_BYTES), wpm: int(args.wpm, "wpm", 100, 800, DEFAULT_WPM) },
        job: false,
      };
    case "say": {
      only(args, ["text", "voice"]);
      const out: JsonObject = { text: text(args.text, "text", MAX_SAY_BYTES) };
      if (args.voice !== undefined) out.voice = voiceId(args.voice);
      return { name: n, args: out, job: true };
    }
    case "listen":
      only(args, ["ms", ...VAD_KEYS]);
      return { name: n, args: { ms: int(args.ms, "ms", 1, MAX_LISTEN_MS, DEFAULT_LISTEN_MS), ...vadArgs(args) }, job: true };
    case "diag": {
      only(args, ["timeline"]);
      const out: JsonObject = {};
      if (args.timeline !== undefined) out.timeline = int(args.timeline, "timeline", 0, MAX_DIAG_TIMELINE_S);
      return { name: n, args: out, job: false };
    }
    case "log": {
      only(args, ["since", "max"]);
      const out: JsonObject = {};
      if (args.since !== undefined) out.since = int(args.since, "since", 0, 2 ** 31 - 1);
      if (args.max !== undefined) out.max = int(args.max, "max", 1024, MAX_LOG_BYTES);
      return { name: n, args: out, job: false };
    }
    case "volume":
      only(args, ["volume"]);
      return { name: n, args: { volume: int(args.volume, "volume", 0, 21) }, job: false };
  }
}

export interface TalkArgs {
  say?: string;
  voice?: string;
  listen: boolean;
  listen_ms: number;
  /** Device VAD knobs (end_silence_ms, no_speech_ms, vad), forwarded to the device's listen. */
  vad: JsonObject;
}

/** molty_talk / POST /v1/devices/<id>/talk: `{say?, listen? (default true), listen_ms? (1..10000, default 10000), voice?, end_silence_ms?, no_speech_ms?, vad?}`. */
export function validateTalk(raw: unknown): TalkArgs {
  const args = raw === undefined || raw === null ? {} : raw;
  if (!isRecord(args)) throw invalid("args must be an object");
  only(args, ["say", "listen", "listen_ms", "voice", ...VAD_KEYS]);
  if (args.listen !== undefined && typeof args.listen !== "boolean") throw invalid("listen must be a boolean");
  const out: TalkArgs = { listen: args.listen !== false, listen_ms: int(args.listen_ms, "listen_ms", 1, MAX_LISTEN_MS, TALK_DEFAULT_LISTEN_MS), vad: vadArgs(args) };
  if (args.say !== undefined) out.say = text(args.say, "say", MAX_SAY_BYTES);
  if (args.voice !== undefined) out.voice = voiceId(args.voice);
  if (out.say === undefined && !out.listen) throw invalid("give say, or listen (or both)");
  return out;
}
