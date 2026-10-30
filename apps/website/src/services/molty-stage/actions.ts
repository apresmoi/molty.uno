// What a Molty can be asked to do on a page-fluid slot (the showcase's control path, read by
// pageFluid.ts): a mood (device/creatures/js/mood.js MOODS, held MOOD_HOLD seconds, then the dance
// goes on) or a glass beat (beatsOf(id): its signature beats, then the generic ones). Pure
// data + the attribute/event protocol; nothing here touches the fluid.
//
//   slot.dataset.fluidAct = actValue(action, n)   →  "beat:shell#3", "mood:happy#4"
//   slot.addEventListener(ACT_EVENT, (e: CustomEvent<ActDetail>) => …)   start / end
import { MOODS } from "@creatures/mood.js";
import { beatsOf } from "./beats/registry";
import { GENERIC } from "./beats/generic";

export type ActionKind = "mood" | "beat";

export interface MoltyAction {
  kind: ActionKind;
  name: string;
  /** Seconds it plays (a mood is held MOOD_HOLD). */
  len: number;
}

export interface MoltyActions {
  moods: MoltyAction[];
  /** Its own beats (beats/<id>.ts), whatever that file exports today. */
  own: MoltyAction[];
  /** The generic glass beats every Molty plays (eye, confused, bonk, press, peek). */
  glass: MoltyAction[];
}

/** Seconds a mood asked for is held before the dance resumes. */
export const MOOD_HOLD = 4;
/** The CustomEvent a slot dispatches when an asked action starts and when it ends (or is cut). */
export const ACT_EVENT = "fluid-act";

export interface ActDetail {
  /** The data-fluid-act value it answers. */
  act: string;
  kind: ActionKind;
  name: string;
  phase: "start" | "end";
}

/** Is this one of the mood engine's moods? */
export const isMood = (name: string) => MOODS.includes(name);

const GENERIC_NAMES = new Set(GENERIC.map((b) => b.name));

/** Every action of a Molty, grouped: moods, its own beats, the generic glass beats. */
export function actionsOf(id: string): MoltyActions {
  const beats = beatsOf(id).map((b) => ({ kind: "beat" as const, name: b.name, len: b.len }));
  return {
    moods: MOODS.map((name) => ({ kind: "mood" as const, name, len: MOOD_HOLD })),
    own: beats.filter((b) => !GENERIC_NAMES.has(b.name)),
    glass: beats.filter((b) => GENERIC_NAMES.has(b.name)),
  };
}

/** The data-fluid-act value for one ask; `n` makes a repeat of the same action a new value. */
export const actValue = (a: { kind: ActionKind; name: string }, n: number) => `${a.kind}:${a.name}#${n}`;

/** A data-fluid-act value back into its parts; anything malformed is null. */
export function parseAct(value: string): { key: string; kind: ActionKind; name: string } | null {
  const m = /^(mood|beat):([a-z][\w-]{0,31})(?:#\d{1,12})?$/i.exec(value);
  return m ? { key: value, kind: m[1] as ActionKind, name: m[2]! } : null;
}
