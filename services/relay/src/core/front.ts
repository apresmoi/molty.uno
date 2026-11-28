// The voice front (docs/AGENT-LINK.md "Voice front"): a realtime voice model is Molty's OUTER voice,
// the person's agent its INNER voice. The model runs in the owner's voice box (services/molty-voice,
// engine "realtime", with the owner's own key on the owner's machine); the relay never holds a key and
// never talks to a model provider. The relay's part, in room.ts "Voice front":
//
//   - a front listen: the device's mic goes to the box as it arrives (stt.start front:true); the box's
//     model ends the person's turn (listen_stop) and answers at once (front.reply), played like any say;
//   - the agent still gets ONE `utterance` per turn, with `front: {said, waiting, role}` (what the front
//     already answered, whether it handed the turn to the agent with a holding line, and that it speaks);
//   - one voice: the agent's say / talk is never text to speech: it goes to the box (tts front:"inner",
//     turn), whose session decides at once (molty_voice/realtime.py "state"): voiced only for the turn
//     its front waits for (or between conversations), else context only (tts.end absorbed); the
//     result says which (innerResult). An answer to a turn over a minute old is only kept as context
//     (FrontTalk.fate); a repeat gets the first one's job;
//   - hands-free: after Molty speaks, it listens again, until nobody speaks, a stop phrase, a tap, the
//     agent's `front {end}`, the owner's switch, or the limits below.
//
// Host-neutral: the room (core/room.ts "Voice front") plugs FrontTalk into its device and jobs.

import type { JobStatus, Outcome } from "./api.js";
import type { KV } from "./store.js";
import type { ActivityDraft, Who } from "./activity.js";
import { isRecord, type JsonObject } from "./protocol.js";
import { type Clock, sleep } from "./store.js";
import { TURN_HOLD_MS } from "./turns.js";
import type { FrontSpeech, RemoteVoiceProvider } from "../voice/remote.js";

/** Limits on one conversation (the box has its own cost limits: session length, idle, daily minutes). */
export const FRONT = {
  /** Human turns in one hands-free conversation, then it ends (the next shake starts another). */
  maxTurns: 30,
  /** Longest conversation, ms. */
  maxMs: 15 * 60 * 1000,
  /** The front handed a turn to the agent and nothing came: after this long it says it is still waiting, once. */
  waitMs: 8000,
  /** The utterance waits this long at most for the front's reply to be known (what it said, whether it is waiting). */
  saidWaitMs: 1500,
  /**
   * A reply's first audio waits this long at most for its transcript (the text the device shows: it is
   * set once per say). The model's text usually runs ahead of its audio; what is not in by then is not shown.
   */
  textHoldMs: 150,
  /** The box decides at once whether an agent line is voiced: absorbed comes back within this, else it is voiced. */
  verdictMs: 400,
  /** The device ends a stopped listen within one 100 ms chunk; a reply waits this long at most for its end frame. */
  deviceFreeMs: 600,
  /**
   * A front say's text goes to the device as it grows (cap say_text): at most once this often, the final text always.
   * Each is a command the device answers while it plays: live, ~20 of them in a long line came with its link stalling.
   */
  textEveryMs: 300,
} as const;

export const FRONT_MODES = ["off", "on"] as const;
export type FrontMode = (typeof FRONT_MODES)[number];

/** Ends the conversation when it is all the person said (the front says goodbye first). */
const STOP_RE = /^(?:ok(?:ay)?[,.!\s]*)?(?:that'?s all|that is all|stop|bye|goodbye|good bye|bye bye|see you|thanks,? bye|thank you,? bye|i'?m done|we'?re done|never ?mind|go to sleep)(?:,?\s*molty)?[.!\s]*$/i;
export function stopPhrase(text: string): boolean {
  return STOP_RE.test(text.trim());
}

/**
 * What becomes of an agent's answer to a turn (its in_reply_to) with the front on: it goes to the box,
 * which voices it only when its front waits for that turn (molty_voice/realtime.py "state"), or not at
 * all, only kept as context: its turn is over TURN_HOLD_MS old.
 */
export type FrontFate = { speak: true } | { speak: false; reason: "old_turn" };

const INNER_HINT = {
  said: "the voice front said it for you, in its own words (`said`): you are its inner voice, not the speaker. It listens next: the person's words come as an utterance",
  absorbed: "not said: the voice front voices your line only when it handed the turn to you (front.waiting); it keeps this as context. Send only facts or decisions it lacks",
  queued: "the voice front says it once the device is free: molty_wait {job} for what it said",
  old_turn: "this turn is over a minute old: not said, kept as the front's context",
} as const;

/** What a say / talk got with the front on: the front is the speaker, the agent its inner voice. */
export function innerResult(how: "said" | "absorbed" | "queued", said?: string, reason?: "old_turn"): JsonObject {
  return { spoken: how === "said", as: "inner", how, ...(said ? { said } : {}), ...(reason ? { reason } : {}), hint: INNER_HINT[reason ?? how] };
}

/** The `utterance`'s `front.role`: plain words for any agent, whatever its skill. */
export const FRONT_ROLE = "you are the inner voice: a voice front speaks for you and already said `said`; send only facts or decisions it lacks (it waits for them when `waiting`), no greetings, no chat.";
/** ...and, while the agent has not said who it is, how to (an agent with an old skill reads `front.role` too). */
export const FRONT_SETUP =
  ' The voice front does not know who you are, so it speaks as a neutral voice: send once `front {"context": "<who you are and how you talk, 2 or 3 sentences>", "persona": true}` and, whenever they change, `front {"context": "<3 to 5 fresh facts: what you are doing, recent memories>", "facts": true}` (REST: POST /v1/devices/<id>/cmd {"name": "front", "args": {...}}; 1500 characters each).';

/** Who the agent is and its fresh facts (`front {context, persona | facts: true}`), kept per device: KV keys. */
const PERSONA = "front:persona";
const FACTS = "front:facts";

/** Speech by the front for a say / talk job (the room's Prepared). */
export interface FrontPrepared {
  front?: FrontSpeech;
}

/** What a say / talk the front voiced got: `said`, and `faithful: false` when the box found facts of the line missing. */
export function voiced(sp: FrontSpeech | undefined, said?: string): JsonObject {
  return { ...innerResult("said", said), ...(sp?.unfaithful ? { faithful: false, hint: "the voice front did NOT say your line as given (see said): tell the person another way if it matters" } : {}) };
}

/** A say / talk the front did not voice: done (it absorbed the thought), else failed. */
export function absorbedOr(sp: FrontSpeech | undefined, error: string) {
  return sp?.absorbed ? { status: "done" as const, result: innerResult("absorbed") } : { status: "failed" as const, error };
}

/** One turn's times (relay clock, ms), for its activity line. */
export interface FrontTurn {
  /** The person's turn ended (the model's turn detection, as the relay got it). */
  endAt: number;
  /** The box announced the front's reply (front.reply). */
  replyAt?: number;
  /** The box's own: from the model's end of turn to the reply's first audio (front.reply ms.created + tts.end ms.first_audio). */
  boxMs?: number;
  /** The reply's first audio left for the device. */
  replyOut?: number;
  /** The utterance reached the agents. */
  utteranceAt?: number;
  /** The front handed the turn to the agent. */
  waiting?: boolean;
  /** The agent's answer: its call, and its first audio to the device. */
  agentAt?: number;
  agentOut?: number;
}

/** The turn's times as ms after the end of the person's speech, and the one line the activity log prints. */
export function frontTurnSummary(t: FrontTurn): { ms: Record<string, number>; line: string } {
  const d = (at: number | undefined) => (at === undefined ? undefined : Math.max(0, Math.round(at - t.endAt)));
  const ms: Record<string, number> = {};
  const put = (k: string, v: number | undefined) => v !== undefined && (ms[k] = v);
  put("reply", d(t.replyAt));
  put("box", t.boxMs);
  put("reply_first_audio", d(t.replyOut));
  put("utterance", d(t.utteranceAt));
  put("agent_answer", d(t.agentAt));
  put("agent_first_audio", d(t.agentOut));
  const s = (v: number | undefined) => (v === undefined ? "-" : `${(v / 1000).toFixed(2)} s`);
  const line =
    `front turn: Molty answered ${s(ms.reply_first_audio)} after the person stopped` +
    (ms.box !== undefined ? ` (model ${s(ms.box)})` : "") +
    (ms.utterance !== undefined ? `, utterance ${s(ms.utterance)}` : "") +
    (t.waiting ? ", handed to the agent" : "") +
    (ms.agent_answer !== undefined ? `, agent answered ${s(ms.agent_answer)}, spoken ${s(ms.agent_first_audio)}` : t.waiting ? ", no agent answer" : "");
  return { ms, line };
}

/** What the conversation needs from the room (core/room.ts builds it): device, speaker lock, jobs, the log. */
export interface FrontHost {
  readonly clock: Clock;
  readonly box: RemoteVoiceProvider;
  timings(): { frontWaitMs: number; frontSaidWaitMs: number };
  log(msg: string, data: Record<string, unknown>): void;
  record(e: ActivityDraft): Promise<void>;
  /** Why the front does not serve now (null: it does). */
  whyNot(): Promise<string | null>;
  /** Plays front speech as a relay-owned lead-in say (taking over `takeover`'s audio lock when that job holds it). */
  play(sp: FrontSpeech, args: JsonObject, takeover?: string): Promise<Outcome>;
  /** The room's storage (the persona line). */
  readonly kv: KV;
  /** A relay-owned hands-free listen. */
  listen(): Promise<Outcome>;
  /** An agent line to the box's front (tts front:"inner", `turn`: the box's listen id it answers): speech, or undefined (no box). */
  prepare(text: string, exact: boolean, turn?: string): Promise<FrontPrepared | undefined>;
  /** The relay's latest human turn (KV `turn`). */
  latestTurn(): Promise<{ id: string; at: number } | undefined>;
  jobStatus(job: string): Promise<JobStatus | undefined>;
  /** A job that is done already (an answer not spoken): its id. */
  doneJob(kind: "say" | "talk", by: Who | undefined, result: JsonObject): Promise<string>;
  /** The human turn Molty is listening to now (a shake's, before its utterance). */
  openTurn(): string | undefined;
  /** Something holds or waits for the speaker. */
  busy(): boolean;
  online(): boolean;
  /** Bumped by everything an agent starts or shows. */
  agentSeq(): number;
  /** Molty thinks while the agent does. */
  thinking(): void;
}

/**
 * The hands-free conversation (in flight only, never stored): which listens are the front's, its
 * replies, each turn's times, the "still waiting" timer and every way the conversation ends.
 */
export class FrontTalk {
  private c: { started: number; turns: number; ending?: string; waitTimer?: unknown; waitSeq?: number } | undefined;
  /** Front listens by the box's stream id -> their job; the front's reply to each; each turn's times. */
  private readonly listens = new Map<string, string>();
  private readonly replies = new Map<string, FrontSpeech>();
  private readonly turns = new Map<string, FrontTurn>();
  private readonly waiting = new Map<string, () => void>();
  /** The last turns the person spoke in, oldest first: when each ended, whether the front answered it / handed it to the agent. */
  private readonly past = new Map<string, { at: number }>();
  /** Each agent's answer to a turn (`<agent> <turn>` -> its job, once known: a call in flight claims it first). */
  private readonly answers = new Map<string, { job: Promise<string | undefined>; settle: (job?: string) => void }>();

  constructor(private readonly h: FrontHost) {}

  async serving(): Promise<boolean> {
    return (await this.h.whyNot()) === null;
  }

  get live(): boolean {
    return this.c !== undefined;
  }

  /** A front listen starts (`sid`: the box's stream): the conversation begins or goes on, earlier turns' lines are written. */
  listening(sid: string, job: string) {
    this.listens.set(sid, job);
    while (this.listens.size > 16) this.listens.delete(this.listens.keys().next().value!);
    const now = this.h.clock.now();
    if (!this.c) {
      this.c = { started: now, turns: 0 };
      void this.h.record({ ts: now, kind: "voice_box", ev: "front.start", data: {} });
    }
    this.agentAnswering();
    this.c.turns++;
    for (const j of [...this.turns.keys()]) this.flush(j);
  }

  /** The person's turn in a front listen ended (the model's turn detection, or the device's own end). */
  turnEnded(job: string, at: number) {
    this.spoken(job).at = at;
    if (this.turns.has(job)) return;
    this.turns.set(job, { endAt: at });
    while (this.turns.size > 8) this.flush(this.turns.keys().next().value!);
  }

  /** The person speaks in listen `job` (the box's speech hint, or the end of their turn): the latest turn now. */
  spoken(job: string) {
    let p = this.past.get(job);
    if (!p) this.past.set(job, (p = { at: this.h.clock.now() }));
    while (this.past.size > 16) this.past.delete(this.past.keys().next().value!);
    return p;
  }

  /** The turn's utterance reached the agents. */
  uttered(job: string) {
    const t = this.turns.get(job);
    if (t) t.utteranceAt = this.h.clock.now();
  }

  /** A listen is over: a front listen nobody spoke into (that did not yield to an answer) ends the conversation, a failed hands-free one too. */
  listened(r: { ok: true; front?: boolean; text: string; yielded?: boolean } | { ok: false } | undefined, handsFree = false, job?: string) {
    if (job && !(r?.ok && r.text.trim())) this.past.delete(job); // no words: not a turn after all
    // the front answers that turn (REPLYING): never an idle end under it
    if (job && this.replies.has(job)) return;
    if (r?.ok ? r.front && !r.text.trim() && !r.yielded : this.live && handsFree) void this.end(r?.ok ? "idle" : "listen_failed");
  }

  /** The person said only a stop phrase: the conversation ends after the front's goodbye. */
  heard(text: string) {
    if (this.c && stopPhrase(text)) this.c.ending = "stop_phrase";
  }

  /** The box's front answers a listen by itself: played at once, taking the listen's lock (its device listen is over). */
  async reply(sid: string, sp: FrontSpeech) {
    const job = this.listens.get(sid);
    if (!job || !this.c || (await this.h.whyNot()) !== null) return sp.cancel();
    const t = this.turns.get(job);
    if (t) t.replyAt = this.h.clock.now();
    this.replies.set(job, sp);
    while (this.replies.size > 4) this.replies.delete(this.replies.keys().next().value!);
    this.waiting.get(job)?.();
    const started = await this.h.play(sp, { front_reply: job }, job);
    if (!started.ok) {
      this.h.log("front reply not played", { job, error: started.error });
      sp.cancel();
    }
  }

  /** What the front answered the turn `job` (for its utterance / talk result): waits frontSaidWaitMs at most. */
  async said(job: string, text: string): Promise<JsonObject | undefined> {
    if (!text.trim()) return undefined;
    const { frontSaidWaitMs: ms } = this.h.timings();
    const deadline = this.h.clock.now() + ms;
    if (!this.replies.has(job)) {
      await new Promise<void>((resolve) => {
        const timer = this.h.clock.setTimeout(resolve, ms);
        this.waiting.set(job, () => (this.h.clock.clearTimeout(timer), resolve()));
      });
      this.waiting.delete(job);
    }
    const sp = this.replies.get(job);
    if (!sp) return undefined;
    const left = deadline - this.h.clock.now();
    if (!sp.final && left > 0) await Promise.race([sp.ended, sleep(this.h.clock, left)]);
    const role = FRONT_ROLE + ((await this.h.kv.get<string>(PERSONA)) ? "" : FRONT_SETUP);
    return { said: sp.said, waiting: sp.wait, role, ...(sp.final ? {} : { partial: true }) };
  }

  /** A job the front spoke has ended (`replyOf`: it was the reply to that turn): Molty listens again, or waits for the agent. */
  spoke(replyOf: string | undefined, sp: FrontSpeech | undefined, firstOut?: number, listen = false) {
    const t = replyOf ? this.turns.get(replyOf) : undefined;
    if (t) {
      if (firstOut !== undefined) t.replyOut = firstOut;
      t.waiting = !!sp?.wait;
      // the model's own time, as the box measured it: its end of turn -> the reply's start -> its first audio
      const b = sp?.boxMs ?? {};
      if (typeof b.created === "number" || typeof b.first_audio === "number") t.boxMs = ((b.created as number) ?? 0) + ((b.first_audio as number) ?? 0);
    }
    if (!this.c && !listen) return;
    const wait = !!replyOf && !!sp?.wait;
    // once the job's audio lock is released: whatever queued behind it goes first
    this.h.clock.setTimeout(() => void this.next(wait, listen), 0);
  }

  /** `listen`: an agent's talk asked to listen after its answer: the front does (and starts the conversation). */
  private async next(wait: boolean, listen: boolean) {
    const c = this.c;
    if (!c && !listen) return;
    if (c?.ending) return this.end(c.ending);
    if (c && (c.turns >= FRONT.maxTurns || this.h.clock.now() - c.started >= FRONT.maxMs)) return this.end("limit");
    if (this.h.busy()) return; // something else speaks or listens: its end brings Molty back here
    if (!this.h.online()) return this.end("device_offline");
    if ((await this.h.whyNot()) !== null) return this.end("front_off");
    if (wait && c) {
      // the front handed the turn to the agent: Molty thinks until its answer; none in time -> "still waiting", once
      this.h.thinking();
      this.agentAnswering();
      c.waitSeq = this.h.agentSeq();
      c.waitTimer = this.h.clock.setTimeout(() => void this.stillWaiting(c), this.h.timings().frontWaitMs);
      return;
    }
    // hands-free: listen again (an agent's answer queues behind it, and it yields while nobody speaks)
    const started = await this.h.listen();
    if (!started.ok) this.h.log("front listen not started", { error: started.error });
  }

  private async stillWaiting(c: NonNullable<FrontTalk["c"]>) {
    c.waitTimer = undefined;
    if (this.c !== c || this.h.agentSeq() !== c.waitSeq || this.h.busy()) return;
    let sp: FrontSpeech;
    try {
      sp = this.h.box.frontSpeak("", "wait");
    } catch {
      return;
    }
    const started = await this.h.play(sp, { front_wait: true });
    if (!started.ok) sp.cancel();
  }

  /** An agent's answer to `turn` (in_reply_to), now (`latest`: the relay's latest turn record): FrontFate. */
  fate(turn: string, latest: { id: string; at: number } | undefined): FrontFate {
    const p = this.past.get(turn);
    const at = p?.at ?? (latest?.id === turn ? latest.at : undefined);
    if (at !== undefined && this.h.clock.now() - at > TURN_HOLD_MS) return { speak: false, reason: "old_turn" };
    return { speak: true };
  }

  /**
   * The front on, agent `hash`'s say / talk (`turn`: its in_reply_to): a repeat for the same turn gets
   * the first answer's job (unless that failed); one to a turn over a minute old only goes to the front
   * as context. Else the line goes to the box with the box's id of that listen, and the box decides at
   * once (molty_voice/realtime.py "state"): absorbed (context only) answers now with a done job, never
   * queued for the speaker; voiced comes back as `prepared` speech for the say / talk job. Undefined:
   * not the front's (off, no box: the room's own path).
   */
  async lateAnswer(kind: "say" | "talk", hash: string, by: Who | undefined, turn: string | undefined, text: string | undefined, exact = false): Promise<{ ok: true; result: JsonObject; prepared?: FrontPrepared } | undefined> {
    if (text === undefined || !(await this.serving())) return undefined;
    const done = async (result: JsonObject) => {
      const job = await this.h.doneJob(kind, by, result);
      this.answerOf(hash, turn, job);
      return { ok: true as const, result: { job } };
    };
    let settle: ((job?: string) => void) | undefined;
    if (turn !== undefined) {
      // the repeat rule holds for calls in flight too: the first claims the turn before anything is awaited,
      // a second waits for its job (live and in tests, a retry 5 ms later was voiced or absorbed on its own)
      for (let first = this.answerOf(hash, turn); first; first = this.answerOf(hash, turn)) {
        const job = await first;
        const was = job && (await this.h.jobStatus(job));
        if (was && was !== "failed") return { ok: true, result: { job: job!, repeated: true } };
        if (this.answerOf(hash, turn) === first) break; // that one came to nothing: this call answers
      }
      settle = this.claim(hash, turn);
      if (!this.fate(turn, await this.h.latestTurn()).speak) {
        this.h.box.frontContext(`Your inner voice's late answer to something the person said earlier (not said aloud): ${text}`);
        return done(innerResult("absorbed", undefined, "old_turn"));
      }
    }
    // the box's listen id of that turn ("-": not one the front heard: the box keeps it as context)
    const sid = turn === undefined ? undefined : ([...this.listens].find(([, j]) => j === turn)?.[0] ?? "-");
    const prepared = await this.h.prepare(text, exact, sid).catch(() => undefined);
    if (!prepared?.front) return settle?.(), undefined;
    const sp = prepared.front;
    void sp.ended.then(() => sp.feed.error !== undefined && settle?.()); // cancelled (the job was refused): the claim comes to nothing
    await Promise.race([sp.ended, sleep(this.h.clock, FRONT.verdictMs)]);
    if (sp.absorbed) return done(innerResult("absorbed"));
    this.agentAnswering();
    return { ok: true, result: innerResult("queued"), prepared };
  }

  /** The answer of agent `hash` to `turn` (its job, maybe still in flight), or records `job` as it (a retry of a failed one replaces it). */
  answerOf(hash: string, turn: string | undefined, job?: string): Promise<string | undefined> | undefined {
    if (turn === undefined) return undefined;
    const a = this.answers.get(`${hash} ${turn}`);
    if (job === undefined) return a?.job;
    a?.settle(job);
    this.claim(hash, turn)(job);
    return a?.job;
  }

  /** Agent `hash` is answering `turn` now: a second call for it waits for this one's job (settle it). */
  private claim(hash: string, turn: string): (job?: string) => void {
    const k = `${hash} ${turn}`;
    let settle!: (job?: string) => void;
    const job = new Promise<string | undefined>((r) => (settle = r));
    this.answers.delete(k);
    this.answers.set(k, { job, settle });
    while (this.answers.size > 16) this.answers.delete(this.answers.keys().next().value!);
    return settle;
  }

  /** The agent is answering: no "still waiting". */
  agentAnswering(c = this.c) {
    if (c?.waitTimer !== undefined) this.h.clock.clearTimeout(c.waitTimer);
    if (c) c.waitTimer = undefined;
  }

  /** The agent's answer to a front turn started playing: that turn's line is complete. */
  answered(turn: string, called: number, firstOut: number) {
    const t = this.turns.get(turn);
    if (!t) return;
    t.agentAt = called;
    t.agentOut = firstOut;
    this.flush(turn);
  }

  /** The conversation is over: the box closes its session (its billing stops), each turn's line is written. */
  async end(reason: string) {
    const c = this.c;
    if (!c) return;
    this.c = undefined;
    this.agentAnswering(c);
    for (const j of [...this.turns.keys()]) this.flush(j);
    this.h.box.frontEnd(reason);
    const now = this.h.clock.now();
    this.h.log("front conversation ended", { reason, turns: c.turns, ms: now - c.started });
    await this.h.record({ ts: now, kind: "voice_box", ev: "front.end", data: { reason, turns: c.turns, ms: now - c.started } });
  }

  /** One turn's times: a log line and one activity entry (kind trace, ev "front"), printed as one line. */
  private flush(job: string) {
    const t = this.turns.get(job);
    if (!t) return;
    this.turns.delete(job);
    const { ms, line } = frontTurnSummary(t);
    this.h.log("front turn", { job, ...ms });
    void this.h.record({ ts: t.endAt, kind: "trace", ev: "front", job, data: { ...ms, ...(t.waiting ? { waiting: true } : {}), line } });
  }

  /** The box's session opened / closed (its own limits, usage): for the owner's activity log. */
  async session(f: Record<string, unknown>) {
    if (f.state !== "open" && f.state !== "closed") return;
    const data: JsonObject = { state: f.state };
    if (typeof f.reason === "string") data.reason = f.reason.slice(0, 40);
    if (typeof f.s === "number" && Number.isFinite(f.s)) data.s = Math.round(f.s);
    if (typeof f.today_s === "number" && Number.isFinite(f.today_s)) data.today_s = Math.round(f.today_s);
    if (isRecord(f.usage)) {
      const u: JsonObject = {};
      for (const [k, v] of Object.entries(f.usage).slice(0, 8)) if (/^[a-z_]{1,24}$/.test(k) && typeof v === "number" && Number.isFinite(v)) u[k] = Math.round(v);
      data.usage = u;
    }
    await this.h.record({ ts: this.h.clock.now(), kind: "voice_box", ev: "front.session", data });
    // the box closed its session by itself at a hard limit: the conversation cannot go on
    if (f.state === "closed" && typeof f.reason === "string" && ["daily_cap", "max_session"].includes(f.reason)) await this.end(`box_${f.reason}`);
  }

  /** A front box (re)connected: it gets the agent's persona line again. */
  async boxed() {
    for (const [k, as] of [[PERSONA, "persona"], [FACTS, "facts"]] as const) {
      const t = await this.h.kv.get<string>(k);
      if (t) this.h.box.frontContext(t, as);
    }
  }

  /** The owner's view (GET …/voice `front`): the switch, whether it serves and why not, the conversation. */
  view(front: string | undefined, mode: string, box: { front?: boolean } | undefined, engine: string | null): JsonObject {
    const why = frontWhyNot(front, mode, box);
    return { mode: front ?? "off", available: !!box?.front, serving: !why, ...(why && front === "on" ? { reason: why } : {}), conversation: this.live, engine };
  }

  /** The agent's `front {context?, end?}`: silent background for the front, or the conversation's end. */
  async command(args: JsonObject, name?: string): Promise<Outcome> {
    const why = await this.h.whyNot();
    if (why !== null) return { ok: true, result: { skipped: "front_off", reason: why } };
    const out: JsonObject = { conversation: this.live };
    // kept by the box across its sessions (never one opened for it); who the agent is (with its paired name, so
    // the front can say "yes, this is Dude") and its fresh facts by the relay too, given to every box that connects
    const as = args.persona === true ? "persona" : args.facts === true ? "facts" : undefined;
    let text = args.context as string;
    if (as === "persona" && name) text = `Your agent's name: ${name}. ${text}`;
    if (as) await this.h.kv.put(as === "persona" ? PERSONA : FACTS, text);
    if (typeof args.context === "string") out.context = this.h.box.frontContext(text, as);
    if (args.end === true) {
      out.ended = this.live;
      await this.end("agent");
    }
    return { ok: true, result: out };
  }
}

/** Why the front does not serve now, in plain words for the owner (null: it does). */
export function frontWhyNot(front: string | undefined, mode: string, box: { front?: boolean } | undefined): string | null {
  if (front !== "on") return "the voice front is off";
  if (mode === "off" || mode === "cloud") return `voice mode is ${mode}: the voice front runs on your voice box (mode auto or box)`;
  if (!box) return "no voice box is connected: run molty-voice with the realtime engine";
  if (!box.front) return "the connected voice box offers no voice front (engine kyutai, an older molty-voice, or its daily limit is reached)";
  return null;
}

/**
 * The text a front say shows (the device takes it once, with `cmd say`): what the front said by its
 * first audio, waiting FRONT.textHoldMs at most from `from` for the sentence (or the transcript) to end.
 */
export async function frontText(sp: FrontSpeech, clock: Clock, from: number, caps?: string[]): Promise<string> {
  // the device takes the rest as it comes (cap say_text): the first word is enough, the voice never waits for more
  const enough = caps?.includes(SAY_TEXT_CAP) ? () => sp.said.trim() !== "" : () => /[.!?…]$/.test(sp.said);
  while (!sp.final && !sp.feed.done && clock.now() - from < FRONT.textHoldMs && !enough()) {
    await Promise.race([sp.changed(), sleep(clock, Math.max(1, FRONT.textHoldMs - (clock.now() - from)))]);
  }
  return sp.said.trim();
}

/** Firmware cap: the device takes a say's text as it grows (`cmd say_text {job, text}`). */
export const SAY_TEXT_CAP = "say_text";
const TEXT_MAX_BYTES = 4096;

/**
 * The text of front say `sp` as the model speaks it (docs/AGENT-LINK.md "Text in pieces"), when the
 * device has the cap (false: it does not, nothing is sent). After the `cmd say` that carried `first`,
 * `cmd say_text {job, text}` gets the say's whole text so far, whole words only, each continuing the
 * last byte for byte: at most every FRONT.textEveryMs, the final text always. It stops at the final
 * text, when the device does not take it (taken:false, an error), when the say is over (`job` is not
 * `saying`), or when the model's transcript changed words already sent (a diverging text is never sent).
 */
export function frontFollow(sp: FrontSpeech, caps: string[] | undefined, first: string, clock: Clock, job: string, saying: Set<string>, cmd: (name: string, args: JsonObject) => Promise<Outcome<JsonObject>>): boolean {
  if (!caps?.includes(SAY_TEXT_CAP)) return false;
  void (async () => {
    let sent = first;
    const dev = { taken: true }; // the device's last answer: taken:false stops the text
    await sleep(clock, FRONT.textEveryMs); // after the cmd say, the latest text of the window
    for (;;) {
      const text = sp.final ? sp.said.trim() : sp.said.replace(/\S*$/, "").trim();
      if (!saying.has(job)) return;
      if (!dev.taken) return;
      if (text !== sent) {
        if (!text.startsWith(sent) || !/\s/.test(text[sent.length] ?? "") || new TextEncoder().encode(text).length > TEXT_MAX_BYTES) return;
        // never awaited: the next text never waits for the device's answer to this one (only taken:false stops it)
        void cmd("say_text", { job, text }).then((r) => (dev.taken = r.ok && r.result.taken !== false));
        sent = text;
      }
      if (sp.final) return;
      await sp.changed();
      await sleep(clock, FRONT.textEveryMs);
    }
  })();
  return true;
}
