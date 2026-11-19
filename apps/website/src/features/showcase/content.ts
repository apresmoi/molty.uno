// The showcase (/showcase/): the cat and everything it can do, on one big stage.
export const title = "moltys showcase — molty.UNO";
export const description =
  "The cat, the first Molty, drawn live by the device's own code: its moods, its gestures, the recording ring, a question and scenes.";

/** The heading's colour: the cat's own body colour (packages/cat, palette.body). */
export const SHOWCASE_INK = "#a99be8";

/** Autoplay: seconds between actions. */
export const AUTOPLAY_GAP = 1.2;

const NAME = /^[a-z][\w-]{0,31}$/i;

/** ?a=<action name>: anything malformed is ignored (null). */
export function readShowcaseUrl(search: string): string | null {
  const a = new URLSearchParams(search).get("a");
  return a && NAME.test(a) ? a : null;
}

/** The same search string with a set (or removed); every other parameter is kept. */
export function writeShowcaseUrl(search: string, action: string | null): string {
  const q = new URLSearchParams(search);
  if (action) q.set("a", action);
  else q.delete("a");
  const s = q.toString();
  return s ? `?${s}` : "";
}
