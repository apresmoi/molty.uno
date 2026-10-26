// Landing copy and data. Few words: the fluid does the talking.
import { SITE_URL, SITE_DESCRIPTION, REPO_URL } from "../../config/site.mjs";

export const title = "molty.UNO — liquid ASCII creatures for any screen";
export const description = SITE_DESCRIPTION;

export const ldJson = {
  "@context": "https://schema.org",
  "@type": "SoftwareSourceCode",
  name: "molty.UNO",
  url: SITE_URL,
  codeRepository: REPO_URL,
  license: "https://opensource.org/licenses/MIT",
  description: SITE_DESCRIPTION,
  programmingLanguage: ["C", "TypeScript", "JavaScript", "Go"],
};

/** Colours the fluid gives each heading: the wordmark's own green, then the Molty beside it. */
export const WORDMARK = "#3ccf96";
export const FLUID_INK = "#9cb8ac";

/** The hero's Moltys take turns emerging (the duck and octopus wait for their sections). */
export const HERO_MOLTYS = ["cute-frog", "cute-dragon", "cute-jellyfish", "cute-bunny", "cute-kitten"];
export const GALLERY_FIRST = "cute-axolotl";
export const BRIDGE_MOLTY = { id: "cute-octopus", color: "#c6a8f2" };
export const BAKE_MOLTY = { id: "cute-duck", color: "#ffdf5a" };

/** What an agent might say through Molty, read one word at a time out of the fluid. */
export const readerText = "Build passed. Three tests were flaky, so I reran them. Want me to open the pull request?";

export const bridgeFlow = "agent ──MCP · REST──▶ relay ◀──WSS── molty";

export const pairingMessage = `Hi! Please connect yourself to my Molty.

Instructions: https://<relay>/skill.md
Device: molty-k7m2w9c4ya
Code: K7M2W9C4

I'll tap Molty to accept you.`;

export const boards = [
  {
    tag: "1.46",
    board: "Waveshare ESP32-S3-Touch-LCD-1.46",
    screen: "412×412 round · mic · speaker",
    status: "reference",
  },
  {
    tag: "1.28",
    board: "Waveshare ESP32-S3-Touch-LCD-1.28",
    screen: "240×240 round · no audio",
    status: "supported",
  },
] as const;

/** Porting guide (docs/PORTING.md, synced to this slug when present). */
export const PORTING_HREF = "/boards/porting/";
