// What the cat can be asked to do on a page's screen, read from its own package
// (packages/cat/cat.json: the moods and gestures it names, and how it describes each) and from
// the firmware's sample commands. Pure data: a page's frontmatter builds the lists (the package
// file never reaches the browser bundle), a stage plays them (hooks/useMoltyStage).

/** The parts of a creature package this reads (docs/PACKAGES.md section 5). */
export interface CreaturePackage {
  creature: {
    about: { gestures: Record<string, string>; moods: Record<string, string> };
    gestures: Record<string, { dur?: number }>;
    moods: Record<string, unknown>;
  };
}

/** One line of the firmware's sample commands (device/firmware/test/host/fixtures). */
export interface SceneCommand {
  name: string;
  args: { layout: string; params: Record<string, unknown>; ttl_s: number };
}

export type MoltyAction =
  /** POST /api/creature/state { mood }, held a few seconds, then idle again. */
  | { kind: "mood"; name: string; about: string }
  /** ... { act }: one gesture, once. */
  | { kind: "act"; name: string; about: string }
  /** The device was shaken. */
  | { kind: "shake"; name: string; about: string }
  /** A listen records: the thin red ring. */
  | { kind: "listen"; name: string; about: string }
  /** A question on the rim, answered by a tap. */
  | { kind: "ask"; name: string; about: string }
  /** A package's scene. */
  | { kind: "scene"; name: string; about: string; layout: string; params: Record<string, unknown>; ttl: number };

export interface ActionGroup {
  label: string;
  actions: MoltyAction[];
}

/** Seconds a mood asked for is held before the cat is idle again (hold_s). */
export const MOOD_HOLD = 6;
/** Seconds the recording ring stays up when asked for. */
export const LISTEN_SECONDS = 7;
/** Seconds a question waits for a tap before it lets go. */
export const ASK_SECONDS = 20;

/** A voice for the recording ring to move with (no microphone is asked for): syllables about
 *  four a second, in phrases with pauses between, 0..1. The firmware's own made-up voice
 *  (mole_fluid.c diag_voice). */
export function madeUpVoice(seconds: number): number {
  const syllable = 0.5 + 0.5 * Math.sin(seconds * 2 * Math.PI * 4);
  const phrase = Math.sin(seconds * 2 * Math.PI * 0.35);
  return phrase > 0 ? Math.min(1, (0.35 + 0.65 * syllable) * (0.6 + 0.4 * phrase)) : 0;
}

/** The question the stage asks, and what the cat does for each answer. */
export const STAGE_QUESTION = {
  question: { title: "feed the cat?", options: ["yes", "no"] },
  then: [{ act: "eat" }, { act: "shake_head" }] as Array<Record<string, unknown>>,
};

const GLASS: MoltyAction[] = [
  { kind: "listen", name: "listen", about: "a thin red ring while Molty records; it moves with your voice" },
  { kind: "ask", name: "ask", about: "a question on the rim; tap an answer on the glass" },
];

const moods = (pkg: CreaturePackage): MoltyAction[] =>
  Object.keys(pkg.creature.moods).map((name) => ({ kind: "mood", name, about: pkg.creature.about.moods[name] ?? "" }));
/** A gesture the package gives a length is its own; the rest every creature plays its own way. */
const gestures = (pkg: CreaturePackage, own: boolean): MoltyAction[] =>
  Object.entries(pkg.creature.gestures)
    .filter(([name, g]) => (g.dur !== undefined) === own && name !== "shaken")
    .map(([name]) => ({ kind: "act", name, about: pkg.creature.about.gestures[name] ?? "" }));
const shake = (pkg: CreaturePackage): MoltyAction => ({
  kind: "shake",
  name: "shake",
  about: pkg.creature.about.gestures.shaken ?? "",
});

/** The landing's few: its moods, its own gestures, the ring and a question. */
export function landingActions(pkg: CreaturePackage): ActionGroup[] {
  return [
    { label: "moods", actions: moods(pkg) },
    { label: "its own", actions: [...gestures(pkg, true), shake(pkg)] },
    { label: "on the glass", actions: GLASS },
  ];
}

/** Everything: also the gestures every creature shares, and scenes from a package. */
export function showcaseActions(pkg: CreaturePackage, scenes: MoltyAction[]): ActionGroup[] {
  return [
    { label: "moods", actions: moods(pkg) },
    { label: "its own", actions: [...gestures(pkg, true), shake(pkg)] },
    { label: "gestures", actions: gestures(pkg, false) },
    { label: "on the glass", actions: GLASS },
    { label: "scenes", actions: scenes },
  ];
}

/** The sample commands (one JSON object a line) that play `layout`, the first of each. */
export function sampleScene(commands: string, layout: string): SceneCommand["args"] {
  for (const line of commands.split("\n")) {
    if (!line.trim()) continue;
    const c = JSON.parse(line) as SceneCommand;
    if (c.name.startsWith("scene.") && c.args.layout === layout) return c.args;
  }
  throw new Error(`no sample command plays the layout "${layout}"`);
}

/** The scenes the showcase offers, from the astrology package's sample commands. */
export function showcaseScenes(commands: string): MoltyAction[] {
  const scene = (name: string, layout: string, about: string): MoltyAction => {
    const args = sampleScene(commands, layout);
    return { kind: "scene", name, about, layout, params: args.params, ttl: 40 };
  };
  return [
    scene("chart", "chart", "a birth chart: tap a planet, a sign or a line and Molty reads it"),
    scene("moon", "globe", "the moon as it is lit tonight"),
    scene("saturn", "saturn", "Saturn and its rings"),
  ];
}
