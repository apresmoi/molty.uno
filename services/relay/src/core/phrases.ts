// SPDX-License-Identifier: MIT
// Quick phrases and the auto filler (docs/AGENT-LINK.md "Quick phrases"): short lines Molty
// can say with zero TTS latency because their audio is synthesised ahead of time, in the
// device's current voice, and cached here.
//
// - Every agent may register its own phrases (`phr:<token hash>`: at most PHRASE_LIMITS.perAgent,
//   each at most textMax characters); built-ins (BUILTIN_PHRASES) are there for everyone. An
//   agent's own id shadows a built-in of the same id for that agent only.
// - `quick {id}` plays one from the cache: no TTS call when it is cached (a miss synthesises it
//   once, then it is cached too).
// - The auto filler: when a human turn ends with speech and no agent has done anything for the
//   owner's delay (device setting `filler_ms`, FILLER_DEFAULT_MS when the firmware does not
//   report it), the relay plays a random FILLER_POOL phrase itself (never the same one twice
//   in a row).
//
// Cache (`pcm:<key>` meta + `pcmd:<key>:<i>` PCM chunks, key = hash of voice + text): bounded
// to PHRASE_LIMITS.cacheEntries entries, least recently used evicted first. Host-neutral.

import { CommandError, PHRASE_ID_RE, type SayMarks } from "./commands.js";
import { isRecord } from "./protocol.js";
import { parseCues, speechWords } from "./speech.js";
import type { KV } from "./store.js";
import { sha256Hex } from "./crypto.js";
import { UTTERANCE_CATEGORIES, type UtteranceCategory, keywordCategory } from "./reflex.js";

export const PHRASE_LIMITS = {
  /** Phrases one agent may register. */
  perAgent: 20,
  /** Characters of a phrase's text (inline cues included). */
  textMax: 60,
  /** Cached phrase audios on one device (every voice, every agent, built-ins). */
  cacheEntries: 64,
  /** PCM bytes per stored chunk (a storage value stays far below the 128 KiB / 2 MB limits). */
  chunkBytes: 64 * 1024,
  /** Longest phrase audio cached (16 kHz samples): 8 s. Longer audio is refused. */
  maxSamples: 8 * 16000,
  /** A `phrases set` waits at most this long for the new phrases' audio, then answers (the rest keeps going). */
  warmBudgetMs: 8000,
  /** Phrase syntheses running at once while warming. */
  warmConcurrency: 2,
};

export { PHRASE_ID_RE };

/**
 * What kind of human turn a filler fits: a question ("Good question."), a request ("On it."),
 * something hard / complex ("Let me think."), or `any` turn ("Hmm…"). The auto filler picks by
 * the category of the human's utterance (UtteranceClassifier).
 */
export const FILLER_FOR = ["question", "request", "hard", "any"] as const;
export type FillerFor = (typeof FILLER_FOR)[number];
/** A human utterance's category (core/reflex.ts, the same one the reflex engines answer); `other` = none of the three. */
export type { UtteranceCategory };

export interface Phrase {
  id: string;
  /** What is spoken and shown (inline cues stripped). */
  text: string;
  /** The text's inline [cues] (commands.ts SayMarks). */
  marks?: SayMarks;
  /** An agent's phrase the auto filler may use for it (core/phrases.ts pickFiller). */
  filler?: boolean;
  /** The turns it fits (default any). */
  for?: FillerFor;
}

/** Built-in phrases every agent can play without registering anything (unless it turned them off). */
export const BUILTIN_PHRASES: readonly Phrase[] = [
  { id: "hmm", text: "Hmm…", for: "any" },
  { id: "let_me_think", text: "Let me think.", for: "hard" },
  { id: "one_sec", text: "One sec.", for: "any" },
  { id: "let_me_see", text: "Let me see.", for: "any" },
  { id: "good_question", text: "Good question.", for: "question" },
  { id: "on_it", text: "On it.", for: "request" },
  { id: "checking", text: "Checking.", for: "request" },
];

/** The built-in filler pool: every built-in, each used only for the turns it fits (`for`). */
export const FILLER_POOL: readonly string[] = BUILTIN_PHRASES.map((p) => p.id);

/**
 * The auto filler delay (owner setting, device `filler_ms`): 0 = off, else FILLER_MIN_MS..FILLER_MAX_MS,
 * counted from the listen's end (the end frame / the box's turn end). Nothing after that adds to
 * it: the classifier runs in parallel (a verdict not in by then → the keywords), the pool and its
 * audio are ready before the moment. Below the STT's own latency it fires as soon as the words are known.
 */
export const FILLER_DEFAULT_MS = 2500;
export const FILLER_MIN_MS = 400;
export const FILLER_MAX_MS = 5000;
/** An agent's quick within this long of a relay filler (and with nothing of its own since) is skipped: "filled". */
export const FILL_SKIP_MS = 30000;

/** The device's reported filler setting (listen ack `filler_ms`), else the default. */
export function fillerMs(reported: unknown): number {
  if (reported === 0) return 0;
  if (typeof reported === "number" && Number.isInteger(reported) && reported >= FILLER_MIN_MS && reported <= FILLER_MAX_MS) return reported;
  return FILLER_DEFAULT_MS;
}

/**
 * A filler for a turn of `category` from `pool`: the phrases made for that category, else the
 * `any` ones (no `for` = any), else nothing (a "Good question." never follows a statement). Never
 * `last` when there is another (no immediate repeats). `rnd` in [0, 1).
 */
export function pickFiller(pool: readonly Phrase[], category: UtteranceCategory, last: string | undefined, rnd: number = Math.random()): Phrase | undefined {
  const fits = (want: FillerFor) => pool.filter((p) => (p.for ?? "any") === want);
  let c = category === "other" ? [] : fits(category);
  if (!c.length) c = fits("any");
  if (!c.length) return undefined;
  if (c.length > 1) c = c.filter((p) => p.id !== last);
  return c[Math.min(c.length - 1, Math.floor(rnd * c.length))];
}

/**
 * Classifies a human utterance for the auto filler. By default the room uses Molty's reflex
 * (DeviceRoom.fillerCategory: the reflex decision already made on those words, else one call of the
 * owner's engine — jev / openai / keywords); RoomDeps.classifier overrides it (tests, other hosts).
 * Either gets at most CLASSIFY_BUDGET_MS, and never more than the filler's own delay (it runs in
 * parallel with it); a miss, a throw, a timeout or a verdict not in by the filler's moment → the keywords.
 */
export interface UtteranceClassifier {
  readonly name: string;
  classify(text: string): UtteranceCategory | Promise<UtteranceCategory>;
}
export const CLASSIFY_BUDGET_MS = 400;
export { UTTERANCE_CATEGORIES };

/** The keyword classifier: core/reflex.ts keywordCategory (Molty's reflex keywords engine) — one implementation. */
export const keywordClassifier: UtteranceClassifier = {
  name: "keywords",
  classify: (text: string) => keywordCategory(text),
};

export function builtin(id: string): Phrase | undefined {
  return BUILTIN_PHRASES.find((p) => p.id === id);
}

const invalid = (message: string) => new CommandError("invalid_args", message);

function phraseId(v: unknown, field: string): string {
  if (typeof v !== "string" || !PHRASE_ID_RE.test(v)) throw invalid(`${field} must be a phrase id: 1..24 of a-z 0-9 _ - (starting with a letter or digit), e.g. "hmm"`);
  return v;
}

/** One phrase as registered: `{id, text, filler?, for?}`, text 1..textMax characters with something to speak. */
export function validatePhrase(v: unknown, i: number): Phrase {
  if (!isRecord(v)) throw invalid(`phrases[${i}] must be {"id": "hmm", "text": "Hmm…"}`);
  for (const k of Object.keys(v)) if (!["id", "text", "filler", "for"].includes(k)) throw invalid(`phrases[${i}] has unknown field ${k} (use {"id","text","filler"?,"for"?})`);
  if (v.filler !== undefined && typeof v.filler !== "boolean") throw invalid(`phrases[${i}].filler must be a boolean`);
  if (v.for !== undefined && !(FILLER_FOR as readonly unknown[]).includes(v.for)) throw invalid(`phrases[${i}].for must be one of ${FILLER_FOR.join(", ")}`);
  const id = phraseId(v.id, `phrases[${i}].id`);
  if (typeof v.text !== "string" || v.text.trim() === "") throw invalid(`phrases[${i}].text must be a nonempty string`);
  if ([...v.text].length > PHRASE_LIMITS.textMax) throw invalid(`phrases[${i}].text is over ${PHRASE_LIMITS.textMax} characters: a quick phrase is a few words ("Hmm, let me think.")`);
  if (/[\u0000-\u001f\u007f]/.test(v.text)) throw invalid(`phrases[${i}].text must be one line of printable text`);
  const parsed = parseCues(v.text);
  if (speechWords(parsed.text).length === 0) throw invalid(`phrases[${i}].text has nothing to speak`);
  return {
    id,
    text: parsed.text,
    ...(parsed.marks.length ? { marks: parsed.marks } : {}),
    ...(v.filler === true ? { filler: true } : {}),
    ...(v.for !== undefined && v.for !== "any" ? { for: v.for as FillerFor } : {}),
  };
}

export type PhrasesRequest = { action: "list" } | { action: "set"; phrases: Phrase[]; builtins?: boolean } | { action: "delete"; ids: string[] };

/** `{action: list | set | delete, phrases?: [{id, text}], id?, ids?}` (MCP molty_phrases / REST). */
export function validatePhrasesRequest(raw: unknown): PhrasesRequest {
  const args = raw === undefined || raw === null ? {} : raw;
  if (!isRecord(args)) throw invalid("args must be an object");
  for (const k of Object.keys(args)) if (!["action", "phrases", "id", "ids", "builtins"].includes(k)) throw invalid(`unknown argument ${k}`);
  const action = args.action ?? (args.phrases !== undefined || args.builtins !== undefined ? "set" : "list");
  if (action === "list") return { action };
  if (action === "set") {
    if (args.builtins !== undefined && typeof args.builtins !== "boolean") throw invalid("builtins must be a boolean (false: you never use or see the built-in phrases)");
    const b = args.builtins === undefined ? {} : { builtins: args.builtins as boolean };
    if (args.phrases === undefined && args.builtins !== undefined) return { action, phrases: [], ...b };
    if (!Array.isArray(args.phrases) || args.phrases.length < 1 || args.phrases.length > PHRASE_LIMITS.perAgent) {
      throw invalid(`phrases must be a list of 1..${PHRASE_LIMITS.perAgent} {"id","text"} items`);
    }
    const out = args.phrases.map((p, i) => validatePhrase(p, i));
    const seen = new Set<string>();
    for (const p of out) {
      if (seen.has(p.id)) throw invalid(`phrase id ${p.id} appears twice`);
      seen.add(p.id);
    }
    return { action, phrases: out, ...b };
  }
  if (action === "delete") {
    const list = args.ids !== undefined ? args.ids : args.id !== undefined ? [args.id] : undefined;
    if (!Array.isArray(list) || list.length < 1 || list.length > PHRASE_LIMITS.perAgent) throw invalid('delete takes id (one) or ids (a list), or ids: ["*"] for all of yours');
    if (list.length === 1 && list[0] === "*") return { action, ids: ["*"] };
    return { action, ids: list.map((x, i) => phraseId(x, `ids[${i}]`)) };
  }
  throw invalid("action must be list, set or delete");
}

/** An agent's own phrases (KV `phr:<token hash>`). */
export interface AgentPhrases {
  phrases: Phrase[];
  at: number;
  /** false: this agent never uses or sees the built-ins (and Molty's filler for it uses only its own). */
  builtins?: boolean;
}

/** `set`: upserted into `cur` (an existing id is replaced in place); undefined when over the cap. */
export function mergePhrases(cur: Phrase[], add: Phrase[]): Phrase[] | undefined {
  const out = cur.map((p) => ({ ...p }));
  for (const p of add) {
    const i = out.findIndex((q) => q.id === p.id);
    if (i >= 0) out[i] = p;
    else out.push(p);
  }
  return out.length > PHRASE_LIMITS.perAgent ? undefined : out;
}

// ---- the audio cache ----

/** A phrase's audio, ready to stream: PCM16 mono 16 kHz, word cues [word, sample], who spoke it. */
export interface PhraseAudio {
  text: string;
  pcm: Int16Array;
  cues: Array<[number, number]>;
  /** The provider name that synthesised it (timings.voice). */
  voice: string;
}

interface CacheMeta {
  text: string;
  vkey: string;
  voice: string;
  chunks: number;
  samples: number;
  cues: Array<[number, number]>;
  at: number;
  used: number;
}

const META = "pcm:";
const DATA = "pcmd:";

/** The cache key of a voice (provider + its voice id) and a text. */
export async function cacheKey(vkey: string, text: string): Promise<string> {
  return (await sha256Hex(`${vkey}\n${text}`)).slice(0, 32);
}

export class PhraseCache {
  constructor(
    private readonly kv: KV,
    private readonly now: () => number,
  ) {}

  /** Is it cached (meta only: no PCM read)? */
  async has(vkey: string, text: string): Promise<boolean> {
    return !!(await this.kv.get<CacheMeta>(META + (await cacheKey(vkey, text))));
  }

  /** The cached audio, or undefined (missing, or a chunk was lost: the entry is dropped). */
  async get(vkey: string, text: string): Promise<PhraseAudio | undefined> {
    const key = await cacheKey(vkey, text);
    const meta = await this.kv.get<CacheMeta>(META + key);
    if (!meta || meta.text !== text || meta.vkey !== vkey) return undefined;
    const pcm = new Int16Array(meta.samples);
    let o = 0;
    for (let i = 0; i < meta.chunks; i++) {
      const c = await this.kv.get<Uint8Array>(`${DATA}${key}:${i}`);
      if (!(c instanceof Uint8Array) || o * 2 + c.byteLength > meta.samples * 2) {
        await this.drop(key, meta.chunks);
        return undefined;
      }
      pcm.set(new Int16Array(c.buffer.slice(c.byteOffset, c.byteOffset + c.byteLength)), o);
      o += c.byteLength / 2;
    }
    if (o !== meta.samples) {
      await this.drop(key, meta.chunks);
      return undefined;
    }
    // least recently used: the play counts as a use (a small meta write)
    await this.kv.put<CacheMeta>(META + key, { ...meta, used: this.now() });
    return { text, pcm, cues: meta.cues, voice: meta.voice };
  }

  /** Stores it (evicting the least recently used beyond the cap). False when it is too long to cache. */
  async put(vkey: string, a: PhraseAudio): Promise<boolean> {
    if (a.pcm.length === 0 || a.pcm.length > PHRASE_LIMITS.maxSamples) return false;
    const key = await cacheKey(vkey, a.text);
    const metas = await this.kv.list<CacheMeta>(META);
    const others = [...metas].filter(([k]) => k !== META + key).sort((x, y) => x[1].used - y[1].used);
    for (let i = 0; i <= others.length - PHRASE_LIMITS.cacheEntries; i++) await this.drop(others[i][0].slice(META.length), others[i][1].chunks);
    const bytes = new Uint8Array(a.pcm.buffer, a.pcm.byteOffset, a.pcm.byteLength);
    const chunks = Math.ceil(bytes.byteLength / PHRASE_LIMITS.chunkBytes);
    for (let i = 0; i < chunks; i++) await this.kv.put(`${DATA}${key}:${i}`, bytes.slice(i * PHRASE_LIMITS.chunkBytes, (i + 1) * PHRASE_LIMITS.chunkBytes));
    const now = this.now();
    await this.kv.put<CacheMeta>(META + key, { text: a.text, vkey, voice: a.voice, chunks, samples: a.pcm.length, cues: a.cues, at: now, used: now });
    return true;
  }

  /** The texts cached for any voice (what a voice change re-synthesises). */
  async texts(): Promise<string[]> {
    return [...new Set([...(await this.kv.list<CacheMeta>(META)).values()].map((m) => m.text))];
  }

  async size(): Promise<number> {
    return (await this.kv.list<CacheMeta>(META)).size;
  }

  private async drop(key: string, chunks: number) {
    await this.kv.delete(META + key);
    for (let i = 0; i < chunks; i++) await this.kv.delete(`${DATA}${key}:${i}`);
  }
}

/** Every key prefix of the phrase store (onForget wipes them). */
export const PHRASE_PREFIXES = ["phr:", META, DATA];
