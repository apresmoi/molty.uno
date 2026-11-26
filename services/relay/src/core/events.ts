// SPDX-License-Identifier: MIT
// Agent-visible device events: the allow-list, data shapes, limits and the
// one-line summaries that ride along on tool results. Host-neutral.
//
// Firmware contract (device -> relay `{"t":"evt",name,data}`):
//   shake  {peak_g:number, reversals:int, ms:int, listen?:string}
//          (listen: the local id of the listen the device began on this shake — shake to talk,
//          core/turns.ts; the relay adopts it and agents see {listening:true, turn_id} instead)
//   tap    {x:0..1, y:0..1, spot?:string, opened?:string, closed?:true}
//          (spot: the id of the scene's tap spot the finger landed on, when it landed on one; opened: the
//          layout Molty itself opened because of that tap; closed: the tap beside an opened layout took
//          Molty back to the one under it)
//   scene  {why:"open"|"back"|"end", pkg?:string, layout?:string, spot?:string}
//          (the scene on screen changed on the device itself: a tap opened a layout, it went back, a
//          scene ran out. pkg and layout: what is up now, neither: no scene, the creature is back; spot:
//          the one that opened it. room.ts also follows the screen with it, core/screen.ts)
//   button {id:"accept"|"reject"}
//   answer {job, index:int|null, label?:string, reason?:"timeout"|"cancelled"}
//          (the human's tap on a molty_ask; also checked against the job's options in room.ts)
//   creature {id, name, about?, gestures, moods}
//          (the creature on screen, on each welcome and each change: kept per device and handed to
//          agents as `device.creature` on utterances, `relay.creature` in status, and a `creature` event
//          when it changes; see creatureData)
// Everything else (say.done, link_code, ...) stays internal to the relay.

import { GESTURE_INPUTS, MAX_ASK_LABEL, MAX_ASK_OPTIONS, PKG_ID_RE } from "./commands.js";
import { DRAFT_DROPPED_EVENT, DRAFT_USED_EVENT, PARTIAL_EVENT } from "./partials.js";
import { isRecord, type JsonObject } from "./protocol.js";
import { LAYOUT_RE } from "./screen.js";

export interface AgentEvent {
  /** "e<seq>", monotonically increasing per device. */
  id: string;
  type: string;
  /** Relay receive time, unix ms. */
  ts: number;
  data: JsonObject;
  /** Internal (never returned): the one agent token hash this event is for (draft.used / draft.dropped). Absent = every agent. */
  to?: string;
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
  // input: what woke Molty when it was not a shake (a gesture bound to talk / hold); hold: a button held to talk
  shake: (d) =>
    num(d.peak_g, 0, 64) && int(d.reversals, 0, 1000) && int(d.ms, 0, 600000)
      ? {
          peak_g: d.peak_g as number,
          reversals: d.reversals as number,
          ms: d.ms as number,
          ...((GESTURE_INPUTS as readonly unknown[]).includes(d.input) ? { input: d.input as string } : {}),
          ...(d.hold === true ? { hold: true } : {}),
        }
      : undefined,
  tap: (d) =>
    num(d.x, 0, 1) && num(d.y, 0, 1)
      ? {
          x: d.x as number,
          y: d.y as number,
          ...(spot(d.spot) ? { spot: d.spot } : {}),
          ...(typeof d.opened === "string" && LAYOUT_RE.test(d.opened) ? { opened: d.opened } : {}),
          ...(d.closed === true ? { closed: true } : {}),
          ...(typeof d.muted === "boolean" ? { muted: d.muted } : {}),
        }
      : undefined,
  double_tap: (d) => (num(d.x, 0, 1) && num(d.y, 0, 1) ? { x: d.x as number, y: d.y as number } : undefined),
  button: (d) => (d.id === "accept" || d.id === "reject" ? { id: d.id } : undefined),
  answer: answerShape,
  scene: sceneShape,
  ring: ringShape,
  rhythm: (d) =>
    (d.state === "playing" || d.state === "paused" || d.state === "stopped") && int(d.bpm, 1, 1000)
      ? { state: d.state, bpm: d.bpm as number, ...(typeof d.rhythm === "string" && RHYTHM_ID_RE.test(d.rhythm) ? { rhythm: d.rhythm } : {}) }
      : undefined,
};

const RHYTHM_ID_RE = /^[a-z0-9-]{1,24}$/;
/** Device `ring`: a scene's wheel settled: a number (value) or a choice (index, label), and the rhythm a pick chose (id). */
function ringShape(d: Record<string, unknown>): JsonObject | undefined {
  if (!spot(d.spot)) return undefined;
  if (num(d.value, -10000, 10000) && d.index === undefined) return { spot: d.spot, value: d.value as number };
  if (!int(d.index, 0, 31)) return undefined;
  return {
    spot: d.spot,
    index: d.index as number,
    ...(typeof d.label === "string" && /^[\x20-\x7e]{1,24}$/.test(d.label) ? { label: d.label } : {}),
    ...(typeof d.id === "string" && RHYTHM_ID_RE.test(d.id) ? { id: d.id } : {}),
  };
}

/** A scene spot's id, as the package names it (the firmware's PKG_STR_MAX). */
const SPOT_RE = /^[\w.:-]{1,40}$/;
const spot = (v: unknown): v is string => typeof v === "string" && SPOT_RE.test(v);

/** The device's own screen change. */
export const SCENE_EVENT = "scene";

/** Device `scene`: what is up now is a package and a layout, or neither; anything else is dropped. A bad spot goes, the event stays. */
function sceneShape(d: Record<string, unknown>): JsonObject | undefined {
  if (d.why !== "open" && d.why !== "back" && d.why !== "end") return undefined;
  if (d.pkg === undefined && d.layout === undefined) return { why: d.why };
  if (typeof d.pkg !== "string" || !PKG_ID_RE.test(d.pkg) || typeof d.layout !== "string" || !LAYOUT_RE.test(d.layout)) return undefined;
  return { why: d.why, pkg: d.pkg, layout: d.layout, ...(spot(d.spot) ? { spot: d.spot } : {}) };
}
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
 * `{turn_id, text, reason, ms:{speech, stt}, trigger:"shake"|"talk", device:{caps, mood, voice}}`.
 * Filterable like device events; a device can never send it.
 */
export const UTTERANCE_EVENT = "utterance";

/**
 * Relay-generated: the creature on Molty's screen changed (the device reported another one). Data:
 * the creature, as `device.creature` on an utterance. A device `creature` frame is read by room.ts.
 */
export const CREATURE_EVENT = "creature";

/**
 * Relay-generated: the human turned do not disturb on or off (the PWR button's double press,
 * the console). Data `{on}`. While on, agents speak, listen and ask only in reply to an
 * utterance (in_reply_to); anything else is refused `do_not_disturb`. A device `dnd` frame is
 * read by room.ts.
 */
export const DND_EVENT = "dnd";

/** Who Molty is right now: the creature package on screen, its look and what it plays (docs/CREATURES.md). */
export type DeviceCreature = {
  /** The package id ("" for an uploaded .molty). */
  id: string;
  name: string;
  /** How it looks, and one line per gesture and mood: what each looks like on this animal. */
  about?: { look: string; gestures: Record<string, string>; moods: Record<string, string> };
  /** The gestures (molty_act) and moods (molty_mood) it plays; absent for a .molty (every one). */
  gestures?: string[];
  moods?: string[];
};

export const CREATURE_LIMITS = { look: 240, line: 100, names: 32 };
const CR_ID_RE = /^[a-z0-9-]{0,32}$/;
const CR_NAME_RE = /^[A-Za-z0-9_-]{1,24}$/;
const words = (v: unknown, max: number): v is string => typeof v === "string" && v.length >= 1 && v.length <= max && /^[\x20-\x7e]+$/.test(v);
const names = (v: unknown): v is string[] => Array.isArray(v) && v.length <= CREATURE_LIMITS.names && v.every((n) => typeof n === "string" && CR_NAME_RE.test(n));
const lines = (v: unknown): Record<string, string> | undefined => {
  if (v === undefined) return {};
  if (!isRecord(v) || Object.keys(v).length > CREATURE_LIMITS.names) return undefined;
  for (const [k, l] of Object.entries(v)) if (!CR_NAME_RE.test(k) || !words(l, CREATURE_LIMITS.line)) return undefined;
  return v as Record<string, string>;
};

/** The device's `creature` frame, checked and bounded (every field the firmware's, nothing else), or undefined to drop it. */
export function creatureData(d: unknown): DeviceCreature | undefined {
  if (!isRecord(d) || typeof d.id !== "string" || !CR_ID_RE.test(d.id) || !words(d.name, 32)) return undefined;
  const out: DeviceCreature = { id: d.id, name: d.name };
  if (d.about !== undefined) {
    const a = d.about;
    if (!isRecord(a) || !words(a.look, CREATURE_LIMITS.look)) return undefined;
    const g = lines(a.gestures), m = lines(a.moods);
    if (!g || !m) return undefined;
    out.about = { look: a.look, gestures: g, moods: m };
  }
  for (const k of ["gestures", "moods"] as const) {
    if (d[k] === undefined) continue;
    if (!names(d[k])) return undefined;
    out[k] = d[k] as string[];
  }
  return out;
}

/**
 * Types that only ride the inbox (pull and ride-along), never a webhook: they follow the screen and
 * ask for no answer, and an utterance says where it stands (`device.scene`).
 */
export const INBOX_ONLY_EVENTS: readonly string[] = [SCENE_EVENT];

/** Every type an agent can receive and filter on (webhook `events`): device events + the relay's own, less the inbox-only ones. */
export const AGENT_EVENT_TYPES = [...Object.keys(SHAPES).filter((t) => !INBOX_ONLY_EVENTS.includes(t)), UTTERANCE_EVENT, PARTIAL_EVENT, DRAFT_USED_EVENT, DRAFT_DROPPED_EVENT, CREATURE_EVENT, DND_EVENT];

/** Types a webhook gets only when its `events` names them (never by default): chatty, or nothing to answer (a wheel turned, a
 *  rhythm stopped: the human acts on Molty itself). Pull and ride-along carry them. */
export const OPT_IN_EVENTS: readonly string[] = [PARTIAL_EVENT, CREATURE_EVENT, "ring", "rhythm"];

/** What a webhook without `events` receives. */
export const DEFAULT_WEBHOOK_EVENTS = AGENT_EVENT_TYPES.filter((t) => !OPT_IN_EVENTS.includes(t));

/** Does a webhook with this `events` list (absent = the default) want `type`? */
export const webhookWants = (events: string[] | undefined, type: string) => !INBOX_ONLY_EVENTS.includes(type) && (events ? events.includes(type) : !OPT_IN_EVENTS.includes(type));

const SAMPLE_CREATURE: JsonObject = {
  id: "duck",
  name: "Duck",
  about: { look: "A round yellow duckling with an orange bill.", gestures: { nod: "bobs its head forward" }, moods: { idle: "keeps busy with small things" } },
  gestures: ["nod"],
  moods: ["idle"],
};

/** One representative event per type: webhook templates are dry-run against every type they will receive. */
export const SAMPLE_EVENTS: Record<string, AgentEvent> = {
  shake: { id: "e1", type: "shake", ts: 0, data: { peak_g: 2.4, reversals: 5, ms: 900 } },
  tap: { id: "e2", type: "tap", ts: 0, data: { x: 0.5, y: 0.5 } },
  double_tap: { id: "e9", type: "double_tap", ts: 0, data: { x: 0.5, y: 0.5 } },
  button: { id: "e3", type: "button", ts: 0, data: { id: "accept" } },
  answer: { id: "e4", type: "answer", ts: 0, data: { job: "molty-xxxxxxxxxx.j_sample00", index: 0, label: "yes" } },
  ring: { id: "e12", type: "ring", ts: 0, data: { spot: "tempo", value: 112 } },
  rhythm: { id: "e13", type: "rhythm", ts: 0, data: { state: "paused", rhythm: "chacarera", bpm: 104 } },
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
      device: { caps: ["say", "listen", "shake_talk"], mood: "thinking", voice: "box", creature: SAMPLE_CREATURE },
      // ms until Molty says its own filler unless an agent acts first (core/phrases.ts); absent when off
      filler_in_ms: 2300,
    },
  },
  [CREATURE_EVENT]: { id: "e10", type: CREATURE_EVENT, ts: 0, data: SAMPLE_CREATURE },
  [DND_EVENT]: { id: "e11", type: DND_EVENT, ts: 0, data: { on: true } },
  [PARTIAL_EVENT]: {
    id: "e6",
    type: PARTIAL_EVENT,
    ts: 0,
    data: { turn_id: "molty-xxxxxxxxxx.j_sample01", seq: 1, text_so_far: "what's on my calendar", new_text: "what's on my calendar", elapsed_ms: 3200 },
  },
  [DRAFT_USED_EVENT]: {
    id: "e7",
    type: DRAFT_USED_EVENT,
    ts: 0,
    data: { turn_id: "molty-xxxxxxxxxx.j_sample01", based_on: 1, opener: "Let me look.", new_words: 1 },
  },
  [DRAFT_DROPPED_EVENT]: {
    id: "e8",
    type: DRAFT_DROPPED_EVENT,
    ts: 0,
    data: { turn_id: "molty-xxxxxxxxxx.j_sample01", based_on: 1, reason: "new_words", new_words: 9 },
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
    case PARTIAL_EVENT:
      return `The human is still talking to Molty (context only, don't answer yet): "${String(d.text_so_far ?? "")}"`;
    case CREATURE_EVENT:
      return `Molty is now ${d.name}: speak as this creature (its look and moves are in the event)`;
    case DND_EVENT:
      return d.on === true
        ? "The human turned on do not disturb: speak, listen, ask or buzz only in reply to an utterance"
        : "The human turned off do not disturb";
    case DRAFT_USED_EVENT:
      return `Molty is speaking your opener "${String(d.opener ?? "")}": you have the turn, continue with in_reply_to`;
    case DRAFT_DROPPED_EVENT:
      return `Your draft opener was dropped (${String(d.reason ?? "")}): answer the utterance as usual`;
    case "tap":
      return typeof d.muted === "boolean" ? `The human ${d.muted ? "muted" : "brought back"} ${String(d.spot)} on Molty's rhythm` : "Molty's screen was tapped";
    case "ring":
      return d.id ? `The human chose ${String(d.id)} on Molty's wheel: it waits, stopped, for a tap in the middle` : d.label ? `The human turned Molty's ${String(d.spot)} wheel to ${String(d.label)}` : `The human turned Molty's ${String(d.spot)} wheel to ${String(d.value)}`;
    case "rhythm":
      return d.state === "stopped" ? "The human stopped the rhythm on Molty and left it" : d.state === "paused" ? "The human stopped the rhythm on Molty: it waits on the clock, choosing" : `The human started ${String(d.rhythm ?? "the rhythm")} on Molty`;
    case SCENE_EVENT:
      if (!d.layout) return "Molty's scene ended: the creature is back";
      if (d.why === "open") return `Molty opened ${d.layout} of ${d.pkg}${d.spot ? ` (the human tapped ${d.spot})` : ""}`;
      return d.why === "back" ? `Molty went back to ${d.layout} of ${d.pkg}` : `Molty's scene ended: ${d.layout} of ${d.pkg} is up now`;
    case "double_tap":
      return "Molty's screen was double-tapped";
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
