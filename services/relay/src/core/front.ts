// The voice front (docs/AGENT-LINK.md "Voice front"): a realtime voice model is Molty's OUTER voice,
// the person's agent its INNER voice. The model runs in the owner's voice box (services/molty-voice,
// engine "realtime", with the owner's own key on the owner's machine); the relay never holds a key and
// never talks to a model provider. The relay's part, in room.ts "Voice front":
//
//   - a front listen: the device's mic goes to the box as it arrives (stt.start front:true); the box's
//     model ends the person's turn (listen_stop) and answers at once (front.reply), played like any say;
//   - the agent still gets ONE `utterance` per turn, with `front: {said, waiting}` (what the front already
//     answered, and whether it handed the turn to the agent with a holding line);
//   - the agent's say / talk is not run through text to speech: its text goes to the box as inner voice
//     (tts front:"own", or "exact" with exact:true), the front says it, queued behind whatever it is saying;
//   - an agent's answer is said only while it belongs to the moment (FrontTalk.fate): a late one is
//     silent context, a stale one dropped, a repeat gets the first one's job;
//   - hands-free: after Molty speaks, it listens again, until nobody speaks, a stop phrase, a tap, the
//     agent's `front {end}`, the owner's switch, or the limits below.
//
// Host-neutral: the room (core/room.ts "Voice front") plugs FrontTalk into its device and jobs.

import type { JobStatus, Outcome } from "./api.js";
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
  /** The device ends a stopped listen within one 100 ms chunk; a reply waits this long at most for its end frame. */
  deviceFreeMs: 600,
} as const;

export const FRONT_MODES = ["off", "on"] as const;
export type FrontMode = (typeof FRONT_MODES)[number];

/** Ends the conversation when it is all the person said (the front says goodbye first). */
const STOP_RE = /^(?:ok(?:ay)?[,.!\s]*)?(?:that'?s all|that is all|stop|bye|goodbye|good bye|bye bye|see you|thanks,? bye|thank you,? bye|i'?m done|we'?re done|never ?mind|go to sleep)(?:,?\s*molty)?[.!\s]*$/i;
export function stopPhrase(text: string): boolean {
  return STOP_RE.test(text.trim());
}

/**
 * What becomes of an agent's answer to a turn (its in_reply_to) with the front on: spoken, or not
 * (`as`): given to the front as silent context (the person has spoken since), or dropped (the turn
 * is over TURN_HOLD_MS old, or its conversation ended and the front had answered it).
 */
export type FrontFate = { speak: true } | { speak: false; as: "context" | "dropped"; reason: "newer_turn" | "old_turn" | "conversation_over" };

const FATE_HINT = {
  newer_turn: "the person has spoken again since this turn: not said aloud, the voice front has it as context. Answer their newer utterance",
  old_turn: "this turn is over a minute old: not said aloud",
  conversation_over: "this conversation is over and the voice front already answered this turn: not said aloud",
} as const;

/** The result of an answer that was not spoken (its job is done with it). */
export function lateResult(f: Extract<FrontFate, { speak: false }>): JsonObject {
  return { spoken: false, as: f.as, reason: f.reason, hint: FATE_HINT[f.reason] };
}

/** A talk's hint when the front does its listen. */
export const FRONT_LISTENS = "the voice front listens now: what the person says comes as an utterance";

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
  /** A relay-owned hands-free listen. */
  listen(): Promise<Outcome>;
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
  private readonly past = new Map<string, { at: number; answered?: boolean; held?: boolean }>();
  /** Each agent's first answer to a turn (`<agent> <turn>` -> its job). */
  private readonly answers = new Map<string, string>();

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
    while (this.listens.size > 4) this.listens.delete(this.listens.keys().next().value!);
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
    this.spoken(job).answered = true;
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
    return { said: sp.said, waiting: sp.wait, ...(sp.final ? {} : { partial: true }) };
  }

  /** A job the front spoke has ended (`replyOf`: it was the reply to that turn): Molty listens again, or waits for the agent. */
  spoke(replyOf: string | undefined, sp: FrontSpeech | undefined, firstOut?: number, listen = false) {
    const t = replyOf ? this.turns.get(replyOf) : undefined;
    if (t) {
      if (firstOut !== undefined) t.replyOut = firstOut;
      t.waiting = !!sp?.wait;
      if (t.waiting) this.spoken(replyOf!).held = true;
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

  /**
   * An agent's answer to `turn` (in_reply_to), now (`latest`: the relay's latest turn record). Spoken
   * when it still belongs to the moment; the answer the front is waiting for always is (but a minute
   * on); the others are context or dropped (FrontFate).
   */
  fate(turn: string, latest: { id: string; at: number } | undefined): FrontFate {
    const p = this.past.get(turn);
    const at = p?.at ?? (latest?.id === turn ? latest.at : undefined);
    if (at !== undefined && this.h.clock.now() - at > TURN_HOLD_MS) return { speak: false, as: "dropped", reason: "old_turn" };
    if (p?.held) return { speak: true };
    let last: string | undefined;
    for (const k of this.past.keys()) last = k;
    const open = this.h.openTurn();
    if ((latest && latest.id !== turn) || (last !== undefined && last !== turn) || (open !== undefined && open !== turn)) {
      return this.live ? { speak: false, as: "context", reason: "newer_turn" } : { speak: false, as: "dropped", reason: "conversation_over" };
    }
    if (!this.live && p?.answered) return { speak: false, as: "dropped", reason: "conversation_over" };
    return { speak: true };
  }

  /**
   * The front on, agent `hash`'s say / talk answering `turn` (in_reply_to): its answer to that turn
   * again gets the first one's job (unless that failed); one that no longer belongs to the moment
   * (fate) is not spoken: the front has it as context, its job is done at once saying so.
   * undefined: speak it.
   */
  async lateAnswer(kind: "say" | "talk", hash: string, by: Who | undefined, turn: string | undefined, text: string | undefined): Promise<{ ok: true; result: JsonObject } | undefined> {
    if (turn === undefined || text === undefined || !(await this.serving())) return undefined;
    const first = this.answerOf(hash, turn);
    const was = first && (await this.h.jobStatus(first));
    if (was && was !== "failed") return { ok: true, result: { job: first!, repeated: true } };
    const f = this.fate(turn, await this.h.latestTurn());
    if (f.speak) return undefined;
    if (f.as === "context") this.h.box.frontContext(`Your agent's late answer to something the person said earlier (not said aloud): ${text}`);
    return { ok: true, result: { job: this.answerOf(hash, turn, await this.h.doneJob(kind, by, lateResult(f)))! } };
  }

  /** The answer of agent `hash` to `turn` (its job), or records `job` as it (a retry of a failed one replaces it). */
  answerOf(hash: string, turn: string | undefined, job?: string): string | undefined {
    const k = `${hash} ${turn}`;
    if (job === undefined || turn === undefined) return this.answers.get(k);
    this.answers.delete(k);
    this.answers.set(k, job);
    while (this.answers.size > 16) this.answers.delete(this.answers.keys().next().value!);
    return job;
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

  /** The owner's view (GET …/voice `front`): the switch, whether it serves and why not, the conversation. */
  view(front: string | undefined, mode: string, box: { front?: boolean } | undefined, engine: string | null): JsonObject {
    const why = frontWhyNot(front, mode, box);
    return { mode: front ?? "off", available: !!box?.front, serving: !why, ...(why && front === "on" ? { reason: why } : {}), conversation: this.live, engine };
  }

  /** The agent's `front {context?, end?}`: silent background for the front, or the conversation's end. */
  async command(args: JsonObject): Promise<Outcome> {
    const why = await this.h.whyNot();
    if (why !== null) return { ok: true, result: { skipped: "front_off", reason: why } };
    const out: JsonObject = { conversation: this.live };
    if (typeof args.context === "string") out.context = this.live ? this.h.box.frontContext(args.context) : false;
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
export async function frontText(sp: FrontSpeech, clock: Clock, from: number): Promise<string> {
  while (!sp.final && !sp.feed.done && clock.now() - from < FRONT.textHoldMs && !/[.!?…]$/.test(sp.said)) {
    await Promise.race([sp.changed(), sleep(clock, Math.max(1, FRONT.textHoldMs - (clock.now() - from)))]);
  }
  return sp.said.trim();
}
