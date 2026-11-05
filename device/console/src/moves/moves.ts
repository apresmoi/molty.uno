// The creature's gestures and moods (Molty settings): played with POST /api/creature/state
// {act} or {mood}, the same setter an agent's act and mood reach; GET reports the one
// playing as `act` and `mood`. A package lists its own under creature_info.gestures and
// .moods when the firmware reports them; until then the firmware's whole vocabulary
// (creature/cute_act.h CUTE_ACTIONS, cute_spec.c CUTE_MOOD_NAMES), which every creature plays.

export const ACTS = ['wiggle', 'nod', 'shake_head', 'bounce', 'peek', 'glance_left', 'glance_right', 'blink',
  'wobble', 'stretch', 'look_up', 'yawn', 'inspect', 'paddle'];
export const MOODS = ['idle', 'listening', 'thinking', 'speaking', 'happy', 'sleepy', 'surprised'];

const names = (v: unknown, all: string[]) =>
  Array.isArray(v) && v.length && v.every((x) => typeof x === 'string') ? v as string[] : all;

/** The gestures and moods of the creature on the device, from GET /api/creature/state. */
export function creatureMoves(s: Record<string, unknown> | null) {
  const ci = (s?.creature_info ?? {}) as { gestures?: unknown; moods?: unknown };
  return { acts: names(ci.gestures, ACTS), moods: names(ci.moods, MOODS) };
}

/** A button label: "shake_head" reads "shake head". */
export const moveLabel = (n: string) => n.replace(/_/g, ' ');
