// Which board is on this serial port? /setup only offers the firmware of the one board it
// identifies, so nobody can flash another board's image. Pure functions: no browser APIs,
// unit-tested by scripts/tests/setup-match.test.ts against the real board.json files.
//
// Evidence, strongest first:
//   1. molty firmware is running: Improv device info names the board (definitive).
//   2. anything else: the chip in download mode (esptool-js) — chip family, embedded PSRAM
//      size (eFuse), flash size (flash ID) — plus the port's USB vendor/product ID, matched
//      against each board.json `detect` + `usb`. Exactly one board must match.

export interface UsbId {
  vid: number;
  pid: number;
}

/** A board as /setup knows it (public/firmware/index.json, from board.json). */
export interface BoardSpec {
  name: string;
  usb: UsbId[];
  detect: { chip: string; psram: string; flash: string };
}

/** What the page learned about the device on the port. */
export interface Evidence {
  usb?: UsbId;
  /** board id reported by molty firmware over Improv */
  firmwareBoard?: string;
  /** from download mode: esptool-js chip name ("ESP32-S3"), PSRAM ("2MB" | "8MB" | "none"), flash ("16MB") */
  chip?: string;
  psram?: string;
  flash?: string;
}

export type Identification<B extends BoardSpec = BoardSpec> =
  | { ok: true; board: B; via: "firmware" | "fingerprint" }
  | { ok: false; reason: "unknown-firmware-board" | "no-evidence" | "no-match" | "ambiguous"; candidates: B[] };

/** "0x1a86" | 6790 → 6790 */
export const usbNumber = (v: string | number): number => (typeof v === "number" ? v : parseInt(v, 16));

const sameUsb = (a: UsbId, b: UsbId) => a.vid === b.vid && a.pid === b.pid;

/** Web Serial port filters: every USB ID a known board can show up as. */
export function portFilters(boards: BoardSpec[]): Array<{ usbVendorId: number; usbProductId: number }> {
  const seen = new Map<string, { usbVendorId: number; usbProductId: number }>();
  for (const b of boards)
    for (const u of b.usb) seen.set(`${u.vid}:${u.pid}`, { usbVendorId: u.vid, usbProductId: u.pid });
  return [...seen.values()];
}

/** The single board the evidence points at, or why there is none. */
export function identify<B extends BoardSpec>(e: Evidence, boards: B[]): Identification<B> {
  if (e.firmwareBoard) {
    const board = boards.find((b) => b.name === e.firmwareBoard);
    return board
      ? { ok: true, board, via: "firmware" }
      : { ok: false, reason: "unknown-firmware-board", candidates: [] };
  }
  if (!e.chip || !e.psram || !e.flash) return { ok: false, reason: "no-evidence", candidates: [] };
  const candidates = boards.filter(
    (b) =>
      b.detect.chip === e.chip &&
      b.detect.psram === e.psram &&
      b.detect.flash === e.flash &&
      (e.usb === undefined || b.usb.some((u) => sameUsb(u, e.usb!))),
  );
  if (candidates.length === 1) return { ok: true, board: candidates[0], via: "fingerprint" };
  return { ok: false, reason: candidates.length ? "ambiguous" : "no-match", candidates };
}

/** esptool-js ESP32-S3 eFuse PSRAM capacity (getPsramCap) → board.json `detect.psram` */
export const psramFromCap = (cap: number): string => ({ 0: "none", 1: "8MB", 2: "2MB" })[cap] ?? `unknown(${cap})`;

/** Improv device info "hardware" field: molty firmware sends "<board id>/esp32-s3". */
export const boardFromImprovChip = (chip: string | undefined): string | undefined => {
  const m = /^([a-z0-9][a-z0-9.-]*)\/esp32/.exec(chip ?? "");
  return m ? m[1] : undefined;
};
