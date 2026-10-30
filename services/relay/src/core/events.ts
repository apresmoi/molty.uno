// SPDX-License-Identifier: MIT
// Agent-visible device events: the allow-list, data shapes, limits and the
// one-line summaries that ride along on tool results. Host-neutral.
//
// Firmware contract (device -> relay `{"t":"evt",name,data}`):
//   shake  {peak_g:number, reversals:int, ms:int, listen?:string}
//          (listen: the local id of the listen the device began on this shake — shake to talk,
//          core/turns.ts; the relay adopts it and agents see {listening:true, turn_id} instead)
//   tap    {x:0..1, y:0..1}
//   button {id:"accept"|"reject"}
//   answer {job, index:int|null, label?:string, reason?:"timeout"|"cancelled"}
//          (the human's tap on a molty_ask; also checked against the job's options in room.ts)
// Everything else (say.done, link_code, ...) stays internal to the relay.

import { MAX_ASK_LABEL, MAX_ASK_OPTIONS } from "./commands.js";
import { isRecord, type JsonObject } from "./protocol.js";

export interface AgentEvent {
  /** "e<seq>", monotonically increasing per device. */
  id: string;
  type: string;
  /** Relay receive time, unix ms. */
  ts: number;
  data: JsonObject;
}

export const INBOX_LIMITS = {
  /** Ring size: older events are trimmed on append. */
  keep: 200,
  /** Max age of a kept event. */
  maxAgeMs: 24 * 60 * 60 * 1000,
  /** Per-device intake cap; excess events are dropped (counted). */
  perMinute: 30,
  /** Events per pull response. */
  page: 50,
  /** Events attached to an ordinary tool/REST response. */
  piggyback: 10,
  /** Long-poll caps. */
  mcpWaitS: 8,
  restWaitS: 25,
  /** Concurrent long-polls (wait > 0) admitted per agent token / per device. */
  pollsPerToken: 2,
  pollsPerDevice: 16,
};

const num = (v: unknown, lo: number, hi: number) => typeof v === "number" && Number.isFinite(v) && v >= lo && v <= hi;
const int = (v: unknown, lo: number, hi: number) => Number.isInteger(v) && num(v, lo, hi);

/** Allow-list + shape check. Returns the clean data, or undefined to drop the event. */
const SHAPES: Record<string, (d: Record<string, unknown>) => JsonObject | undefined> = {
  shake: (d) => (num(d.peak_g, 0, 64) && int(d.reversals, 0, 1000) && int(d.ms, 0, 600000) ? { peak_g: d.peak_g as number, reversals: d.reversals as number, ms: d.ms as number } : undefined),
  tap: (d) => (num(d.x, 0, 1) && num(d.y, 0, 1) ? { x: d.x as number, y: d.y as number } : undefined),
  button: (d) => (d.id === "accept" || d.id === "reject" ? { id: d.id } : undefined),
  answer: answerShape,
};

const JOB_RE = /^molty-[a-z2-9]{10}\.j_[A-Za-z0-9_-]{8,32}$/;
const LABEL_RE = new RegExp(`^[\\x20-\\x7e]{1,${MAX_ASK_LABEL}}$`);

/**
 * Device `answer` (stateless part): a tap `{job, index, label?}` or no answer
 * `{job, index:null, reason:"timeout"|"cancelled"}`, never both. Stored as
 * `{job, index, label|null, reason?}`. room.ts additionally requires a pending
 * ask for `job`, `index` within its options and `label` equal to options[index].
 */
function answerShape(d: Record<string, unknown>): JsonObject | undefined {
  if (typeof d.job !== "string" || !JOB_RE.test(d.job)) return undefined;
  if (d.index === null || d.index === undefined) {
    if (d.reason !== "timeout" && d.reason !== "cancelled") return undefined;
    if (d.label !== undefined && d.label !== null) return undefined;
    return { job: d.job, index: null, label: null, reason: d.reason };
  }
  if (!int(d.index, 0, MAX_ASK_OPTIONS - 1) || d.reason !== undefined) return undefined;
  if (d.label !== undefined && (typeof d.label !== "string" || !LABEL_RE.test(d.label))) return undefined;
  return { job: d.job, index: d.index as number, label: (d.label as string | undefined) ?? null };
}

/** Why an ask ended without an answer. `offline` is relay-side (the device dropped mid-ask). */
export const ASK_NO_ANSWER = ["timeout", "cancelled", "offline"] as const;

/**
 * Relay-generated (not device) event: the human said something to Molty on its own initiative
 * (shake to talk, core/turns.ts). One per heard turn; never sent for no_speech. Data
 * `{turn_id, text, reason, ms:{speech, stt}, trigger:"shake", device:{caps, mood, voice}}`.
 * Filterable like device events; a device can never send it.
 */
export const UTTERANCE_EVENT = "utterance";

/** Every type an agent can receive and filter on (webhook `events`): device events + utterance. */
export const AGENT_EVENT_TYPES = [...Object.keys(SHAPES), UTTERANCE_EVENT];

/** One representative event per type: webhook templates are dry-run against every type they will receive. */
export const SAMPLE_EVENTS: Record<string, AgentEvent> = {
  shake: { id: "e1", type: "shake", ts: 0, data: { peak_g: 2.4, reversals: 5, ms: 900 } },
  tap: { id: "e2", type: "tap", ts: 0, data: { x: 0.5, y: 0.5 } },
  button: { id: "e3", type: "button", ts: 0, data: { id: "accept" } },
  answer: { id: "e4", type: "answer", ts: 0, data: { job: "molty-xxxxxxxxxx.j_sample00", index: 0, label: "yes" } },
  [UTTERANCE_EVENT]: {
    id: "e5",
    type: UTTERANCE_EVENT,
    ts: 0,
    data: {
      turn_id: "molty-xxxxxxxxxx.j_sample01",
      text: "what's on my calendar today?",
      reason: "turn_end",
      ms: { speech: 1800, stt: 120 },
      trigger: "shake",
      device: { caps: ["say", "listen", "shake_talk"], mood: "thinking", voice: "box" },
    },
  },
};

export function agentEventData(name: string, data: unknown): JsonObject | undefined {
  const shape = Object.hasOwn(SHAPES, name) ? SHAPES[name] : undefined;
  return shape && isRecord(data) ? shape(data) : undefined;
}

export function seqOf(id: unknown): number | undefined {
  if (typeof id === "number" && Number.isInteger(id) && id >= 0) return id;
  if (typeof id !== "string") return undefined;
  const m = /^e?(\d{1,15})$/.exec(id);
  return m ? Number(m[1]) : undefined;
}

export const eventId = (seq: number) => `e${seq}`;

/** A short human sentence for one event: "Molty was shaken (peak 2.1 g)". */
export function eventSentence(ev: AgentEvent): string {
  const d = ev.data as Record<string, unknown>;
  switch (ev.type) {
    case "shake":
      return d.listening === true
        ? `Molty was shaken (peak ${Number(d.peak_g).toFixed(1)} g) and is listening: the human's words follow as an utterance`
        : `Molty was shaken (peak ${Number(d.peak_g).toFixed(1)} g)`;
    case UTTERANCE_EVENT:
      return `The human said to Molty: "${String(d.text ?? "")}"`;
    case "tap":
      return "Molty's screen was tapped";
    case "button":
      return `Molty's ${d.id} button was pressed`;
    case "answer":
      return typeof d.label === "string" ? `Molty's question was answered: ${d.label}` : `Molty's question got no answer (${String(d.reason ?? "timeout")})`;
    default:
      return `Molty event ${ev.type}`;
  }
}

function ago(ms: number): string {
  const s = Math.max(0, Math.round(ms / 1000));
  if (s < 60) return `${s} s ago`;
  if (s < 3600) return `${Math.round(s / 60)} min ago`;
  return `${Math.round(s / 3600)} h ago`;
}

/** One compact line for the events riding along on a tool result. */
export function eventsLine(events: AgentEvent[], now: number, more: boolean): string {
  const parts = events.map((e) => `${eventSentence(e).replace(/^Molty /, "").replace(/^Molty's /, "")} ${ago(now - e.ts)} [${e.id}]`);
  return `Molty: ${parts.join("; ")}${more ? "; more unread: call molty_events" : ""}.`;
}
