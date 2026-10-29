// /build/: will my board work, the reference build's parts, community builds, the waitlist.
// The requirements restate docs/PORTING.md ("Requirements" and "Capabilities"), the single
// source of truth; scripts/tests/build.test.ts fails when the two disagree.
import { amazonSearch } from "../../config/affiliate.mjs";

export const title = "build your own — molty.UNO";
export const description =
  "Will your board run molty.UNO? The reference build's parts list (one €33 board, optional battery and vibration), community boards, and a waitlist if you'd rather get one built.";

/** The heading's colour in the fluid (the wordmark's green). */
export const BUILD_INK = "#3ccf96";

export const PORTING_HREF = "/getting-started/porting/";
export const SUBMIT_HREF = "/getting-started/porting/#submit-your-board";

export const intro = "One €33 board and a USB-C cable is a Molty. Or bring a screen you already have.";

export interface Requirement {
  /** the row of docs/PORTING.md's Requirements table */
  row: "Chip" | "Flash" | "PSRAM" | "Display";
  label: string;
  /** must appear in that row's "Needed" cell */
  need: string;
  note?: string;
}

export const requirements: Requirement[] = [
  { row: "Chip", label: "chip", need: "ESP32-S3", note: "C3, C6 and the classic ESP32: not yet" },
  { row: "Flash", label: "flash", need: "16 MB", note: "two firmware slots + the console and creature" },
  { row: "PSRAM", label: "PSRAM", need: "2 MB or more", note: "8 MB recommended" },
  { row: "Display", label: "screen", need: "any size", note: "a colour screen, round or rectangular" },
];

export interface NiceToHave {
  /** the capability in docs/PORTING.md's Capabilities table (and board.json `caps`) */
  cap: "touch" | "imu" | "speaker" | "mic" | "battery" | "usb_net";
  label: string;
  without: string;
}

export const niceToHave: NiceToHave[] = [
  { cap: "touch", label: "touch", without: "no ripples under your finger; pairing is accepted on a button" },
  { cap: "imu", label: "IMU", without: "no tilt: the fluid doesn't run downhill" },
  { cap: "speaker", label: "speaker", without: "your agent can't speak through Molty" },
  { cap: "mic", label: "mic", without: "your agent can't listen through Molty" },
  { cap: "battery", label: "battery", without: "lives on USB power" },
  { cap: "usb_net", label: "native USB-C", without: "Wi-Fi is set up over the serial cable instead" },
];

export interface PartLink {
  label: string;
  url: string;
  note?: string;
}

export interface Part {
  name: string;
  spec?: string;
  links?: PartLink[];
  warn?: string;
}

export interface PartGroup {
  id: string;
  title: string;
  tag?: string;
  note?: string;
  parts: Part[];
  /** ASCII wiring, with a plain-text description for screen readers */
  wiring?: { diagram: string; description: string; steps: string[] };
}

export const WAVESHARE_146 = "https://www.waveshare.com/esp32-s3-touch-lcd-1.46b.htm";

export const vibrationWiring = ` 3V3 ──────────────┬────────────┐
                   │            │
               ( motor )     1N4148   band end → 3V3
                   │            │
                   ├────────────┘
                   │
                ┌──C───────┐
 GPIO ─ 1kΩ ─┬──B  2N2222  │
             │  └──E───────┘
           10kΩ    │
             │     │
 GND ────────┴─────┴──────────`;

export const partGroups: PartGroup[] = [
  {
    id: "core",
    title: "core",
    parts: [
      {
        name: "Waveshare ESP32-S3-Touch-LCD-1.46, without cover glass",
        spec: "412×412 round touch screen · 16 MB flash · 8 MB PSRAM · speaker + mic · QMI8658 IMU",
        links: [
          { label: "waveshare", url: WAVESHARE_146, note: "cheaper, ships from China" },
          { label: "amazon.de", url: amazonSearch("ESP32-S3-Touch-LCD-1.46"), note: "~€33" },
        ],
      },
      {
        name: "USB-C data cable",
        spec: "a data cable: some only charge, and then no port ever shows up",
        links: [{ label: "amazon.de", url: amazonSearch("USB-C data cable") }],
      },
    ],
  },
  {
    id: "battery",
    title: "battery",
    tag: "optional",
    parts: [
      {
        name: "3.7 V LiPo, ~1000 mAh, 803040 (8×30×40 mm) or smaller",
        spec: "built-in protection circuit · PH1.25 / MX1.25 2-pin plug",
        links: [{ label: "amazon.de", url: amazonSearch("LiPo 3.7V 1000mAh 803040 PH1.25") }],
        warn: "Check polarity before plugging in: red must go to + on the board's silkscreen. Sellers wire these both ways.",
      },
    ],
  },
  {
    id: "vibration",
    title: "vibration",
    tag: "optional · untested · firmware support coming",
    note: "A buzz for taps and notifications. Nobody has built this yet: the parts and wiring are the plan, not a tested build.",
    parts: [
      {
        name: "1027 coin vibration motor",
        spec: "10×2.7 mm · 3 V · 80 mA",
        links: [{ label: "amazon.de", url: amazonSearch("1027 coin vibration motor 3V") }],
      },
      { name: "2N2222 NPN transistor", links: [{ label: "amazon.de", url: amazonSearch("2N2222 transistor") }] },
      { name: "1N4148 diode", links: [{ label: "amazon.de", url: amazonSearch("1N4148 diode") }] },
      {
        name: "1 kΩ + 10 kΩ resistors",
        spec: "any 1/4 W kit",
        links: [{ label: "amazon.de", url: amazonSearch("resistor kit 1/4W") }],
      },
    ],
    wiring: {
      diagram: vibrationWiring,
      description:
        "Vibration motor wiring: 3V3 to the motor, the motor to the 2N2222 collector; a 1N4148 across the motor with its band toward 3V3; a GPIO through 1 kΩ to the base; 10 kΩ from base to GND; emitter to GND.",
      steps: [
        "3V3 → motor → 2N2222 collector",
        "1N4148 across the motor, band to 3V3",
        "GPIO → 1 kΩ → base",
        "10 kΩ base → GND (keeps it off at boot)",
        "emitter → GND",
      ],
    },
  },
  {
    id: "case",
    title: "case",
    note: "No printable case yet: coming.",
    parts: [],
  },
];

export const waitlistCopy = {
  title: "want one built?",
  lede: "Leave your email and we'll write when there are Moltys to order, built and flashed.",
  label: "email",
  placeholder: "you@example.com",
  submit: "join the waitlist",
  sending: "sending…",
  done: "You're on the list. We'll write when there's one for you.",
  errors: {
    bad_email: "That doesn't look like an email address.",
    rate_limited: "Too many tries from here. Try again in an hour.",
    human_check_failed: "The human check didn't pass. Reload and try again.",
    default: "Couldn't reach the waitlist. Try again in a bit.",
  } as Record<string, string>,
  /** without JavaScript the page can't tell which error it was */
  formError: "That didn't go through. Check the address, or try again in a bit.",
  privacy: "We'll only email you about molty.UNO. Unsubscribe or ask us to delete your address anytime:",
};

/** ?waitlist=ok | ?waitlist=error&reason=… (the no-JavaScript form lands back with these). */
export function readWaitlistResult(search: string): { kind: "done" } | { kind: "error"; reason: string } | null {
  const q = new URLSearchParams(search);
  const v = q.get("waitlist");
  if (v === "ok") return { kind: "done" };
  if (v === "error") return { kind: "error", reason: q.get("reason") ?? "default" };
  return null;
}

export const waitlistMessage = (reason: string) => waitlistCopy.errors[reason] ?? waitlistCopy.errors.default!;
