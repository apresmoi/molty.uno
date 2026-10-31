// SPDX-License-Identifier: MIT
// Molty's reflex (docs/AGENT-LINK.md "Reflex"): while the human speaks, Molty reacts on its own to
// WHAT they say — with any agent, also one that never watches partials. Each reflex chunk of the
// box's text so far (from REFLEX.firstWords words, then every few new words or a little speech
// time: the owner's liveliness, REFLEX_LIVELINESS) is classified into one reaction, gated by the
// liveliness (a confident pick shows in full, a plausible one as its softer gesture) and shown
// screen-only through the react path (room.ts reflexFire): agents' own reacts win, owner screens
// block it, one per the liveliness's gap, visual only.
//
//   box stt.partial ─▶ reflexDue ─▶ chunk n ─▶ engine (owner setting `reflex`) ─▶ reflexGate ─▶ fire
//                                     │  local:    the box's own classifier (stt.reflex {n, …}: no extra hop)
//                                     │  jev:      TypeSafe System One, one Choice  (TYPESAFE_API_KEY)
//                                     │  openai:   OpenAI Decisions, one choice      (OPENAI_API_KEY)
//                                     │  keywords: a relay-side heuristic, no key
//                                     └─ error / REFLEX.timeoutMs ─▶ keywords (fallback reason noted)
//   compare: every available engine runs on each chunk (noted in the turn trace), only the configured one fires.
//
// Host-neutral, no TypeScript-only runtime syntax (tools/reflex-bench.ts runs it under Node's type
// stripping). Errors are codes only: a key or a response body never reaches a log or a trace.

import { sentenceEnd, words } from "./words.js";

export const REFLEX_MODES = ["off", "auto", "local", "jev", "openai", "keywords"] as const;
export type ReflexMode = (typeof REFLEX_MODES)[number];
export const REFLEX_ENGINES = ["local", "jev", "openai", "keywords"] as const;
export type ReflexEngine = (typeof REFLEX_ENGINES)[number];
/** What Molty may show; `none` shows nothing. The order is the trace's probability order. */
export const REFLEX_PICKS = ["surprised", "happy", "sad", "laugh", "curious", "none"] as const;
export type ReflexPick = (typeof REFLEX_PICKS)[number];
/**
 * What kind of utterance the human is making (for context-picked fillers, feat/quick-phrases): a
 * question, a request to do something, something hard that needs real thought, or other talk.
 * Every engine answers it in the same call as the reaction.
 */
export const UTTERANCE_CATEGORIES = ["question", "request", "hard", "other"] as const;
export type UtteranceCategory = (typeof UTTERANCE_CATEGORIES)[number];

export const REFLEX = {
  /** The first chunk once the text has this many words (the box classifies every word count from here). */
  firstWords: 3,
  /** Chunks classified per listen (a ramble stays bounded: ~40 s of speech at the lively cadence). */
  maxPerListen: 40,
  /** Remote calls of the configured engine in flight at once; a chunk due beyond that waits for the next word. */
  maxInFlight: 2,
  /** One engine call; then the keywords answer instead (never blocks the listen). */
  timeoutMs: 600,
  /** The text an engine sees: the last this many characters of the text so far. */
  maxChars: 400,
  /** A remote engine refused for good (bad key, no access): not called again for this long. */
  benchMs: 10 * 60 * 1000,
  /** Reflex decisions kept in one listen's trace. */
  traceEntries: 60,
};

/** How lively Molty is while the human talks (owner setting `liveliness`, default normal). */
export const LIVELINESS = ["calm", "normal", "lively"] as const;
export type Liveliness = (typeof LIVELINESS)[number];
export const DEFAULT_LIVELINESS: Liveliness = "normal";
export interface ReflexProfile {
  /** A chunk once this many new words arrived since the last one, or a sentence end, or everyMs of speech with a new word. */
  everyWords: number;
  everyMs: number;
  /** At most one reflex per this long, and none this long after an agent's react. */
  gapMs: number;
  /** Confidence an engine's pick needs to be shown in full. */
  gate: Record<ReflexEngine, number>;
  /** Below the gate: still shown as its softer gesture (REFLEX_SOFT) when the engine's `none` is under `none` and the best reaction reaches `min`. */
  mild?: { none: number; min: number };
}
/**
 * Tuned on tools/sim's labelled partials (scenarios/reflex.yaml) and whole turns
 * (scenarios/reflex-turns.yaml) with Jev's probabilities: see tools/sim/README.md "Reflex over
 * whole turns". calm is the old behaviour's caution with the earlier first chunk.
 */
export const REFLEX_LIVELINESS: Record<Liveliness, ReflexProfile> = {
  calm: { everyWords: 6, everyMs: 2500, gapMs: 3000, gate: { local: 0.35, jev: 0.5, openai: 0.5, keywords: 0.5 } },
  normal: { everyWords: 4, everyMs: 1500, gapMs: 2000, gate: { local: 0.3, jev: 0.4, openai: 0.4, keywords: 0.5 }, mild: { none: 0.6, min: 0.3 } },
  lively: { everyWords: 3, everyMs: 1200, gapMs: 1500, gate: { local: 0.25, jev: 0.35, openai: 0.35, keywords: 0.5 }, mild: { none: 0.75, min: 0.2 } },
};
export const isLiveliness = (v: unknown): v is Liveliness => typeof v === "string" && (LIVELINESS as readonly string[]).includes(v);

/** How each reaction is shown: a react mood (firmware cap `react`) or a one-shot gesture (cap `act`). */
export const REFLEX_SHOW: Record<Exclude<ReflexPick, "none">, { mood: "happy" | "surprised" | "thinking" } | { action: string }> = {
  surprised: { mood: "surprised" },
  happy: { mood: "happy" },
  sad: { action: "nod" },
  laugh: { action: "bounce" },
  curious: { action: "peek" },
};
/** A mild pick's softer gesture (cap `act`): a plausible reaction, shown small. curious alternates its glance. */
export const REFLEX_SOFT: Record<Exclude<ReflexPick, "none">, string> = {
  surprised: "look_up",
  happy: "wiggle",
  sad: "nod",
  laugh: "wobble",
  curious: "glance_left",
};

/** The question every model engine answers (Jev criteria, OpenAI choice descriptions). */
export const REFLEX_QUESTION =
  "Molty is a small creature on the desk, listening to its human, who is still talking: this is only what they said so far. Which visible reaction should Molty show right now?";
export const REFLEX_CRITERIA: Record<ReflexPick, string> = {
  surprised: "astonishing or unexpected news, disbelief, shock (no way, guess what, you won't believe)",
  happy: "good news, joy, excitement, pride, gratitude, love, celebrating",
  sad: "sad or bad news: loss, grief, illness, disappointment, loneliness, a bad day",
  laugh: "a joke, a pun, something funny or silly, or the human laughing",
  curious: "a question, or the human wondering about something",
  none: "ordinary talk: instructions, facts, plans, small talk, or unclear / mixed feelings — no visible reaction",
};

export const CATEGORY_QUESTION = "What kind of thing is the human saying to Molty so far?";
export const CATEGORY_CRITERIA: Record<UtteranceCategory, string> = {
  question: "a simple question with a quick answer (a fact, the time, the weather, yes or no)",
  request: "asks Molty to do something: set, remind, play, send, turn on, add, call, show, stop",
  hard: "needs real thought: explain why or how, advice, a decision, a plan, a comparison, an opinion, or something emotionally heavy",
  other: "statements, stories, feelings, small talk, thanks — nothing to answer or do yet",
};

/** One engine's answer for one chunk. */
export interface ReflexVerdict {
  engine: ReflexEngine;
  /** The engine's choice; after reflexGate (the session's), what Molty shows (none: nothing). */
  pick: ReflexPick;
  /** After reflexGate: the engine's own choice when the gate changed it. */
  raw?: ReflexPick;
  /** After reflexGate: a mild pick, shown as its softer gesture. */
  soft?: boolean;
  conf?: number;
  p?: Partial<Record<ReflexPick, number>>;
  ms: number;
  chars: number;
  tokens?: number;
  model?: string;
  /** The utterance category (no gate: the top choice), its confidence and probabilities. */
  cat?: UtteranceCategory;
  catConf?: number;
  catP?: Partial<Record<UtteranceCategory, number>>;
  /** Why it has no answer: timeout, no_key, unavailable, network, http_<status>, bad_response, refused. */
  error?: string;
}

/** The engine's last `maxChars` characters of the text so far, cut at a word. */
export function reflexInput(text: string): string {
  const t = text.trim();
  if (t.length <= REFLEX.maxChars) return t;
  const cut = t.slice(-REFLEX.maxChars);
  const sp = cut.indexOf(" ");
  return sp >= 0 ? cut.slice(sp + 1) : cut;
}

/**
 * Is a reflex chunk due for `text` (the box's text so far), the last chunk having had `prevN` words
 * `sinceMs` ago? The first from REFLEX.firstWords words, then with a new word once `everyWords` new
 * words came, the text ends a sentence, or `everyMs` passed. Returns the chunk's word count `n` (its
 * id: the box's stt.reflex names it), or undefined. The box classifies every word count from
 * REFLEX.firstWords (molty_voice/reflex.py reflex_due), so any n the relay picks has its verdict.
 */
export function reflexDue(prevN: number, text: string, sinceMs: number, p: ReflexProfile = REFLEX_LIVELINESS[DEFAULT_LIVELINESS]): number | undefined {
  const n = words(text).length;
  if (n < REFLEX.firstWords) return undefined;
  const added = n - prevN;
  if (added < 1) return undefined;
  if (prevN === 0 || added >= p.everyWords || sentenceEnd(text) || sinceMs >= p.everyMs) return n;
  return undefined;
}

const round2 = (x: number) => Math.round(x * 100) / 100;

/**
 * What Molty shows for an engine's verdict at a liveliness: its choice in full at or above the
 * engine's gate; below it, the most likely reaction as its softer gesture when the profile allows
 * mild picks, `none` is unsure (under mild.none) and that reaction is plausible (mild.min); else
 * nothing. A request is never curiosity ("can you set a timer?": the keywords' and the box's rule too).
 */
export function reflexGate(v: ReflexVerdict, p: ReflexProfile): ReflexVerdict {
  const { soft: _s, raw: _r, ...base } = v;
  const choice = v.pick;
  const conf = v.conf ?? 0;
  let out: ReflexVerdict;
  if (choice !== "none" && conf >= p.gate[v.engine]) out = { ...base, pick: choice };
  else {
    let alt: Exclude<ReflexPick, "none"> | undefined;
    for (const k of REFLEX_PICKS) if (k !== "none" && (v.p?.[k] ?? -1) > (alt ? (v.p?.[alt] ?? -1) : -1)) alt = k;
    const pn = v.p?.none;
    if (p.mild && alt && pn !== undefined && pn < p.mild.none && (v.p?.[alt] ?? 0) >= p.mild.min) out = { ...base, pick: alt, soft: true, ...(choice !== alt ? { raw: choice } : {}) };
    else out = choice === "none" ? { ...base, pick: "none" } : { ...base, pick: "none", raw: choice };
  }
  if (out.pick === "curious" && v.cat === "request") return { ...base, pick: "none", raw: choice };
  return out;
}

// ---------------------------------------------------------------- keywords

const KW: Array<[Exclude<ReflexPick, "none">, RegExp]> = [
  ["laugh", /\b(ha(ha)+|he(he)+|lol|lmao|rofl|hilarious|so funny|that's funny|a joke|knock knock|walks into a bar|pun)\b/i],
  [
    "sad",
    /\b(died|dead|passed away|funeral|grief|sad|sadly|miss (him|her|them)|lost (my|our|her|his)|cried|crying|depressed|lonely|heartbroken|broke up|cancer|in hospital|in the hospital|sick|terrible (day|news|week)|awful (day|news|week)|bad news|devastated|unfortunately)\b/i,
  ],
  ["surprised", /\b(oh my god|oh my gosh|omg|wow|whoa|no way|can't believe|cannot believe|you won't believe|guess what|holy|unbelievable|out of nowhere|suddenly|shocked|seriously)\b/i],
  [
    "happy",
    /\b(got the job|got promoted|promotion|great news|good news|awesome|amazing|wonderful|fantastic|so happy|happy|excited|yay|hooray|we won|i won|passed (my|the)|engaged|got married|birthday|love (it|you|this)|finally)\b/i,
  ],
];
const NEGATED = /\b(not|never|no|isn't|wasn't|aren't|don't|didn't|won't)\s+(\w+\s+)?$/i;
const QUESTION = /^(what|why|how|when|where|who|which|can you|could you|do you|did you|are you|is it|is there|will you|would you|should i|have you)\b/i;

const HARD = /\b(explain|why (is|are|do|does|did|would|should)|how (does|do|did|come|would|could)|what('s| is) the difference|difference between|compare|pros and cons|should i|what should|what would you|do you think|your opinion|advice|help me (decide|figure|understand|plan|think)|plan (my|a|the)|i don't know what to do|meaning of)\b/i;
const REQUEST = /^(please |hey molty,? |molty,? )?(set|remind|play|turn|tell me|show|send|call|add|make|find|open|start|stop|cancel|book|order|text|email|write|read|put|schedule|pause|skip|check)\b|\b(can you|could you|would you|will you|please|i need you to|i want you to)\b/i;

/** The utterance category by heuristics (the keywords engine; the local engine's rules too). */
export function keywordCategory(text: string): UtteranceCategory {
  const t = reflexInput(text);
  const last = (t.split(/(?<=[.?!])\s+/).filter(Boolean).at(-1) ?? t).trim();
  if (HARD.test(last)) return "hard";
  if (REQUEST.test(last)) return "request";
  if (/\?["')\]]*\s*$/.test(last) || QUESTION.test(last)) return "question";
  return "other";
}

/** The relay-side heuristic (no key, no network): the fallback of every engine. */
export function keywordReflex(text: string): ReflexVerdict {
  const t = reflexInput(text);
  const base = { engine: "keywords" as const, ms: 0, chars: t.length, cat: keywordCategory(t), catConf: 1 };
  for (const [pick, re] of KW) {
    const m = re.exec(t);
    if (!m) continue;
    // "not happy", "never sad": a negated feeling is no reaction (the laugh / surprise words aren't negated)
    if ((pick === "happy" || pick === "sad") && NEGATED.test(t.slice(0, m.index))) continue;
    return { ...base, pick, conf: 1 };
  }
  const last = t.split(/(?<=[.?!])\s+/).at(-1) ?? t;
  // a question that asks Molty to do something is a request, not curiosity
  if ((/\?["')\]]*\s*$/.test(t) || QUESTION.test(last.trim())) && base.cat !== "request") return { ...base, pick: "curious", conf: 1 };
  return { ...base, pick: "none", conf: 1 };
}

// ---------------------------------------------------------------- model engines: request + parse

export const JEV_URL = "https://api.typesafe.ai/v1/systemone";
export const JEV_MODEL = "jev-latest";
export const OPENAI_URL = "https://api.openai.com/v1/decisions";
export const OPENAI_MODEL = "gpt-6-luna";
/** $ per million input tokens (output is free on both): the bench's cost estimate. */
export const PRICE_PER_MTOK: Record<"jev" | "openai", number> = { jev: 0.042, openai: 0.1 };

/** TypeSafe System One: one Choice over the reactions (docs.typesafe.ai api/system-one, primitives/choice). */
export function jevBody(text: string): Record<string, unknown> {
  return {
    model: JEV_MODEL,
    state: { human_said_so_far: reflexInput(text) },
    questions: {
      reaction: { type: "choice", instructions: REFLEX_QUESTION, criteria: REFLEX_CRITERIA },
      category: { type: "choice", instructions: CATEGORY_QUESTION, criteria: CATEGORY_CRITERIA },
    },
  };
}

/** OpenAI Decisions (developers.openai.com/api/reference/resources/decisions): one choice question. */
export function openaiBody(text: string): Record<string, unknown> {
  return {
    model: OPENAI_MODEL,
    input: `The human, so far: ${JSON.stringify(reflexInput(text))}`,
    questions: [
      { type: "choice", name: "reaction", instructions: REFLEX_QUESTION, choices: REFLEX_PICKS.map((value) => ({ value, description: REFLEX_CRITERIA[value] })) },
      { type: "choice", name: "category", instructions: CATEGORY_QUESTION, choices: UTTERANCE_CATEGORIES.map((value) => ({ value, description: CATEGORY_CRITERIA[value] })) },
    ],
    prompt_cache_key: "molty-reflex",
  };
}

const isCat = (v: unknown): v is UtteranceCategory => typeof v === "string" && (UTTERANCE_CATEGORIES as readonly string[]).includes(v);
const isPick = (v: unknown): v is ReflexPick => typeof v === "string" && (REFLEX_PICKS as readonly string[]).includes(v);
const num01 = (v: unknown): number | undefined => (typeof v === "number" && Number.isFinite(v) && v >= 0 && v <= 1 ? v : undefined);
const isObj = (v: unknown): v is Record<string, unknown> => typeof v === "object" && v !== null && !Array.isArray(v);

/** Probabilities over the picks, from any {pick: p} map (unknown keys and bad numbers dropped). */
function probs(m: Record<string, unknown>): Partial<Record<ReflexPick, number>> {
  const out: Partial<Record<ReflexPick, number>> = {};
  for (const k of REFLEX_PICKS) {
    const v = num01(m[k]);
    if (v !== undefined) out[k] = round2(v);
  }
  return out;
}

type Parsed = Pick<ReflexVerdict, "pick" | "raw" | "conf" | "p" | "tokens" | "model" | "error" | "cat" | "catConf" | "catP">;

function catProbs(m: Record<string, unknown>): Partial<Record<UtteranceCategory, number>> {
  const out: Partial<Record<UtteranceCategory, number>> = {};
  for (const k of UTTERANCE_CATEGORIES) {
    const v = num01(m[k]);
    if (v !== undefined) out[k] = round2(v);
  }
  return out;
}

/** A choice answer's category part (absent when the engine gave none). */
function category(choice: unknown, conf: unknown, pm: Record<string, unknown> | undefined): Pick<ReflexVerdict, "cat" | "catConf" | "catP"> {
  if (!isCat(choice)) return {};
  return { cat: choice, catConf: round2(num01(conf) ?? 0), ...(pm ? { catP: catProbs(pm) } : {}) };
}

export function parseJev(body: unknown): Parsed {
  const a = isObj(body) && isObj(body.answers) ? body.answers.reaction : undefined;
  if (!isObj(a) || !isPick(a.choice)) return { pick: "none", error: "bad_response" };
  const conf = num01(a.confidence) ?? 0;
  const usage = isObj(body) && isObj((body as Record<string, unknown>).usage) ? ((body as Record<string, unknown>).usage as Record<string, unknown>) : {};
  const c = (body as { answers: Record<string, unknown> }).answers.category;
  return {
    pick: a.choice,
    ...(isObj(c) ? category(c.choice, c.confidence, isObj(c.probabilities) ? c.probabilities : undefined) : {}),
    conf: round2(conf),
    p: isObj(a.probabilities) ? probs(a.probabilities) : {},
    ...(typeof usage.input_tokens === "number" ? { tokens: usage.input_tokens } : {}),
    ...(isObj(body) && typeof body.model === "string" ? { model: body.model.slice(0, 40) } : {}),
  };
}

export function parseOpenAI(body: unknown): Parsed {
  const list = isObj(body) && Array.isArray(body.answers) ? body.answers : [];
  const a = list.find((x) => isObj(x) && x.name === "reaction") ?? list[0];
  const c = list.find((x) => isObj(x) && x.name === "category");
  if (isObj(a) && a.type === "refusal") return { pick: "none", error: "refused" };
  if (!isObj(a) || !isPick(a.choice)) return { pick: "none", error: "bad_response" };
  const conf = num01(a.confidence) ?? 0;
  const pm: Record<string, unknown> = {};
  if (Array.isArray(a.probabilities)) for (const e of a.probabilities) if (isObj(e) && typeof e.value === "string") pm[e.value] = e.probability;
  const usage = isObj(body) && isObj((body as Record<string, unknown>).usage) ? ((body as Record<string, unknown>).usage as Record<string, unknown>) : {};
  const cm: Record<string, unknown> = {};
  if (isObj(c) && Array.isArray(c.probabilities)) for (const e of c.probabilities) if (isObj(e) && typeof e.value === "string") cm[e.value] = e.probability;
  return {
    pick: a.choice,
    ...(isObj(c) && c.type === "choice" ? category(c.choice, c.confidence, cm) : {}),
    conf: round2(conf),
    p: probs(pm),
    ...(typeof usage.input_tokens === "number" ? { tokens: usage.input_tokens } : {}),
    ...(isObj(body) && typeof body.model === "string" ? { model: body.model.slice(0, 40) } : {}),
  };
}

export interface ReflexTimer {
  now(): number;
  setTimeout(fn: () => void, ms: number): unknown;
  clearTimeout(handle: unknown): void;
}
export type Fetcher = (req: Request) => Promise<Response>;

/**
 * One call to a model engine, bounded by `timeoutMs` (then aborted: error "timeout"). Never throws;
 * the key goes into the Authorization header only.
 */
export async function remoteReflex(
  engine: "jev" | "openai",
  text: string,
  o: { key: string | undefined; fetch: Fetcher; clock: ReflexTimer; timeoutMs?: number; signal?: AbortSignal },
): Promise<ReflexVerdict> {
  const input = reflexInput(text);
  const t0 = o.clock.now();
  const base = { engine, chars: input.length };
  if (!o.key) return { ...base, pick: "none", ms: 0, error: "no_key" };
  if (o.signal?.aborted) return { ...base, pick: "none", ms: 0, error: "closed" };
  // The deadline races the call itself: it holds even when a fetch implementation ignores the abort
  // (a Request's derived signal is not guaranteed to follow its controller everywhere).
  const ctl = new AbortController();
  let stop!: (why: "timeout" | "closed") => void;
  const stopped = new Promise<"timeout" | "closed">((r) => (stop = r));
  const timer = o.clock.setTimeout(() => stop("timeout"), o.timeoutMs ?? REFLEX.timeoutMs);
  const outer = () => stop("closed");
  o.signal?.addEventListener("abort", outer, { once: true });
  void stopped.then(() => ctl.abort());
  const fail = (error: string): ReflexVerdict => ({ ...base, pick: "none", ms: o.clock.now() - t0, error });
  try {
    const req = new Request(engine === "jev" ? JEV_URL : OPENAI_URL, {
      method: "POST",
      headers: { authorization: `Bearer ${o.key}`, "content-type": "application/json" },
      body: JSON.stringify(engine === "jev" ? jevBody(input) : openaiBody(input)),
      signal: ctl.signal,
    });
    const call = (async () => {
      let res: Response;
      try {
        res = await o.fetch(req);
      } catch {
        return fail("network");
      }
      if (!res.ok) {
        res.body?.cancel().catch(() => {});
        return fail(`http_${res.status}`);
      }
      let body: unknown;
      try {
        body = await res.json();
      } catch {
        return fail("bad_response");
      }
      const parsed = engine === "jev" ? parseJev(body) : parseOpenAI(body);
      return { ...base, ...parsed, ms: o.clock.now() - t0 };
    })();
    return await Promise.race([call, stopped.then(fail)]);
  } finally {
    o.clock.clearTimeout(timer);
    o.signal?.removeEventListener("abort", outer);
  }
}

/** A box `stt.reflex` frame's verdict (the local engine), validated (not gated: reflexGate); undefined when unusable. */
export function parseBoxReflex(f: Record<string, unknown>): (ReflexVerdict & { n: number }) | undefined {
  const n = f.n;
  if (typeof n !== "number" || !Number.isInteger(n) || n < 1 || n > 100000 || !isPick(f.pick)) return undefined;
  const ms = typeof f.ms === "number" && Number.isFinite(f.ms) && f.ms >= 0 && f.ms < 600000 ? Math.round(f.ms) : 0;
  const chars = typeof f.chars === "number" && Number.isInteger(f.chars) && f.chars >= 0 ? Math.min(f.chars, 100000) : 0;
  const conf = num01(f.conf) ?? 0;
  return {
    n,
    engine: "local",
    pick: f.pick,
    ...category(f.cat, f.cat_conf, isObj(f.cat_p) ? f.cat_p : undefined),
    conf: round2(conf),
    p: isObj(f.p) ? probs(f.p) : {},
    ms,
    chars,
    ...(typeof f.tokens === "number" && Number.isInteger(f.tokens) && f.tokens >= 0 ? { tokens: Math.min(f.tokens, 100000) } : {}),
    ...(typeof f.model === "string" && f.model.length <= 80 ? { model: f.model } : {}),
  };
}

// ---------------------------------------------------------------- engine choice

export interface ReflexAvail {
  /** The voice box streams this listen and offers its classifier (hello `reflex: true`). */
  local: boolean;
  jev: boolean;
  openai: boolean;
}

/** The engine a mode fires with, given what is available (auto: local, else jev, else keywords). */
export function resolveEngine(mode: Exclude<ReflexMode, "off">, a: ReflexAvail): { engine: ReflexEngine; fallback?: string } {
  if (mode === "auto") return { engine: a.local ? "local" : a.jev ? "jev" : "keywords" };
  if (mode === "keywords") return { engine: "keywords" };
  if (a[mode]) return { engine: mode };
  return { engine: "keywords", fallback: mode === "local" ? "unavailable" : "no_key" };
}

/** Remote engines refused for good (401/403/404/400) are benched for REFLEX.benchMs. */
export class EngineBench {
  private readonly until = new Map<string, number>();
  ok(engine: string, now: number): boolean {
    return (this.until.get(engine) ?? 0) <= now;
  }
  note(v: ReflexVerdict, now: number): void {
    if (v.error && /^http_(400|401|403|404)$/.test(v.error)) this.until.set(v.engine, now + REFLEX.benchMs);
  }
}

// ---------------------------------------------------------------- one listen's reflex

/** One decision in the turn trace (relay.reflex): counts and codes, never words. */
export interface ReflexNote {
  /** The chunk: words in the text so far. */
  n: number;
  /** When the decision came (the room makes it ms from the trace's t0). */
  at: number;
  e: ReflexEngine;
  ms: number;
  ch: number;
  tk?: number;
  pick: ReflexPick;
  raw?: ReflexPick;
  /** A mild pick: shown as its softer gesture. */
  soft?: boolean;
  c?: number;
  /** Probabilities in REFLEX_PICKS order (absent = unknown). */
  p?: Array<number | null>;
  /** The utterance category, and its confidence. */
  cat?: UtteranceCategory;
  cc?: number;
  /** This engine's pick was the one to show: "ok", or why it did not (rate_limited, agent, screen, closed, unsupported, stale: a newer chunk already answered, …). */
  fired?: string;
  /** It answered for the configured engine, which failed (that reason). */
  fb?: string;
  err?: string;
  /** compare: it answered after the configured engine's deadline. */
  late?: boolean;
}

export interface ReflexSessionDeps {
  mode: Exclude<ReflexMode, "off">;
  compare: boolean;
  avail: ReflexAvail;
  keys: { jev?: string; openai?: string };
  fetch: Fetcher;
  clock: ReflexTimer;
  timeoutMs?: number;
  bench?: EngineBench;
  /** The owner's liveliness (cadence + gates; absent: DEFAULT_LIVELINESS). */
  liveliness?: Liveliness;
  /** Show the pick (room.ts: gating + the react path), soft = its softer gesture; resolves "ok" or why not. */
  fire(pick: Exclude<ReflexPick, "none">, soft: boolean): Promise<string>;
  note(n: ReflexNote): void;
}

const toNote = (n: number, at: number, v: ReflexVerdict): ReflexNote => {
  const out: ReflexNote = { n, at, e: v.engine, ms: v.ms, ch: v.chars, pick: v.pick };
  if (v.tokens !== undefined) out.tk = v.tokens;
  if (v.raw !== undefined) out.raw = v.raw;
  if (v.soft) out.soft = true;
  if (v.conf !== undefined) out.c = v.conf;
  if (v.p && Object.keys(v.p).length) out.p = REFLEX_PICKS.map((k) => v.p?.[k] ?? null);
  if (v.cat) out.cat = v.cat;
  if (v.catConf !== undefined) out.cc = v.catConf;
  if (v.error) out.err = v.error;
  return out;
};

/**
 * The reflex of one listen: chunks the box's text so far at the liveliness's cadence, asks the
 * configured engine (and, in compare mode, every other available one), gates its verdict
 * (reflexGate) and fires it — or the keywords' when it fails or misses REFLEX.timeoutMs. A verdict
 * that comes back after a newer chunk's is stale (noted, not shown); at most REFLEX.maxInFlight
 * calls of the configured engine run at once (a chunk due beyond that waits for the next word).
 * Never blocks: everything runs beside the listen.
 */
export class ReflexSession {
  readonly engine: ReflexEngine;
  readonly fallback?: string;
  readonly liveliness: Liveliness;
  readonly profile: ReflexProfile;
  private readonly d: ReflexSessionDeps;
  private lastN = 0;
  private lastAt = 0;
  /** The newest chunk whose configured-engine verdict came back (older ones landing later are stale). */
  private answeredN = 0;
  private inFlight = 0;
  chunks = 0;
  /** Chunks due while REFLEX.maxInFlight calls were out (deferred to a later word). */
  deferred = 0;
  private closed = false;
  private readonly ctl = new AbortController();
  /** Local verdicts by chunk n, and who waits for them. */
  private readonly local = new Map<number, ReflexVerdict>();
  private readonly waiters = new Map<number, Array<(v: ReflexVerdict) => void>>();
  /** Chunks whose local wait ended (timed out): a later verdict is late. */
  private readonly localDone = new Set<number>();
  private readonly pending = new Set<Promise<void>>();
  /** The configured engine's (or its fallback's) latest category, by chunk. */
  latest: { n: number; cat: UtteranceCategory; engine: ReflexEngine } | undefined;
  /** What this listen can use (keys present, not benched). */
  private readonly avail: ReflexAvail;

  constructor(d: ReflexSessionDeps) {
    this.d = d;
    this.liveliness = isLiveliness(d.liveliness) ? d.liveliness : DEFAULT_LIVELINESS;
    this.profile = REFLEX_LIVELINESS[this.liveliness];
    const now = d.clock.now();
    const bench = d.bench;
    const avail: ReflexAvail = {
      local: d.avail.local,
      jev: d.avail.jev && !!d.keys.jev && (!bench || bench.ok("jev", now)),
      openai: d.avail.openai && !!d.keys.openai && (!bench || bench.ok("openai", now)),
    };
    this.avail = avail;
    const r = resolveEngine(d.mode, avail);
    this.engine = r.engine;
    if (r.fallback) this.fallback = r.fallback;
  }

  /** Engines that run on each chunk: the configured one, plus (compare) every other available one. */
  get engines(): ReflexEngine[] {
    if (!this.d.compare) return [this.engine];
    const all: ReflexEngine[] = [this.engine];
    for (const e of REFLEX_ENGINES) if (e !== this.engine && (e === "keywords" || this.avail[e])) all.push(e);
    return all;
  }

  /** Does this listen want the box's classifier (stt.start `reflex: true`)? */
  get wantsLocal(): boolean {
    return this.engines.includes("local");
  }

  /** The box's text so far: starts a chunk when one is due. */
  text(text: string): void {
    if (this.closed || this.chunks >= REFLEX.maxPerListen) return;
    const now = this.d.clock.now();
    const n = reflexDue(this.lastN, text, now - this.lastAt, this.profile);
    if (n === undefined) return;
    if ((this.engine === "jev" || this.engine === "openai") && this.inFlight >= REFLEX.maxInFlight) {
      this.deferred++;
      return;
    }
    this.lastN = n;
    this.lastAt = now;
    this.chunks++;
    // the box's verdicts for word counts the relay skipped are of no use any more
    for (const k of this.local.keys()) if (k < n) this.local.delete(k);
    const p = this.chunk(n, text).catch(() => {});
    this.pending.add(p);
    void p.then(() => this.pending.delete(p));
  }

  /** The box's verdict for chunk n (stt.reflex). */
  onLocal(v: ReflexVerdict & { n: number }): void {
    if (this.closed) return;
    const { n, ...verdict } = v;
    if (this.localDone.has(n)) {
      if (this.d.compare || this.engine === "local") this.d.note({ ...toNote(n, this.d.clock.now(), reflexGate(verdict, this.profile)), late: true });
      return;
    }
    const w = this.waiters.get(n);
    if (w) {
      this.waiters.delete(n);
      for (const f of w) f(verdict);
    } else if (n > this.lastN && this.local.size < REFLEX.maxPerListen) this.local.set(n, verdict); // (a word count the relay skipped: dropped)
  }

  /** The listen ended: in-flight calls are aborted, nothing more fires or is noted. */
  close(): void {
    this.closed = true;
    this.ctl.abort();
    for (const [n, w] of this.waiters) {
      this.localDone.add(n);
      for (const f of w) f({ engine: "local", pick: "none", ms: 0, chars: 0, error: "closed" });
    }
    this.waiters.clear();
  }

  /**
   * The utterance category of `text` (the filler's question, feat/quick-phrases): the latest chunk's
   * when it covered all of `text`'s words, else one call of the configured engine now (keywords on
   * failure / timeout; the box's classifier is only asked by chunk, so local answers by keywords here).
   */
  async category(text: string): Promise<{ category: UtteranceCategory; engine: ReflexEngine; ms: number }> {
    if (this.latest && this.latest.n === words(text).length) return { category: this.latest.cat, engine: this.latest.engine, ms: 0 };
    return categoryOf(text, { engine: this.engine, keys: this.d.keys, fetch: this.d.fetch, clock: this.d.clock, timeoutMs: this.d.timeoutMs });
  }

  /** Settles when every chunk started so far is done (tests, the bench). */
  async idle(): Promise<void> {
    while (this.pending.size) await Promise.all([...this.pending]);
  }

  private run(engine: ReflexEngine, n: number, text: string): Promise<ReflexVerdict> {
    const timeoutMs = this.d.timeoutMs ?? REFLEX.timeoutMs;
    if (engine === "keywords") return Promise.resolve(keywordReflex(text));
    if (engine === "local") {
      const t0 = this.d.clock.now();
      const have = this.local.get(n);
      if (have) {
        this.local.delete(n);
        return Promise.resolve(have);
      }
      return new Promise((resolve) => {
        const timer = this.d.clock.setTimeout(() => {
          const w = this.waiters.get(n);
          if (!w) return;
          this.waiters.delete(n);
          this.localDone.add(n);
          for (const f of w) f({ engine: "local", pick: "none", ms: this.d.clock.now() - t0, chars: reflexInput(text).length, error: "timeout" });
        }, timeoutMs);
        const list = this.waiters.get(n) ?? [];
        list.push((v) => {
          this.d.clock.clearTimeout(timer);
          resolve(v);
        });
        this.waiters.set(n, list);
      });
    }
    return remoteReflex(engine, text, { key: this.d.keys[engine], fetch: this.d.fetch, clock: this.d.clock, timeoutMs, signal: this.ctl.signal });
  }

  private async chunk(n: number, text: string): Promise<void> {
    const engines = this.engines;
    const others = engines.slice(1).map(async (e) => {
      const v = await this.run(e, n, text);
      if (this.closed) return;
      this.d.bench?.note(v, this.d.clock.now());
      this.d.note(toNote(n, this.d.clock.now(), v.error ? v : reflexGate(v, this.profile)));
    });
    const main = (async () => {
      this.inFlight++;
      let v: ReflexVerdict;
      try {
        v = await this.run(this.engine, n, text);
      } finally {
        this.inFlight--;
      }
      if (this.closed) return;
      this.d.bench?.note(v, this.d.clock.now());
      let fb = this.fallback;
      if (v.error) {
        // the configured engine has no answer in time: the keywords' answer instead
        this.d.note(toNote(n, this.d.clock.now(), v));
        fb = v.error;
        v = keywordReflex(text);
      }
      v = reflexGate(v, this.profile);
      const note = toNote(n, this.d.clock.now(), v);
      if (fb) note.fb = fb;
      if (v.cat && (!this.latest || this.latest.n <= n)) this.latest = { n, cat: v.cat, engine: v.engine };
      const stale = n < this.answeredN;
      if (!stale) this.answeredN = n;
      note.fired = v.pick === "none" ? "none" : stale ? "stale" : await this.d.fire(v.pick, !!v.soft);
      if (!this.closed) this.d.note(note);
    })();
    await Promise.all([main, ...others]);
  }
}

/**
 * One category answer outside a listen's chunks: the model engine when it is jev / openai with a key,
 * the keywords otherwise or when it fails. Never throws, never longer than the engine timeout.
 */
export async function categoryOf(
  text: string,
  o: { engine: ReflexEngine; keys: { jev?: string; openai?: string }; fetch: Fetcher; clock: ReflexTimer; timeoutMs?: number },
): Promise<{ category: UtteranceCategory; engine: ReflexEngine; ms: number }> {
  if ((o.engine === "jev" || o.engine === "openai") && o.keys[o.engine]) {
    const v = await remoteReflex(o.engine, text, { key: o.keys[o.engine], fetch: o.fetch, clock: o.clock, timeoutMs: o.timeoutMs });
    if (!v.error && v.cat) return { category: v.cat, engine: o.engine, ms: v.ms };
  }
  return { category: keywordCategory(text), engine: "keywords", ms: 0 };
}
