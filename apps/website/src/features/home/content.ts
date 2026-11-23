// Landing copy and data. Few words: the screens do the talking.
import { SITE_URL, SITE_DESCRIPTION, REPO_URL, REPO_BRANCH } from "../../config/site.mjs";

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

/** What an agent might say through Molty, read one word at a time on its screen; its
 *  question is answered by a tap on the screen (yes / no / idk), and the agent says one of these back. */
export const readerText = "Build passed. Three tests were flaky, so I reran them. Want me to open the pull request?";
export const readerAnswers = {
  yes: "Opening it now.",
  no: "Okay, leaving it.",
  idk: "Okay, I'll leave it for you to check.",
};

export const bridgeFlow = "agent ──MCP · REST──▶ relay ◀──WSS── molty";

export const pairingMessage = `Hi! Please connect yourself to my Molty.

Skill: ${REPO_URL}/tree/${REPO_BRANCH}/skills/molty
Device: molty-k7m2w9c4ya
Code: K7M2W9C4

I'll tap Molty to accept you.`;

export const boards = [
  {
    tag: "1.46",
    board: "Waveshare ESP32-S3-Touch-LCD-1.46",
    screen: "412×412 round · mic · speaker",
    status: "reference",
    px: 412,
  },
  {
    tag: "1.75C",
    board: "Waveshare ESP32-S3-Touch-AMOLED-1.75C",
    screen: "466×466 AMOLED · mic · speaker",
    status: "experimental",
    px: 466,
  },
  {
    tag: "1.28",
    board: "Waveshare ESP32-S3-Touch-LCD-1.28",
    screen: "240×240 round · no audio",
    status: "experimental",
    px: 240,
  },
] as const;

/** The ink screen bake shows beside the boards: an idea, not a board. */
export const inkConcept = {
  tag: "ink",
  screen: "square e-paper, 1-bit",
  board: "an idea: no ink board yet",
  status: "concept",
};

/** How a skill that draws is made (docs/DISPLAY.md, synced to this slug). */
export const SKILLS_HREF = "/moltys/draw/";

/** Porting guide (docs/PORTING.md, synced to this slug). */
export const PORTING_HREF = "/getting-started/porting/";
