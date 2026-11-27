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
//   - hands-free: after Molty speaks, it listens again, until nobody speaks, a stop phrase, a tap, the
//     agent's `front {end}`, the owner's switch, or the limits below.
//
// Host-neutral constants and helpers only; the state lives in the room (in flight, never stored).

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
    `, utterance ${s(ms.utterance)}` +
    (t.waiting ? ", handed to the agent" : "") +
    (ms.agent_answer !== undefined ? `, agent answered ${s(ms.agent_answer)}, spoken ${s(ms.agent_first_audio)}` : t.waiting ? ", no agent answer" : "");
  return { ms, line };
}
