// The demo's controls (/demo/): data only. What each control calls is the module's own entry
// (services/molty-screen sim.ts, core.ts); the words and numbers below are copied from where the
// device or its agents take them, each with its source.
import type { DeviceQuestion } from "../../services/molty-screen";

/** The moods an agent can set (mole_fluid.h: POST /api/creature/state {mood}; `working` is the
 *  agent's busy mood, fd_agent_work). The cat's own come from its package. */
export const AGENT_MOODS = ["working"];

/** A say's pace (POST /api/creature/state {wpm}): the reader's default and its range. */
export const WPM = { def: 220, lo: 80, hi: 600 };

/** The sentence the say box starts with. */
export const SAY_TEXT = "Hello! The moon is a waxing crescent tonight, low in the west after sunset.";

let jobs = 0;
const job = () => `sim-${++jobs}`;

/** The questions on the menu. `pair` is the pairing question as link_pair.c builds it; `install`
 *  as pkg_cmd.c builds it ("install <name>?", its detail, its two answers in its two colours). */
export const QUESTIONS: Record<string, () => DeviceQuestion> = {
  "yes or no": () => ({ kind: "ask", job: job(), title: "feed the cat?", options: ["yes", "no"] }),
  three: () => ({ kind: "ask", job: job(), title: "which one?", options: ["tea", "coffee", "water"] }),
  five: () => ({
    kind: "ask",
    job: job(),
    title: "how was today?",
    options: ["great", "good", "ok", "meh", "bad"],
  }),
  "pairing code": () => ({
    kind: "pair",
    job: job(),
    title: "pair with claude?",
    detail: "check the code matches",
    code: "4821",
    options: ["accept", "reject"],
  }),
  install: () => ({
    kind: "install",
    job: job(),
    id: "astrology",
    version: "1.7.1",
    ms: 3000,
    title: "install astrology?",
    detail: "a new package for molty",
    options: ["install", "not now"],
    colors: ["#3ccf96", "#f1f3f5"],
  }),
};

/** The progress ring's steps for "grow". */
export const PROGRESS_STEP = 20;

/** The chart's default moment: Juan's (the skill's own sample, astrology-commands.jsonl). */
export const CHART_DATE = "1991-06-06T14:45";

/** How long the skill holds each screen (skills/molty-astro/scripts/astro.mjs DUR and ttl(): a
 *  screen's own length plus 5 s, 60 s for one without). */
const DUR: Record<string, number> = {
  globe: 2.5,
  saturn: 2.5,
  sign: 4,
  signs: 36,
  aspect: 5,
  chart: 0,
  transit: 6,
  occultation: 6,
  appulse: 6,
  "solar-eclipse": 6,
  "lunar-eclipse": 6,
};
export const sceneTtl = (layout: string) => (DUR[layout] ? Math.ceil(DUR[layout]) + 5 : 60);

/** The skill's aspect kinds (astro.mjs ASPECT_KINDS). */
export const ASPECT_KINDS = ["conjunction", "sextile", "square", "trine", "opposition"];

/** The lab: how each of the firmware's experiment switches is shown (web_device.c LAB has the
 *  switches; one the module does not have is not shown, a switch with no line here shows as a
 *  number). One line a switch. */
export const LAB_LABELS: Record<string, { label: string; values: Array<[number, string]> }> = {
  scene_dust: {
    label: "Dust in scenes",
    values: [
      [0, "Fine specks (today)"],
      [1, "Cat's dots"],
      [2, "Cat's dots, lighter sending"],
      [3, "Fine specks + hard dots on the water"],
    ],
  },
  scene_hard_pct: {
    label: "How many hard dots (with the mix)",
    values: [
      [1, "sparse (1%)"],
      [2, "2%"],
      [3, "3%"],
      [5, "5%"],
      [8, "dense (8%)"],
    ],
  },
};
