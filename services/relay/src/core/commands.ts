// Agent-facing command validation, shared by every surface (REST, MCP, and
// later the OpenClaw node adapter). Produces the args sent to the device.

import { isRecord, type JsonObject } from "./protocol.js";
import { parseCues, speechWords } from "./speech.js";

export const MOODS = ["idle", "listening", "thinking", "speaking", "happy", "sleepy", "surprised", "working"] as const;
/** The shared one-shot gestures every creature plays (`act`, and `[name]` cues in say text). A creature package adds its
 *  own (the cat's eat): `act` takes any gesture name, and the device answers unknown_gesture for one its creature lacks
 *  (its gestures: the device's creature event, device.creature.gestures). */
export const GESTURE_RE = /^[a-z][a-z0-9_]{0,23}$/;
export const ACTIONS = ["wiggle", "nod", "shake_head", "bounce", "peek", "glance_left", "glance_right", "blink", "wobble", "stretch", "look_up", "yawn", "inspect", "paddle"] as const;
/** Need the firmware cap `working`: older firmware refuses them (a say's cue is dropped, not refused). */
export const WORKING_CAP = "working";
export const WORKING_ACTIONS: readonly string[] = ["inspect", "paddle"];
/** `progress {pct?, step?, done?}`: needs the firmware cap `progress` (a line fills the ring while the creature works). */
export const PROGRESS_CAP = "progress";
export const MAX_PROGRESS_STEP = 80;
/** A say's `[working]` cue: never a device mark. The relay plays the say to its end, then sets the mood. */
export const WORKING_MOOD = "working";
export const COMMANDS = ["status", "state", "mood", "show", "say", "quick", "keep", "listen", "volume", "imu", "diag", "log", "act", "buzz", "progress", "gestures", "pkg.install", "pkg.remove", "pkg.list", "pkg.available", "scene.play", "scene.queue", "scene.clear", "creature.select", "creature.list", "rhythm.play", "rhythm.set", "rhythm.stop", "front"] as const;
/** `buzz {pattern | ms, strength?}`: the vibration motor (firmware cap `haptic`, only on boards that have one). A pattern is
 *  a name the device knows (tick, tap, double, nudge: unknown_pattern otherwise) or [on, off, on, ...] ms. */
export const HAPTIC_CAP = "haptic";
export const BUZZ_NAME_RE = /^[a-z][a-z0-9_]{0,15}$/;
export const MAX_BUZZ_SEGS = 8;
export const MAX_BUZZ_MS = 2000;
/** What the human's inputs can do (`gestures`; firmware: sense/gesture.h). */
export const GESTURE_INPUTS = ["tap", "double_tap", "button_accept", "button_reject"] as const;
export const GESTURE_ACTIONS = ["none", "talk", "event", "stop", "screen", "hold"] as const;
/** The device reports do not disturb (its `dnd` event): the relay holds back unprompted speech. */
export const DND_CAP = "dnd";
export const GESTURES_CAP = "gestures";
/** Firmware that keeps a say or listen through a short link drop (its hello names it; welcome answers). */
export const RESUME_CAP = "resume";
export const RESUME_FEATURE = "resume";
export type CommandName = (typeof COMMANDS)[number];

// ---- packages (docs/PACKAGES.md): data-only drawings a Molty downloads itself, played with a few params ----

/** Firmware caps: pkg.* need `pkg`, scene.* need `scene`, creature.* need `creatures`. Anything else is a clean 409 unsupported. */
export const PKG_CAP = "pkg";
export const SCENE_CAP = "scene";
export const CREATURES_CAP = "creatures";
export const PKG_ID_RE = /^[a-z0-9-]{1,32}$/;
/** A package file's ceiling; the device downloads it straight from the url: its bytes never pass through the relay. */
export const MAX_PKG_BYTES = 262144;
export const MAX_PKG_URL_BYTES = 512;
/** A scene's params, as JSON: a few hundred bytes for a drawing; a layout with spots carries their readings (the device holds 4096 characters of them). */
export const MAX_SCENE_PARAMS_BYTES = 8192;
export const DEFAULT_SCENE_TTL_S = 60;
export const MAX_SCENE_TTL_S = 600;

// ---- the beat player (docs/AGENT-LINK.md "Rhythm"): a library of patterns and a tempo, sent once; Molty loops it ----

/** Firmware cap: rhythm.* (boards with a speaker). */
export const RHYTHM_CAP = "rhythm";
/** rhythm.play's library as JSON: up to 16 patterns of a few hundred bytes (the device checks each strictly). */
export const MAX_RHYTHM_BYTES = 32768;
export const RHYTHM_BPM = [40, 220] as const;
export const MAX_RHYTHMS = 96;
/** rhythm.play's `regions` (the wheel's level above the rhythms): up to 16 {label, color}. */
export const MAX_RHYTHM_REGIONS = 16;
/** `sync`: the picture against the sound, ms, kept on the device. */
export const RHYTHM_SYNC_MS = 250;
function syncArg(v: unknown): number {
  if (typeof v !== "number" || !Number.isInteger(v) || Math.abs(v) > RHYTHM_SYNC_MS) throw invalid(`sync must be a whole number of ms, -${RHYTHM_SYNC_MS}..${RHYTHM_SYNC_MS}`);
  return v;
}
const RHYTHM_LAYER_RE = /^[a-z][a-z0-9-]{0,15}$/;
function bpmArg(v: unknown): number {
  if (typeof v !== "number" || !Number.isFinite(v) || v < RHYTHM_BPM[0] || v > RHYTHM_BPM[1]) throw invalid(`bpm must be a number ${RHYTHM_BPM[0]}..${RHYTHM_BPM[1]}`);
  return v;
}
function muteArg(v: unknown): string[] {
  if (!Array.isArray(v) || v.length > 4 || !v.every((x) => typeof x === "string" && RHYTHM_LAYER_RE.test(x))) throw invalid("mute must be a list of up to 4 layer ids");
  return v as string[];
}
function boolArg(v: unknown, name: string): boolean {
  if (typeof v !== "boolean") throw invalid(`${name} must be a boolean`);
  return v;
}

export const MAX_SHOW_BYTES = 16384;
export const MAX_SAY_BYTES = 4096;
/** keep: the most first words a reply can continue after (a line is at most OPENER.maxWords). */
export const MAX_KEEP_WORDS = 64;
/** Inline `[mood]` / `[action]` cues kept per say (core/speech.ts parseCues): later ones are dropped. */
export const MAX_SAY_MARKS = 16;
/** A say's marks: [word index of the stripped text, mood or action name]; fired when that word's cue plays. */
export type SayMarks = Array<[number, string]>;
/**
 * The listen window (ms). Every listen ends early anyway (the device's end of speech, or the
 * voice box's semantic end of turn): the window is a ceiling. `0` = unlimited — it ends only
 * on end of turn / speech, no_speech, or LISTEN_CEILING_MS.
 */
export const MAX_LISTEN_MS = 30000;
/** The out-of-the-box window. An agent that omits ms / listen_ms gets the DEVICE's setting
 *  (console: 5..30 s or unlimited, kept on the device, like ring_color) — this is its default. */
export const DEFAULT_LISTEN_MS = 30000;
export const LISTEN_UNLIMITED = 0;
/** An unlimited listen's safety backstop (reason "max"): a stuck mic or a noisy room can't stream forever. */
export const LISTEN_CEILING_MS = 300000;
/** Firmware without the `listen_window` cap: 1..10000 ms only, and the relay always sends one. */
export const LEGACY_MAX_LISTEN_MS = 10000;
/** The firmware cap: ms optional (its own setting), 0..MAX_LISTEN_MS, the window in the listen's ack. */
export const LISTEN_WINDOW_CAP = "listen_window";
export const DEFAULT_WPM = 300;
export const MAX_DIAG_TIMELINE_S = 180;
export const MAX_LOG_BYTES = 32768;
/** mood hold: seconds before the device reverts (quick moods revert by default). */
export const MAX_MOOD_HOLD_S = 3600;
/** talk: default listen window — the same as listen's (a ceiling, not a fixed length). */
export const TALK_DEFAULT_LISTEN_MS = DEFAULT_LISTEN_MS;
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

/**
 * Say text with inline cues (`[happy] Build passed! [nod] …`): the raw text is size-checked,
 * then the cues are stripped (never spoken, never shown) and returned as marks.
 */
function sayText(value: unknown, field: string): { text: string; marks: SayMarks } {
  const raw = text(value, field, MAX_SAY_BYTES);
  const parsed = parseCues(raw);
  if (speechWords(parsed.text).length === 0) {
    throw invalid(parsed.text === raw.trim() ? `${field} has no words to speak` : `${field} has nothing to speak after removing its [cues]: use mood or act for a gesture without speech`);
  }
  return parsed;
}

function int(value: unknown, field: string, min: number, max: number, fallback?: number): number {
  if (value === undefined && fallback !== undefined) return fallback;
  if (typeof value !== "number" || !Number.isInteger(value) || value < min || value > max) {
    throw invalid(`${field} must be an integer ${min}..${max}`);
  }
  return value;
}

function pkgId(v: unknown, field: string): string {
  if (typeof v !== "string" || !PKG_ID_RE.test(v)) throw invalid(`${field} must be 1..32 of a-z 0-9 -`);
  return v;
}

function voiceId(v: unknown): string {
  if (typeof v !== "string" || !/^[A-Za-z0-9_.:-]{1,64}$/.test(v)) throw invalid("voice must be a short identifier");
  return v;
}

// ---- colours: ask buttons and the listening ring ----

/**
 * Named colours an agent may use (ask options, ring_color). Light, saturated tones that read on
 * the dark fluid at the dim end of its brightness (the fluid multiplies a cell's colour by its
 * ink); green / red / amber are the website's yes / no / idk inks (--m-yes/--m-no/--m-idk).
 */
export const COLORS = {
  green: "#3ccf96",
  red: "#ff6b6b",
  amber: "#ffa94d",
  blue: "#4dabf7",
  violet: "#b197fc",
  pink: "#f783ac",
  cyan: "#3bc9db",
  white: "#f1f3f5",
} as const;
export type ColorName = keyof typeof COLORS;
/** Ask options without a colour take these by index. */
export const DEFAULT_ASK_COLORS: readonly ColorName[] = ["green", "red", "amber", "blue", "violet"];

/** A palette name or #rrggbb → lowercase "#rrggbb" (what the device gets). */
export function resolveColor(value: unknown, field: string): string {
  if (typeof value === "string") {
    const v = value.trim().toLowerCase();
    if (v in COLORS) return COLORS[v as ColorName];
    if (/^#[0-9a-f]{6}$/.test(v)) return v;
  }
  throw invalid(`${field} must be one of ${Object.keys(COLORS).join(", ")} or a hex colour like "#3ccf96"`);
}

/** Device end-of-speech knobs for listen (firmware VAD); passed through only when given. */
export const VAD_KEYS = ["end_silence_ms", "no_speech_ms", "vad"] as const;
export const VAD_LIMITS = { end_silence_ms: [300, 3000], no_speech_ms: [1000, 10000] } as const;
/** The device's no_speech_ms default (device/firmware/main/audio/vad.h): what the voice box gets when the agent gave none. */
export const DEVICE_NO_SPEECH_MS = 4000;
/**
 * The voice box's no_speech_ms when the agent gave none (semantic end of turn): longer than the
 * device's 4 s — the box decides from the model's voice activity on captured audio, and a human
 * who starts after a pause (reaction time after Molty's say) must not be cut off.
 */
export const BOX_NO_SPEECH_MS = 8000;

/**
 * The turn acknowledgement (firmware cap `ack`): when a listen that heard speech ends, the
 * DEVICE itself plays a short chirp and shows `thinking` (ACK_THINK_S at most, or until the
 * agent's next say / mood / ask / listen) — no network, before any agent answers. The human's
 * setting (console: chirp + thinking | thinking only | off) applies; an agent's `ack` on
 * listen / talk can only turn it down: true (default) = the setting, "silent" = no chirp,
 * false = none. The device reports what it will do in the listen's ack (`ack`: "chirp" |
 * "thinking" | "off"): with it, the relay sends no automatic `thinking` of its own.
 */
export const ACK_CAP = "ack";
export const ACK_THINK_S = 30;
export type AckArg = boolean | "silent";
export const ACK_MODES = ["chirp", "thinking", "off"] as const;
function ackArg(v: unknown): AckArg | undefined {
  if (v === undefined) return undefined;
  if (typeof v === "boolean" || v === "silent") return v;
  throw invalid('ack must be true, false or "silent"');
}

/** A listen window: undefined (the device's setting), 0 (unlimited) or 1..MAX_LISTEN_MS. */
function listenMs(v: unknown, field: string): number | undefined {
  if (v === undefined) return undefined;
  if (typeof v !== "number" || !Number.isInteger(v) || v < 0 || v > MAX_LISTEN_MS) {
    throw invalid(`${field} must be an integer 1..${MAX_LISTEN_MS}, or 0 for unlimited (ends on end of turn; ${LISTEN_CEILING_MS / 60000} min safety ceiling)`);
  }
  return v;
}

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

/** A quick phrase's id (core/phrases.ts): 1..24 of a-z 0-9 _ -. */
export const PHRASE_ID_RE = /^[a-z0-9][a-z0-9_-]{0,23}$/;

export interface Validated {
  name: CommandName;
  /** Args forwarded to the device. */
  args: JsonObject;
  /** say/listen/pkg.install are jobs: the agent gets `{job}` immediately. */
  job: boolean;
  /** say: the human turn (an `utterance`'s turn_id) this answers (core/turns.ts); relay-only. */
  in_reply_to?: string;
  /** say: the agent's own latency report (clientTiming); relay-only. */
  client_timing?: JsonObject;
}

/**
 * `client_timing` (say / talk / ask; relay-only, never sent to the device): the agent's own
 * latency report for this call — up to CLIENT_TIMING_KEYS keys of ms, e.g. received_ms (event in
 * hand -> call), model_ms, tool_ms. Kept in the job's timings.client and the turn's trace.
 */
export const CLIENT_TIMING_KEYS = 8;
export const CLIENT_TIMING_MAX_MS = 3600000;
const CLIENT_TIMING_KEY_RE = /^[a-z][a-z0-9_]{0,31}$/;
export function clientTiming(v: unknown): JsonObject | undefined {
  if (v === undefined) return undefined;
  const msg = `client_timing must be an object of up to ${CLIENT_TIMING_KEYS} numbers (ms, 0..${CLIENT_TIMING_MAX_MS}) with keys like received_ms, model_ms (lower case, digits, _; <= 32 chars)`;
  if (!isRecord(v)) throw invalid(msg);
  const keys = Object.keys(v);
  if (keys.length > CLIENT_TIMING_KEYS) throw invalid(msg);
  const out: JsonObject = {};
  for (const k of keys) {
    const x = v[k];
    if (!CLIENT_TIMING_KEY_RE.test(k) || typeof x !== "number" || !Number.isFinite(x) || x < 0 || x > CLIENT_TIMING_MAX_MS) throw invalid(msg);
    out[k] = Math.round(x * 10) / 10;
  }
  return out;
}

/** An `utterance`'s turn_id (the listen job's id): what say / talk name in `in_reply_to`. */
const TURN_ID_RE = /^molty-[a-z2-9]{10}\.j_[A-Za-z0-9_-]{8,32}$/;
function turnId(v: unknown): string | undefined {
  if (v === undefined) return undefined;
  if (typeof v !== "string" || !TURN_ID_RE.test(v)) throw invalid("in_reply_to must be an utterance's turn_id (molty-xxxxxxxxxx.j_…)");
  return v;
}

/** The voice front's silent context (`front {context}`): at most this many characters. */
export const MAX_FRONT_CONTEXT = 2000;
/** `front {context, persona | facts: true}`: who the agent is / its fresh facts (each kept per device), at most this many characters. */
export const MAX_FRONT_PERSONA = 1500;

/** say / talk `exact`: word for word; with the voice front on, the front keeps the wording as close as it can (core/front.ts). */
function exactArg(v: unknown): boolean {
  if (v === undefined) return false;
  if (typeof v !== "boolean") throw invalid("exact must be a boolean");
  return v;
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
      only(args, ["text", "voice", "in_reply_to", "client_timing", "exact"]);
      // relay-only `marks` (the text's [cues]): room.ts sends them to firmware with say_marks
      const said = sayText(args.text, "text");
      const out: JsonObject = { text: said.text };
      if (said.marks.length) out.marks = said.marks;
      if (args.voice !== undefined) out.voice = voiceId(args.voice);
      if (exactArg(args.exact)) out.exact = true;
      const reply = turnId(args.in_reply_to);
      const ct = clientTiming(args.client_timing);
      return { name: n, args: out, job: true, ...(reply !== undefined ? { in_reply_to: reply } : {}), ...(ct ? { client_timing: ct } : {}) };
    }
    case "quick": {
      // a cached phrase (core/phrases.ts): {id, in_reply_to?, force?}; relay-only, never forwarded as is
      only(args, ["id", "in_reply_to", "force"]);
      if (typeof args.id !== "string" || !PHRASE_ID_RE.test(args.id)) throw invalid('id must be a phrase id (1..24 of a-z 0-9 _ -), e.g. "hmm"');
      if (args.force !== undefined && typeof args.force !== "boolean") throw invalid("force must be a boolean");
      const reply = turnId(args.in_reply_to);
      return { name: n, args: { id: args.id, ...(args.force === true ? { force: true } : {}) }, job: true, ...(reply !== undefined ? { in_reply_to: reply } : {}) };
    }
    case "keep": {
      // first words (core/opener.ts): {words?, in_reply_to?}; relay-only. No words: where the line is
      only(args, ["words", "in_reply_to"]);
      const reply = turnId(args.in_reply_to);
      const out: JsonObject = args.words === undefined ? {} : { words: int(args.words, "words", 0, MAX_KEEP_WORDS) };
      return { name: n, args: out, job: false, ...(reply !== undefined ? { in_reply_to: reply } : {}) };
    }
    case "listen": {
      only(args, ["ms", "ring_color", "ack", ...VAD_KEYS]);
      const ms = listenMs(args.ms, "ms");
      const out: JsonObject = { ...(ms !== undefined ? { ms } : {}), ...vadArgs(args) };
      if (args.ring_color !== undefined) out.ring_color = resolveColor(args.ring_color, "ring_color");
      const ack = ackArg(args.ack);
      if (ack !== undefined) out.ack = ack;
      return { name: n, args: out, job: true };
    }
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
    case "gestures": {
      // Empty = read; {bindings: {input: action, ...}} changes those inputs and keeps the rest.
      only(args, ["bindings"]);
      if (args.bindings === undefined) return { name: n, args: {}, job: false };
      if (!isRecord(args.bindings) || Object.keys(args.bindings).length === 0) throw invalid("bindings must be a non-empty object");
      for (const [input, action] of Object.entries(args.bindings)) {
        if (!(GESTURE_INPUTS as readonly string[]).includes(input)) throw invalid(`bindings keys must be among ${GESTURE_INPUTS.join(", ")}`);
        if (typeof action !== "string" || !(GESTURE_ACTIONS as readonly string[]).includes(action)) throw invalid(`${input} must be one of ${GESTURE_ACTIONS.join(", ")}`);
      }
      return { name: n, args: { bindings: args.bindings as JsonObject }, job: false };
    }
    case "pkg.install": {
      // The device asks its owner with one tap, then downloads, verifies and stores: a job (room.ts runPkgInstall).
      only(args, ["id", "url", "sha256", "size", "name"]);
      const url = text(args.url, "url", MAX_PKG_URL_BYTES);
      // the host allowlist is the firmware's (one constant there); the relay only checks the shape
      if (!/^https:\/\/[\x21-\x7e]+$/.test(url)) throw invalid("url must be an https URL without spaces");
      if (typeof args.sha256 !== "string" || !/^[0-9a-f]{64}$/.test(args.sha256)) throw invalid("sha256 must be 64 lower-case hex digits");
      if (typeof args.name !== "string" || !/^[\x20-\x7e]{1,32}$/.test(args.name)) throw invalid("name must be 1..32 printable ASCII characters (it is shown on Molty)");
      return { name: n, args: { id: pkgId(args.id, "id"), url, sha256: args.sha256, size: int(args.size, "size", 1, MAX_PKG_BYTES), name: args.name }, job: true };
    }
    case "pkg.remove":
      only(args, ["id"]);
      return { name: n, args: { id: pkgId(args.id, "id") }, job: false };
    case "creature.select":
      only(args, ["id"]);
      return { name: n, args: { id: pkgId(args.id, "id") }, job: false };
    case "creature.list":
    case "pkg.list":
    case "pkg.available":
    case "scene.clear":
      only(args, []);
      return { name: n, args: {}, job: false };
    case "scene.play":
    case "scene.queue": {
      only(args, ["pkg", "layout", "params", "ttl_s"]);
      const params = args.params ?? {};
      if (!isRecord(params)) throw invalid("params must be an object of the layout's params");
      if (new TextEncoder().encode(JSON.stringify(params)).byteLength > MAX_SCENE_PARAMS_BYTES) throw invalid(`params exceed ${MAX_SCENE_PARAMS_BYTES} bytes: send a few params, the package holds the drawing`);
      return { name: n, args: { pkg: pkgId(args.pkg, "pkg"), layout: pkgId(args.layout, "layout"), params: params as JsonObject, ttl_s: int(args.ttl_s, "ttl_s", 1, MAX_SCENE_TTL_S, DEFAULT_SCENE_TTL_S) }, job: false };
    }
    case "rhythm.play": {
      // the shape only: the device checks every pattern strictly (rhythm_wire.c) and answers invalid_args
      only(args, ["rhythms", "regions", "pick", "bpm", "mute", "buzz", "paused", "sync"]);
      const r = args.rhythms;
      if (!Array.isArray(r) || r.length < 1 || r.length > MAX_RHYTHMS || !r.every(isRecord)) throw invalid(`rhythms must be a list of 1..${MAX_RHYTHMS} patterns (the molty-rhythm skill's library)`);
      if (new TextEncoder().encode(JSON.stringify(r)).byteLength > MAX_RHYTHM_BYTES) throw invalid(`rhythms exceed ${MAX_RHYTHM_BYTES} bytes`);
      const out: JsonObject = { rhythms: r as JsonObject[], pick: int(args.pick, "pick", 0, r.length - 1), bpm: bpmArg(args.bpm) };
      if (args.regions !== undefined) {
        const g = args.regions;
        if (!Array.isArray(g) || g.length < 1 || g.length > MAX_RHYTHM_REGIONS || !g.every(isRecord)) throw invalid(`regions must be a list of 1..${MAX_RHYTHM_REGIONS} {label, color}`);
        out.regions = g as JsonObject[];
      }
      if (args.sync !== undefined) out.sync = syncArg(args.sync);
      if (args.mute !== undefined) out.mute = muteArg(args.mute);
      if (args.buzz !== undefined) out.buzz = boolArg(args.buzz, "buzz");
      if (args.paused !== undefined) out.paused = boolArg(args.paused, "paused");
      return { name: n, args: out, job: false };
    }
    case "rhythm.set": {
      only(args, ["bpm", "pick", "mute", "next", "buzz", "paused", "sync"]);
      if (!Object.keys(args).length) throw invalid("rhythm.set needs bpm, pick, mute, next, buzz, paused or sync");
      const out: JsonObject = {};
      if (args.bpm !== undefined) out.bpm = bpmArg(args.bpm);
      if (args.pick !== undefined) out.pick = int(args.pick, "pick", 0, MAX_RHYTHMS - 1);
      if (args.mute !== undefined) out.mute = muteArg(args.mute);
      if (args.next !== undefined) { if (args.next !== true) throw invalid("next must be true"); out.next = true; }
      if (args.buzz !== undefined) out.buzz = boolArg(args.buzz, "buzz");
      if (args.paused !== undefined) out.paused = boolArg(args.paused, "paused");
      if (args.sync !== undefined) out.sync = syncArg(args.sync);
      return { name: n, args: out, job: false };
    }
    case "rhythm.stop":
      only(args, []);
      return { name: n, args: {}, job: false };
    case "progress": {
      // relay-checked; step is the agent's words for the activity log (the device draws no text)
      only(args, ["pct", "step", "done"]);
      const out: JsonObject = {};
      if (args.pct !== undefined) {
        if (typeof args.pct !== "number" || !Number.isFinite(args.pct) || args.pct < 0 || args.pct > 100) throw invalid("pct must be a number 0..100 (leave it out when you can't tell how far along you are)");
        out.pct = args.pct;
      }
      if (args.step !== undefined) out.step = text(args.step, "step", MAX_PROGRESS_STEP);
      if (args.done !== undefined) {
        if (typeof args.done !== "boolean") throw invalid("done must be a boolean");
        out.done = args.done;
      }
      return { name: n, args: out, job: false };
    }
    case "buzz": {
      // relay-checked shape; the device knows the names. in_reply_to: relay-only (do not disturb lets a reply through)
      only(args, ["pattern", "ms", "strength", "in_reply_to"]);
      if ((args.pattern === undefined) === (args.ms === undefined)) throw invalid('give pattern (a name, e.g. "double", or [on, off, on, ...] ms) or ms, not both');
      const out: JsonObject = {};
      if (args.ms !== undefined) out.ms = int(args.ms, "ms", 1, MAX_BUZZ_MS);
      else if (typeof args.pattern === "string") {
        if (!BUZZ_NAME_RE.test(args.pattern)) throw invalid("pattern must be a name (tick, tap, double, nudge) or an array of ms");
        out.pattern = args.pattern;
      } else {
        const p = args.pattern;
        if (!Array.isArray(p) || p.length < 1 || p.length > MAX_BUZZ_SEGS) throw invalid(`pattern must be a name or 1..${MAX_BUZZ_SEGS} ms: on, off, on, ...`);
        out.pattern = p.map((v, i) => int(v, `pattern[${i}]`, 0, MAX_BUZZ_MS));
      }
      if (args.strength !== undefined) out.strength = int(args.strength, "strength", 0, 100);
      const reply = turnId(args.in_reply_to);
      return { name: n, args: out, job: false, ...(reply !== undefined ? { in_reply_to: reply } : {}) };
    }
    case "front": {
      // the voice front (core/front.ts): {context?, persona? | facts?, end?}; relay-only, never forwarded to the device
      only(args, ["context", "persona", "facts", "end"]);
      const out: JsonObject = {};
      if (args.context !== undefined) {
        if (typeof args.context !== "string" || !args.context.trim() || args.context.length > MAX_FRONT_CONTEXT) throw invalid(`context must be text of 1..${MAX_FRONT_CONTEXT} characters`);
        out.context = args.context.trim();
      }
      for (const k of ["persona", "facts"] as const) {
        if (args[k] !== undefined && (typeof args[k] !== "boolean" || (args[k] && (typeof out.context !== "string" || out.context.length > MAX_FRONT_PERSONA)))) {
          throw invalid(`${k}: true takes a context of 1..${MAX_FRONT_PERSONA} characters (${k === "persona" ? "who you are and how you talk" : "3 to 5 fresh facts"})`);
        }
        if (args[k] === true) out[k] = true;
      }
      if (out.persona && out.facts) throw invalid("give persona or facts, not both");
      if (args.end !== undefined && typeof args.end !== "boolean") throw invalid("end must be a boolean");
      if (args.end === true) out.end = true;
      if (!Object.keys(out).length) throw invalid("give context, or end: true");
      return { name: n, args: out, job: false };
    }
    case "act":
      only(args, ["action"]);
      if (typeof args.action !== "string" || !GESTURE_RE.test(args.action)) {
        throw invalid(`action must be a gesture name: one of the creature's (device.creature.gestures), e.g. ${ACTIONS.join(", ")}`);
      }
      return { name: n, args: { action: args.action }, job: false };
  }
}

export interface TalkArgs {
  /** What is spoken and shown: the agent's text with its [cues] stripped. */
  say?: string;
  /** The say's [cues] (relay-only; sent to firmware with say_marks). */
  marks?: SayMarks;
  voice?: string;
  listen: boolean;
  /** undefined = the device's setting; 0 = unlimited. */
  listen_ms?: number;
  /** Device VAD knobs (end_silence_ms, no_speech_ms, vad), forwarded to the device's listen. */
  vad: JsonObject;
  /** The listening ring's colour, resolved "#rrggbb"; absent = the device's default. */
  ring_color?: string;
  /** The device's turn acknowledgement for this listen (ACK_CAP); absent = the human's setting. */
  ack?: AckArg;
  /** The human turn (an `utterance`'s turn_id) this answers (core/turns.ts). */
  in_reply_to?: string;
  /** The agent's own latency report (clientTiming): timings.client and the turn's trace. */
  client_timing?: JsonObject;
  /** With the voice front on: keep the wording of `say` as close as it can (it still decides whether and when). */
  exact?: boolean;
}

/** molty_talk / POST /v1/devices/<id>/talk: `{say?, listen? (default true), listen_ms? (0 = unlimited, 1..30000; default the device's setting, 30000 out of the box), voice?, end_silence_ms?, no_speech_ms?, vad?, ring_color?, ack?, in_reply_to?}`. */
export function validateTalk(raw: unknown): TalkArgs {
  const args = raw === undefined || raw === null ? {} : raw;
  if (!isRecord(args)) throw invalid("args must be an object");
  only(args, ["say", "listen", "listen_ms", "voice", "ring_color", "ack", "in_reply_to", "client_timing", "exact", ...VAD_KEYS]);
  if (args.listen !== undefined && typeof args.listen !== "boolean") throw invalid("listen must be a boolean");
  const out: TalkArgs = { listen: args.listen !== false, vad: vadArgs(args) };
  // `[working]`: off to work after this line, nobody to listen for (unless listen is asked for)
  if (args.listen === undefined && typeof args.say === "string" && parseCues(args.say).marks.some((m) => m[1] === WORKING_MOOD)) out.listen = false;
  const lm = listenMs(args.listen_ms, "listen_ms");
  if (lm !== undefined) out.listen_ms = lm;
  if (args.say !== undefined) {
    const said = sayText(args.say, "say");
    out.say = said.text;
    if (said.marks.length) out.marks = said.marks;
  }
  if (args.voice !== undefined) out.voice = voiceId(args.voice);
  if (args.ring_color !== undefined) out.ring_color = resolveColor(args.ring_color, "ring_color");
  const ack = ackArg(args.ack);
  if (ack !== undefined) out.ack = ack;
  const reply = turnId(args.in_reply_to);
  if (reply !== undefined) out.in_reply_to = reply;
  const ct = clientTiming(args.client_timing);
  if (ct) out.client_timing = ct;
  if (exactArg(args.exact)) out.exact = true;
  if (out.say === undefined && !out.listen) throw invalid("give say, or listen (or both)");
  return out;
}

// ---- ask: a question with tap answers ----

/**
 * Longest answer label (trimmed, printable ASCII) by how many answers there are: 14 for 1..2, 10 for 3, 8 for 4..5.
 * The firmware fits these on its screen (device/firmware/main/fluid/fluid_ask.h fluid_ask_label_max); change them with it.
 */
export function askLabelMax(n: number): number {
  return n <= 2 ? 14 : n === 3 ? 10 : 8;
}
/** The longest any label may be (one or two answers). */
export const MAX_ASK_LABEL = 14;
/** Firmware without ASK_TITLE_CAP takes at most this many characters a label, and no title. */
export const LEGACY_ASK_LABEL = 8;
/** The question shown over the answers (optional): at most this many printable ASCII characters. */
export const MAX_ASK_TITLE = 40;
/** The device cap that says its firmware shows a title and labels up to askLabelMax. */
export const ASK_TITLE_CAP = "ask_title";
export const MAX_ASK_OPTIONS = 5;
export const MAX_ASK_TIMEOUT_S = 120;
export const DEFAULT_ASK_TIMEOUT_S = 30;

export interface AskArgs {
  /** The spoken question, [cues] stripped (as TalkArgs). */
  say?: string;
  marks?: SayMarks;
  voice?: string;
  /** Trimmed labels, in order: index i is options[i]. */
  options: string[];
  /** The question shown over the answers, trimmed. */
  title?: string;
  /** options[i]'s colour, resolved "#rrggbb" (given, else DEFAULT_ASK_COLORS[i]). */
  colors: string[];
  timeout_s: number;
  /** The agent's own latency report (clientTiming): timings.client. */
  client_timing?: JsonObject;
  /** The human turn this question answers: traced (a `reply` entry), never claimed. */
  in_reply_to?: string;
}

/**
 * molty_ask / POST /v1/devices/<id>/ask: `{say?, title?, options: 1..5 items, timeout_s? 1..120 (default 30), voice?}`.
 * An item is a label (1..askLabelMax(n) printable ASCII chars: 14 for 1..2 answers, 10 for 3, 8 for 4..5; unique
 * case-insensitive) or `{label, color?}` (color: a COLORS name or #rrggbb); mixed arrays are fine. title: the question
 * shown over the answers, 1..40 printable ASCII chars.
 */
export function validateAsk(raw: unknown): AskArgs {
  const args = raw === undefined || raw === null ? {} : raw;
  if (!isRecord(args)) throw invalid("args must be an object");
  only(args, ["say", "title", "options", "timeout_s", "voice", "client_timing", "in_reply_to"]);
  const opts = args.options;
  if (!Array.isArray(opts) || opts.length < 1 || opts.length > MAX_ASK_OPTIONS) {
    throw invalid(`options must be a list of 1..${MAX_ASK_OPTIONS} short labels, e.g. ["yes","no"]`);
  }
  const seen = new Set<string>();
  const colors: string[] = [];
  const max = askLabelMax(opts.length);
  const options = opts.map((item, i) => {
    let o: unknown = item;
    colors[i] = COLORS[DEFAULT_ASK_COLORS[i]];
    if (isRecord(item)) {
      for (const key of Object.keys(item)) if (key !== "label" && key !== "color") throw invalid(`options[${i}] has unknown field ${key} (use {"label","color"})`);
      o = item.label;
      if (item.color !== undefined) colors[i] = resolveColor(item.color, `options[${i}].color`);
    }
    if (typeof o !== "string") throw invalid(`options[${i}] must be a label string or {"label": "yes", "color": "green"}`);
    const label = o.trim();
    if (label.length < 1 || label.length > max) {
      throw invalid(`options[${i}] "${o}" must be 1..${max} characters after trimming with ${opts.length} answer${opts.length > 1 ? "s" : ""} (14 for 1-2, 10 for 3, 8 for 4-5: it is a tap target on a small round screen): shorten it, e.g. "yes", "no", "later"`);
    }
    if (!/^[\x20-\x7e]+$/.test(label)) throw invalid(`options[${i}] must be printable ASCII (letters, digits, spaces, punctuation; no emoji or accents)`);
    const key = label.toLowerCase();
    if (seen.has(key)) throw invalid(`options must be unique (case-insensitive): "${label}" appears twice`);
    seen.add(key);
    return label;
  });
  const out: AskArgs = { options, colors, timeout_s: int(args.timeout_s, "timeout_s", 1, MAX_ASK_TIMEOUT_S, DEFAULT_ASK_TIMEOUT_S) };
  if (args.title !== undefined) {
    const t = typeof args.title === "string" ? args.title.trim() : "";
    if (t.length < 1 || t.length > MAX_ASK_TITLE || !/^[\x20-\x7e]+$/.test(t)) throw invalid(`title must be 1..${MAX_ASK_TITLE} printable ASCII characters: the question shown over the answers, e.g. "Send the summary to Ana?"`);
    out.title = t;
  }
  if (args.say !== undefined) {
    const said = sayText(args.say, "say");
    out.say = said.text;
    if (said.marks.length) out.marks = said.marks;
  }
  if (args.voice !== undefined) out.voice = voiceId(args.voice);
  const ct = clientTiming(args.client_timing);
  if (ct) out.client_timing = ct;
  const reply = turnId(args.in_reply_to);
  if (reply !== undefined) out.in_reply_to = reply;
  return out;
}

// ---- while the human speaks: draft and react (core/partials.ts) ----

/** A draft's opener: one short spoken line (it plays the instant the human finishes). */
export const MAX_OPENER_BYTES = 200;
export const MAX_OPENER_WORDS = 24;
/** max_new_words an agent may ask for; the relay's calibrated threshold caps it (calibration.ts). */
export const MAX_DRAFT_NEW_WORDS = 20;
/** Drafts one agent may make per human turn (each new one replaces its previous). */
export const MAX_DRAFTS_PER_TURN = 3;

/** Moods a react may show over the listening animation (screen only: the mic is open). */
export const REACT_MOODS = ["happy", "surprised", "thinking"] as const;
/**
 * Actions a react may play (screen only): the device's gestures (`act`, cap `act`) — the same ONE
 * list as molty_act and the inline [nod] cues, so a new gesture is a new react action too.
 */
export const REACT_ACTIONS: readonly string[] = ACTIONS.filter((a) => !WORKING_ACTIONS.includes(a));
/** At most one react per this long, per Molty (one screen). */
export const REACT_MIN_GAP_MS = 3000;
/** How long a react shows before Molty returns to listening. */
export const REACT_SHOW_MS = 1500;
/** Firmware cap: `mood {react:true}` shows a mood without counting as an explicit one (the turn's ack stays). */
export const REACT_CAP = "react";

export interface DraftArgs {
  turn_id: string;
  based_on: number;
  opener: string;
  max_new_words?: number;
  voice?: string;
}

/** molty_draft / POST /v1/devices/<id>/draft: `{turn_id, based_on, opener, max_new_words?, voice?}`. */
export function validateDraft(raw: unknown): DraftArgs {
  if (!isRecord(raw)) throw invalid("args must be an object");
  only(raw, ["turn_id", "based_on", "opener", "max_new_words", "voice"]);
  if (typeof raw.turn_id !== "string" || !TURN_ID_RE.test(raw.turn_id)) throw invalid("turn_id must be the open turn's id (from utterance.partial)");
  const out: DraftArgs = {
    turn_id: raw.turn_id,
    based_on: int(raw.based_on, "based_on", 1, 1000),
    opener: text(raw.opener, "opener", MAX_OPENER_BYTES).trim(),
  };
  if (!out.opener) throw invalid("opener must be a nonempty string");
  if (out.opener.split(/\s+/).length > MAX_OPENER_WORDS) throw invalid(`opener must be at most ${MAX_OPENER_WORDS} words: a short line that fits whatever comes next`);
  if (raw.max_new_words !== undefined) out.max_new_words = int(raw.max_new_words, "max_new_words", 0, MAX_DRAFT_NEW_WORDS);
  if (raw.voice !== undefined) out.voice = voiceId(raw.voice);
  return out;
}

export type ReactArgs = { turn_id: string; mood: (typeof REACT_MOODS)[number]; action?: undefined } | { turn_id: string; action: string; mood?: undefined };

/** molty_react / POST /v1/devices/<id>/react: `{turn_id, mood}` or `{turn_id, action}`. */
export function validateReact(raw: unknown): ReactArgs {
  if (!isRecord(raw)) throw invalid("args must be an object");
  only(raw, ["turn_id", "mood", "action"]);
  if (typeof raw.turn_id !== "string" || !TURN_ID_RE.test(raw.turn_id)) throw invalid("turn_id must be the open turn's id (from utterance.partial)");
  if ((raw.mood === undefined) === (raw.action === undefined)) throw invalid("give mood or action (one of them)");
  if (raw.mood !== undefined) {
    if (typeof raw.mood !== "string" || !(REACT_MOODS as readonly string[]).includes(raw.mood)) throw invalid(`mood must be one of ${REACT_MOODS.join(", ")}`);
    return { turn_id: raw.turn_id, mood: raw.mood as (typeof REACT_MOODS)[number] };
  }
  if (typeof raw.action !== "string" || !REACT_ACTIONS.includes(raw.action)) {
    throw invalid(`action must be one of ${REACT_ACTIONS.join(", ")}`);
  }
  return { turn_id: raw.turn_id, action: raw.action };
}
