// The Molty on the site's screens: the cat (packages/cat, the first creature), the still that
// stands in for it, and what the landing's two side screens play by themselves. The packages
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
/** A word's round window, px a side: the widest the renderer draws in high definition (its
 *  fine dust keeps a speck's place in 9 bits: under 512). */
export const WORD_PX = 504;
/** One frame of the cat as the firmware's renderer draws it (scripts/molty-still.mjs): shown
 *  without JavaScript, before the screen is live, and when the module cannot load. */
export const CAT_STILL = catStill;

/** bridge: the pairing question exactly as the device asks it (link_pair.c); a tap accepts. */
const PAIR: Program = {
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

/** What the landing's side screens play by themselves, by name; loaded when a screen asks for
 *  one (the chart's sample is its own small file). */
export const PROGRAMS = {
  pair: async () => PAIR,
  chart: async () => (await import("./chart")).CHART,
} satisfies Record<string, () => Promise<Program>>;

export type ProgramName = keyof typeof PROGRAMS;
