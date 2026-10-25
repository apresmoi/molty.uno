// Landing copy and data. Few words; the screen does the talking.
import type { CueStep } from "../../services/molty-stage";
import { HERO_MOLTYS } from "../moltys/catalog";
import { SITE_URL, SITE_DESCRIPTION, REPO_URL } from "../../config/site.mjs";

export const title = "Molty — a calm creature of characters for your desk";
export const description = SITE_DESCRIPTION;

export const ldJson = {
  "@context": "https://schema.org",
  "@type": "SoftwareSourceCode",
  name: "Molty",
  url: SITE_URL,
  codeRepository: REPO_URL,
  license: "https://opensource.org/licenses/MIT",
  description: SITE_DESCRIPTION,
  programmingLanguage: ["C", "TypeScript", "JavaScript", "Go"],
};

/** Hero screen: the fluid settles, says hello, then Moltys take turns emerging and dancing. */
export const heroIntro: CueStep[] = [
  { cue: { kind: "blob" }, seconds: 1.6 },
  { cue: { kind: "word", word: "hello" }, seconds: 1.8 },
];
export const heroLoop: CueStep[] = HERO_MOLTYS.flatMap((id) => [
  { cue: { kind: "molty", id }, seconds: 13 },
  { cue: { kind: "blob" }, seconds: 1.8 },
]);

export const pillars = [
  {
    glyph: "⠿",
    title: "A screen that flows",
    text: "One fluid of braille characters fills the round glass. It breathes, swirls when it thinks, and ripples when you touch it.",
  },
  {
    glyph: "⠶",
    title: "Moltys live in it",
    text: "Small creatures condense out of the fluid, dance, look around and melt back. Each one is a 1 KB file — swap them any time.",
  },
  {
    glyph: "⠒",
    title: "Your agent is the brain",
    text: "Molty has no AI of its own. Claude, Grok, OpenClaw — whatever you use — gets a body: moods, words, a voice, ears.",
  },
];

export const howItWorks = String.raw`
  your agent                   relay                       Molty
  Claude · Grok · OpenClaw     pairing · tokens            on your desk
  +---------------+   MCP    +------------------+  WSS   +--------------+
  |  "be happy"   | -------> | routes commands  | <----- | dials out,   |
  |  "say hi"     |   REST   | speech <-> text  | -----> | never opened |
  +---------------+          +------------------+        +--------------+
`.replace(/^\n|\n\s*$/g, "");

export const howNotes = [
  ["Molty dials out.", "Its local API is never exposed to the internet; agents reach it only through the relay."],
  ["Nothing pairs without you.", "A code condenses out of the fluid; you tap the glass to accept."],
  ["Or skip the cloud.", "The molty CLI and local MCP server talk to it directly over USB-C or Wi-Fi."],
] as const;

export const pairingMessage = `Hi! Please connect yourself to my Molty.

Instructions: https://<relay>/skill.md
Device: molty-k7m2w9c4ya
Code: K7M2W9C4

I'll tap Molty to accept you.`;

export const pairingSteps = [
  "Molty's setup console gives you this message with your own device and code.",
  "Paste it to your agent. The skill.md link teaches it the rest.",
  "Same code on Molty's screen? Tap the glass. Done.",
];

export const hardware = [
  ["board", "Waveshare ESP32-S3-Touch-LCD-1.46"],
  ["screen", "412×412 round, capacitive touch"],
  ["senses", "mic · speaker · IMU · battery charging"],
  ["chip", "ESP32-S3 · 16 MB flash · 8 MB PSRAM"],
] as const;

export const flashSteps = String.raw`$ make console firmware        # or download a release
# hold BOOT, plug in USB-C, release
$ cd firmware/build
$ python -m esptool --chip esp32s3 -p PORT write_flash "@flash_args"
# replug, then open http://10.13.37.1`;

export const repoTree = String.raw`firmware/   ESP-IDF: fluid, creature runtime, audio, touch, agent link
console/    the setup page, baked into the firmware
relay/      Cloudflare Worker: pairing, MCP + REST, voice
cli/        molty CLI + local MCP server (Go)
creatures/  the fluid engine, 26 Moltys, the .molty exporter`;
