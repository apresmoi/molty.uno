// The Moltys showcase (/showcase/): every Molty and everything it can do, on one big stage.
export const title = "moltys showcase — molty.UNO";
export const description =
  "All 26 Moltys and their actions: moods, their own beats and the glass beats, played live in the fluid.";

export const SHOWCASE_FIRST = "cute-axolotl";
/** The heading's colour until the stage's Molty tints it. */
export const SHOWCASE_INK = "#3ccf96";

/** Autoplay: seconds between actions, and how many actions before the next Molty. */
export const AUTOPLAY_GAP = 1.2;
export const AUTOPLAY_PER_MOLTY = 4;

export interface ShowcaseState {
  molty: string | null;
  action: string | null;
}

const ID = /^[a-z][\w-]{0,47}$/i;

/** ?m=<catalog id>&a=<action name>: anything malformed is ignored (null). */
export function readShowcaseUrl(search: string): ShowcaseState {
  const q = new URLSearchParams(search);
  const m = q.get("m"),
    a = q.get("a");
  return { molty: m && ID.test(m) ? m : null, action: a && ID.test(a) ? a : null };
}

/** The same search string with m / a set (or a removed); every other parameter is kept. */
export function writeShowcaseUrl(search: string, state: ShowcaseState): string {
  const q = new URLSearchParams(search);
  for (const [key, value] of [
    ["m", state.molty],
    ["a", state.action],
  ] as const)
    if (value) q.set(key, value);
    else q.delete(key);
  const s = q.toString();
  return s ? `?${s}` : "";
}
