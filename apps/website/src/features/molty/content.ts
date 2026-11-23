// The Molty on the site's screens: the cat (packages/cat, the first creature), the still that
// stands in for it, and what bridge's screen plays by itself. The packages
// are the repo's own files, fetched by the screen when it mounts (never bundled, never copied).
import catUrl from "@packages/cat/cat.json?url";
import scenesUrl from "@packages/astrology/astrology.json?url";
import catStill from "../../assets/molty/cat.webp?url";
import wordsUrl from "../../assets/molty/words.json?url";
import type { Program } from "../../services/molty-screen";

/** The creature every screen shows. */
export const CAT = { id: "cat", url: catUrl };
/** The scenes package (astrology): the chart, the globes. */
export const SCENES_URL = scenesUrl;
/** The page's words as a scenes package (config/words.mjs, scripts/words-package.mjs). */
export const WORDS_URL = wordsUrl;
/** A word's round window, px a side, before the page says its grain (MoltyWord then gives it as
 *  many as the page's dust has across it), and the most the renderer takes. */
export const WORD_PX = 504;
export const WORD_PX_MAX = 1024;
/** One frame of the cat as the firmware's renderer draws it (scripts/molty-still.mjs): shown
 *  without JavaScript, before the screen is live, and when the module cannot load. */
export const CAT_STILL = catStill;

/** bridge: the pairing question exactly as the device asks it (link_pair.c); a tap accepts. */
export const PAIR: Program = {
  kind: "ask",
  question: {
    title: "pair with claude?",
    detail: "check the code matches",
    code: "4821",
    options: ["accept", "reject"],
  },
  then: [{ mood: "happy" }, { act: "shake_head" }],
  after: 3.5,
  wait: 30,
  rest: 9,
};
