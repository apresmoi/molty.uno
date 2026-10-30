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
      if (args.voice !== undefined) {
        if (typeof args.voice !== "string" || !/^[A-Za-z0-9_.:-]{1,64}$/.test(args.voice)) throw invalid("voice must be a short identifier");
        out.voice = args.voice;
      }
      return { name: n, args: out, job: true };
    }
    case "listen":
      only(args, ["ms"]);
      return { name: n, args: { ms: int(args.ms, "ms", 1, MAX_LISTEN_MS, DEFAULT_LISTEN_MS) }, job: true };
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
