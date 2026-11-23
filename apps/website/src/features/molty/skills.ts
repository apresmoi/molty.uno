// skills: what the two real skills put on Molty's screen (skills/molty-astro, skills/molty), one
// screen each, as the device plays it: the astrology package's scenes from the firmware's own
// sample commands (the ones its host tests play), and the molty skill's question and words. Pure
// data: the page's frontmatter builds the wall from the commands (the file never reaches the
// browser). scripts/tests/skills.test.ts wants every one to be a skill, a script and a screen
// that exist.
import type { SceneCommand } from "./actions";
import type { Program } from "../../services/molty-screen";

export interface SkillScreen {
  /** What it is called on the wall, and the skill (a folder in skills/) that shows it. */
  name: string;
  skill: "molty-astro" | "molty";
  /** The skill's script that asks for it (in skills/<skill>/scripts/). */
  script: string;
  about: string;
  /** A scene it holds up from its first picture, and what it does by itself. */
  scene?: { layout: string; params: Record<string, unknown> };
  play?: Program;
  /** Left off a phone's wall (it keeps five). */
  wide?: boolean;
}

/** The scenes a skill sends at once, as the sample commands have them: the first that plays
 *  `layout` and those queued right behind it. */
export function sampleQueue(commands: string, layout: string): SceneCommand["args"][] {
  const lines = commands
    .split("\n")
    .filter((l) => l.trim())
    .map((l) => JSON.parse(l) as SceneCommand);
  const first = lines.findIndex((c) => c.name === "scene.play" && c.args.layout === layout);
  if (first < 0) throw new Error(`no sample command plays the layout "${layout}"`);
  const end = lines.findIndex((c, i) => i > first && c.name !== "scene.queue");
  return lines.slice(first, end < 0 ? undefined : end).map((c) => c.args);
}

/** The wall, in order: the chart first (big), then the rest. `commands`: the firmware's sample
 *  commands (device/firmware/test/host/fixtures/astrology-commands.jsonl). */
export function skillScreens(commands: string): SkillScreen[] {
  const scene = (layout: string) => ({ layout, params: sampleQueue(commands, layout)[0]!.params });
  /** The scenes a skill sends for `layout`, played (`after` s in) and again every `every` s. */
  const scenes = (layout: string, after: number, every: number): Program => ({
    kind: "scenes",
    scenes: sampleQueue(commands, layout).map((a) => ({ layout: a.layout, params: a.params, ttl: a.ttl_s })),
    after,
    every,
  });
  return [
    {
      name: "chart",
      skill: "molty-astro",
      script: "astro.mjs",
      about: "the planets on the zodiac wheel, joined by their tightest aspects; tap one to read it",
      scene: scene("chart"),
    },
    {
      name: "sign close-up",
      skill: "molty-astro",
      script: "astro.mjs",
      about: "a sign big, with its planets on its dial and its reading",
      scene: scene("sign-closeup"),
    },
    {
      name: "tonight",
      skill: "molty-astro",
      script: "astro.mjs",
      about: "the moon as it is lit, then each planet as a globe",
      // the moon, Mercury to Saturn and the moon's sign, a few seconds each, then the cat a while
      play: scenes("globe", 0.3, 26),
    },
    {
      name: "signs",
      skill: "molty-astro",
      script: "astro.mjs",
      about: "the twelve signs",
      // all twelve, then each alone (36 s), the cat a while, and again
      scene: scene("signs"),
      play: scenes("signs", 40, 40),
      wide: true,
    },
    {
      name: "ask",
      skill: "molty",
      script: "ask.sh",
      about: "a question with answers you tap",
      play: {
        kind: "ask",
        question: { title: "feed the cat?", options: ["yes", "no"] },
        then: [{ act: "eat" }, { mood: "surprised" }],
        after: 0.5,
        wait: 20,
        rest: 4,
      },
    },
    {
      name: "talk",
      skill: "molty",
      script: "talk.sh",
      about: "its words, one at a time, on the glass",
      play: { kind: "say", text: "Build passed. Want me to open the pull request?", wpm: 220, after: 0.5, rest: 3 },
    },
  ];
}
