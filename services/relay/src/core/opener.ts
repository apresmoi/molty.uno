// First words: the start of Molty's reply, said before the agent can speak.
//
//   turn end ─▶ the box's small model (molty_voice/opener.py; the last few messages + the human's
//   words) ─▶ guardOpener ─▶ TTS ─▶ Molty speaks ─▶ the agent takes over at a word of the line
//
// The model writes the real start of the answer (about 12 to 20 words in short clauses) and knows
// nothing the agent knows. So new plain words may pass, but a name, a number, a yes or no, a result
// or a statement about the world may not. The fallback is silence, never a generic line.
//
// Overlap (planReply): the agent reads the planned line, may claim a branch point (keep: m words),
// and starts its reply with those m words; the relay stops the line there and plays the rest.

export const FIRST_WORDS_MODES = ["off", "opener"] as const;
export type FirstWordsMode = (typeof FIRST_WORDS_MODES)[number];

export const OPENER = {
  /** The whole round trip to the box. Later than this the moment has passed: silence. */
  deadlineMs: 800,
  /** Messages (both sides) given to the model. */
  historyMessages: 5,
  /** Older than this the conversation is over: it is not context any more. */
  historyTtlMs: 10 * 60_000,
  historyChars: 300,
  maxWords: 22,
  maxChars: 150,
  /** No answer this long after a line left hanging mid-sentence: `closing`. */
  hangMs: 15_000,
  closing: "Hmm, give me a moment.",
  /** Said between a line that went past the reply's branch point and the reply. */
  repair: "I mean,",
  /** Words per minute of a line until one has been measured (the box's voice, roughly). */
  wpm: 170,
  /** The sound of Molty pausing at a kept word, waiting for the agent (a built-in quick phrase). */
  pause: "hmm",
} as const;

export interface OpenerMessage {
  role: "human" | "molty";
  text: string;
  at: number;
}

export type OpenerVerdict = { ok: true; text: string } | { ok: false; why: string };

/** Lowercase words, accents off: "¿Qué tal?" → que, tal. */
function tokens(text: string): string[] {
  return (
    text
      .normalize("NFD")
      .replace(/[̀-ͯ]/g, "")
      .toLowerCase()
      .replace(/[’‘]/g, "'")
      .match(/[\p{L}\p{N}]+(?:'[\p{L}]+)?/gu) ?? []
  );
}

const set = (s: string) => new Set(s.split(/\s+/).filter(Boolean));

// Words that say nothing about the world: starting, reacting, pointing. No adjective or noun that
// could be an answer belongs here (no "cold", "sunny", "late", "broken").
const COMMON = set(`
  a an the this that these those it its i i'm i'll i've me my we we'll let's lets let you you're your us our
  and but or so if then well now just still also too very really quite pretty kind sort of some any all
  to in on at for from with about by up out into over
  ok okay alright right sure yeah yep ah oh oof ooh ugh uh hmm hm huh mm um wow hey hi hello morning evening
  thanks welcome sorry nice cool good great fun tough rough hard
  what's what how why when where who which
  look looking see seeing check checking think thinking try trying find finding get getting go going
  dig digging pull pulling start starting work working figure figuring read reading open opening help helping
  know wonder wondering guess mean means say saying sounds sound seems seem feel feels hear hearing
  give moment second sec bit while first thing things part question idea one long much many come coming
  is be are do does can could would will should need needs want wants have has got
  back here there
  a el la los las un una unos unas lo le me mi mis te tu tus nos nuestro yo tu vos
  y o pero entonces bueno ya ahora solo tambien muy si
  de en con para por sobre al del
  vale dale claro uy oh ay hola gracias perdon bien genial
  que como por porque cuando donde quien cual
  ver veo veamos mirar miro mira buscar busco pensar pienso probar pruebo revisar reviso
  voy vamos dejame deja haber a ver dame momento segundo rato cosa tema pregunta
  eso esto esta este estas estos ese esa
  puedo podemos quiero
  your you you're tu vos
  huh wow oof yikes nice cool great lovely tricky wild fun sad awful terrible fantastic wonderful rough tough
  really truly definitely certainly honestly maybe perhaps about that that's those these
  uf uh guau genial bárbaro tremendo
`);

const LEAD_INS = set("ok okay alright right sure yeah well so hmm hm ah oh ugh hey hi hola bueno vale dale claro");

const YES_NO = /^(is|are|was|were|am|did|do|does|has|have|had|can|could|will|would|should|isn't|aren't|didn't|doesn't|won't|es|son|fue|está|hay|tiene)\b/i;
const SPANISH = /[áéíóúñ¿¡]|\b(que|de|el|la|los|las|un|una|por|para|hola|como|cuanto|estoy|mi)\b/i;

const VERDICTS = set("yes yup yep nope si");

const NUMBER_WORDS = set(`
  zero two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen seventeen
  eighteen nineteen twenty thirty forty fifty sixty seventy eighty ninety hundred thousand million billion half
  dos tres cuatro cinco seis siete ocho nueve diez once doce veinte treinta cuarenta cincuenta cien mil millon
`);

// a claim that something happened or an answer follows: only the agent can say those
const CLAIMS: Array<[RegExp, string]> = [
  [/^(yes|no|nope|yup|si|sí)\b/i, "verdict"],
  [/\b(done|finished|completed|listo|hecho|terminado|termine|termin[eé]|encontr[eé]|arreglado)\b/i, "claim"],
  [/\bi(?:'ve| have)\s+\p{L}+/iu, "claim"],
  [/\b(found|fixed|sent|added|saved|booked|scheduled|reminded|created|updated|deleted|called|played|i will|i'll)\b/i, "claim"],
  [/\b(took|takes|took a|equals|because)\b/i, "result"],
  [/\b\p{L}+n['’]t\b/iu, "result"], // "the tests didn't run", "it won't"
  [/\b(maybe|perhaps|probably|certainly|definitely|likely|surely|quizás|quizas|seguro)\b/i, "guess"],
  [/\b(question|interesting|fascinating|topic|pregunta|interesante)\b/i, "meta"], // "that's a good question": no line at all is better
  [/\b(it's|it is|it was|it will|there's|there is|there are|they are|they're|answer is|result is)\b/i, "result"],
  [/\b(es|son|fue|fueron|será|sera|hay)\b/i, "result"],
];

/** "crashing", "crashes", "crash" → one form. */
function stem(word: string): string {
  const w = word.replace(/'s$/, "");
  if (w.length < 5) return w;
  // "quitting" → "quit", "stopped" → "stop"
  return w.replace(/(ing|ed|es|s|e)$/, "").replace(/([bdgklmnprt])\1$/, "$1");
}

/** Is `w` one of `known`, allowing for an ending? */
function knownWord(w: string, known: Set<string>): boolean {
  if (known.has(w)) return true;
  const s = stem(w);
  for (const k of known) if (stem(k) === s) return true;
  return false;
}

/**
 * What may be spoken of the model's `line`, or why nothing may. `human`: their words this turn;
 * `history`: the conversation before them (names and topics it may repeat).
 */
export function guardOpener(line: unknown, human: string, history: readonly OpenerMessage[]): OpenerVerdict {
  if (typeof line !== "string") return { ok: false, why: "empty" };
  const text = line.trim().replace(/^["'“”‘’]+|["'“”‘’]+$/g, "").trim();
  const ws = tokens(text);
  if (!ws.length) return { ok: false, why: "empty" };
  if (ws.length > OPENER.maxWords || text.length > OPENER.maxChars) return { ok: false, why: "long" };
  if (/[\n*#_`<>[\]{}/\\]|->|\bhuman\b|\bmolty\b/i.test(text)) return { ok: false, why: "format" };
  if (/[?¿]/.test(text)) return { ok: false, why: "question" }; // it would wait for an answer nobody is listening for
  const heard = tokens(human);
  const said = new Set(heard);
  // their sentence back, even behind an "okay," ("okay, remind me to call my sister")
  const body = ws.slice(LEAD_INS.has(ws[0]!) ? 1 : 0);
  while (body.length > 1 && LEAD_INS.has(body[body.length - 1]!)) body.pop();
  const copied = (from: string) => ` ${tokens(from).join(" ")} `.includes(` ${body.join(" ")} `);
  // naming their topic is the point; handing back nearly all they said is not
  if (body.length >= 2 && copied(human) && body.length >= heard.length * 0.8) return { ok: false, why: "parrot" };
  if (body.length >= 3 && history.some((m) => copied(m.text))) return { ok: false, why: "parrot" };
  // "did the tests pass?" → "tests passed": their word, turned into the answer
  for (const w of ws) if (/(ed|ó|é)$/.test(w) && w.length >= 5 && !said.has(w) && heard.some((h) => stem(h) === stem(w))) return { ok: false, why: "claim" };
  // "is the door locked?" → "locked, right": a question for yes or no, answered yes
  if (YES_NO.test(human.trim()) && (/\b(right|yeah|yep|sure|claro|dale|certainly|definitely|really|absolutely)\W*$/i.test(text) || body.every((w) => said.has(w) || LEAD_INS.has(w)) && body.length <= 2)) return { ok: false, why: "verdict" };
  // the few-shot's Spanish leaking into another language
  if (/^a ?ver\b/.test(ws.join(" ")) && !SPANISH.test(human)) return { ok: false, why: "language" };
  // "remind me to call my sister" → "calling my sister": their words, the wrong person
  if (/\bme\b/.test(ws.join(" ").replace(/\b(let|give) me\b/g, "")) || (ws.includes("my") && said.has("my"))) return { ok: false, why: "parrot" };
  if (/\d/.test(text) && !(text.match(/\d+/g) ?? []).every((n) => said.has(n))) return { ok: false, why: "number" };
  for (const w of ws) if ((NUMBER_WORDS.has(w) || NUMBER_WORDS.has(w.replace(/s$/, ""))) && !said.has(w)) return { ok: false, why: "number" };
  if (ws.some((w) => VERDICTS.has(w))) return { ok: false, why: "verdict" };
  const straight = text.replace(/’/g, "'");
  for (const [re, why] of CLAIMS) if (re.test(straight) && !re.test(human)) return { ok: false, why };
  // "It went to Ana", "Berlin was quite busy": a sentence about the world, which only the agent can know
  if (/^(it|she|he|they|we)\b/i.test(straight) || /\b(is|are|was|were|has|had|looks?|looked|seems?|seemed)\b/i.test(straight)) return { ok: false, why: "result" };
  const known = new Set([...said, ...history.flatMap((m) => tokens(m.text))]);
  // it only fills the gap: new plain words may pass, a new name may not. A capital mid-line is a
  // name; a sentence's first word has a capital anyway, so only forms that can't be one pass there
  const raw = text.match(/[\p{L}\p{N}]+(?:['’][\p{L}]+)?/gu) ?? [];
  for (let i = 0; i < raw.length; i++) {
    const w = raw[i]!.toLowerCase().replace(/’/g, "'");
    if (COMMON.has(w) || knownWord(w, known) || !/^\p{Lu}/u.test(raw[i]!)) continue;
    if (i === 0 && /(ing|ly|ful)$/.test(w)) continue;
    return { ok: false, why: "unknown_word" };
  }
  return { ok: true, text };
}

/** Does the line stop mid-sentence, so that silence after it would be odd? */
export function hangs(text: string): boolean {
  return !/[.!…]\s*$/.test(text);
}

const SAID_RE = /\[[^\]\n]*\]|[\p{L}\p{N}]+(?:['’][\p{L}]+)?/gu;

/**
 * How many of the line's `words` (as spoken, speechWords) `say` starts with, word for word (at most
 * `limit`), and `say` after them. Case, accents and punctuation aside; cues (`[happy]`) among the
 * repeated words move to the front. Works on the raw text so that cue positions stay right.
 */
export function afterShared(say: string, words: readonly string[], limit = words.length): { n: number; rest: string } {
  const want: string[] = [];
  const of: number[] = []; // the word each token belongs to
  words.slice(0, limit).forEach((w, i) => {
    for (const t of tokens(w)) {
      want.push(t);
      of.push(i);
    }
  });
  let j = 0;
  const whole = () => (j === want.length ? limit : of[j]!); // words fully said (a word without letters counts once reached)
  let n = 0;
  let end = 0;
  const cues: string[] = [];
  for (const m of say.matchAll(SAID_RE)) {
    if (m[0].startsWith("[")) {
      cues.push(m[0]);
      continue;
    }
    if (j === want.length || tokens(m[0])[0] !== want[j]) break;
    j++;
    if (whole() > n) {
      n = whole();
      end = m.index + m[0].length;
    }
  }
  if (!n) return { n, rest: say };
  let rest = say.slice(end).replace(/^[\s,.;:!…-]+/, "");
  if (![...rest].some((c) => /[\p{L}\p{N}]/u.test(c))) return { n, rest: "" };
  if (/[.!?…]["')\]]*$/.test(words[n - 1]!)) rest = rest[0]!.toUpperCase() + rest.slice(1);
  const front = cues.filter((c) => say.indexOf(c) < end);
  return { n, rest: (front.length ? front.join(" ") + " " : "") + rest };
}

/** The end of the clause that word `from` (0-based) is in: words up to its comma or stop, counted. */
export function clauseEnd(words: readonly string[], from: number): number {
  if (from <= 0) return 0;
  for (let c = from; c < words.length; c++) if (/[,.;:!?…]["')\]]*$/.test(words[c - 1]!)) return c;
  return words.length;
}

/** The line as it plays: what the relay sent of it, and where it stops. */
export interface LinePlay {
  words: readonly string[];
  /** Words whose audio has started going to the device (a stop before them is still possible). */
  sent: number;
  /** The stream is over: `stop` words were played. */
  ended: boolean;
  /** Words the relay plays of it (a keep, or a reply's plan); absent: all. */
  stop?: number;
  /** The agent's keep: its reply goes on after this many words. */
  keep?: number;
}

export type ReplyPlan = { stop: number; text: string; how: "switch" | "repair" | "after" };

/**
 * An answer to the turn while (or after) its line plays: where the line stops and what is said after
 * it. `keep`: the reply names its branch point (its text is then what follows those words);
 * otherwise the line's words the reply starts with are its branch point (or the agent's keep).
 * - switch: the line stops where the reply branches (it has not got further) and the rest follows;
 * - repair: it already went further: it finishes its clause, then "I mean," and the reply, without
 *   the words already spoken;
 * - after: a reply that ignores the line plays after the clause playing now.
 */
export function planReply(say: string, line: LinePlay, keep?: number): ReplyPlan {
  const { words } = line;
  const k = afterShared(say, words).n;
  const want = keep ?? line.keep ?? (k || undefined);
  const stop = line.ended ? (line.stop ?? words.length) : want !== undefined && want >= line.sent ? want : clauseEnd(words, line.sent);
  if (k >= stop) return { stop, text: afterShared(say, words, stop).rest, how: "switch" };
  if (want === undefined) return { stop, text: say, how: "after" };
  const own = keep !== undefined ? (k >= keep ? k : 0) : k; // the reply's words that repeat the line
  if (stop <= want) {
    // the line stopped short of the branch point (an explicit keep past it): its missing words go first
    const gap = words.slice(stop, want).join(" ");
    const rest = afterShared(say, words, own).rest;
    return { stop, text: gap && rest ? `${gap} ${rest}` : gap || rest, how: "switch" };
  }
  const rest = afterShared(say, words, own).rest;
  return { stop, text: rest ? `${OPENER.repair} ${rest}` : "", how: "repair" };
}

/** The conversation so far, newest last, kept short. */
export function pushHistory(hist: OpenerMessage[], m: OpenerMessage): OpenerMessage[] {
  const text = m.text.trim().slice(0, OPENER.historyChars);
  if (!text) return hist;
  const kept = hist.filter((x) => m.at - x.at <= OPENER.historyTtlMs);
  const last = kept[kept.length - 1];
  // one reply in two says (first words, then the answer) is one message
  if (last?.role === m.role) return [...kept.slice(0, -1), { ...m, text: `${last.text} ${text}`.slice(-OPENER.historyChars) }];
  return [...kept, { ...m, text }].slice(-OPENER.historyMessages);
}
