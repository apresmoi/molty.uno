// /setup copy and the shape of public/firmware/index.json (scripts/stage-firmware.mjs).
import type { BoardSpec, Evidence, Identification } from "./match";

export const title = "Set up Molty over USB";
export const description =
  "Install molty.UNO and give Molty Wi-Fi from the browser, over the USB cable. The page identifies the board and only offers its firmware.";

/** One board in public/firmware/index.json. */
export interface SetupBoard extends BoardSpec {
  title: string;
  status: string;
  display: { width: number; height: number; shape: string };
  caps: string[];
  serial: boolean;
  manifest: string;
  available: boolean;
}

export interface SetupIndex {
  source: string;
  version: string | null;
  boards: SetupBoard[];
}

export const FIRMWARE_INDEX = "/firmware/index.json";
export const PORTING = "/getting-started/porting/";

export const copy = {
  browser: "Needs Chrome or Edge on a desktop computer (Web Serial). Safari, Firefox and phones cannot do this.",
  privacy: "Your Wi-Fi password goes from this page over the USB cable to Molty and nowhere else: no server sees it.",
  steps: [
    "Plug Molty into this computer with a USB-C data cable.",
    "Connect, and choose the serial port. Only ports that look like a supported board are listed.",
    "The page works out which board it is (Molty shows “hi” when its firmware answers) and offers that board's firmware, nothing else.",
    "Install molty.UNO if it is not on the board yet (this erases the board), then Connect to Wi-Fi. Molty only joins 2.4 GHz networks.",
    "Visit Device opens Molty's own console over Wi-Fi (also at http://molty.local).",
  ],
  nativeUsb:
    "The Waveshare 1.46 has no serial port while molty.UNO runs (its USB-C is a network adapter: set Wi-Fi at http://10.13.37.1). To install, hold BOOT while plugging it in, then Connect.",
  connect: "Connect",
  again: "Try another device",
  noPort:
    "No supported device was picked. Use a data cable; the 1.46 shows a serial port only while BOOT is held as it is plugged in.",
  noIndex: "The firmware list is missing from this site build.",
  unsupported: "This browser cannot open serial ports. Use Chrome or Edge on a desktop computer.",
  recoverable:
    "Nothing here can brick a board: the chip's ROM bootloader always accepts a serial flash (hold BOOT while plugging in, or press RESET with BOOT held).",
  noFirmware: "This site has no firmware for that board yet.",
};

const usbText = (e: Evidence) =>
  e.usb ? `USB ${e.usb.vid.toString(16).padStart(4, "0")}:${e.usb.pid.toString(16).padStart(4, "0")}` : "";

/** The sentence under the Connect button after identification. */
export function verdict(r: Identification<SetupBoard>, e: Evidence): { ok: boolean; text: string } {
  if (r.ok)
    return {
      ok: true,
      text:
        r.via === "firmware"
          ? `${r.board.title}: its molty.UNO says so.`
          : `${r.board.title}: ${e.chip}, ${e.psram} PSRAM, ${e.flash} flash, ${usbText(e)}.`,
    };
  const found = [e.chip, e.psram && `${e.psram} PSRAM`, e.flash && `${e.flash} flash`, usbText(e)]
    .filter(Boolean)
    .join(", ");
  switch (r.reason) {
    case "unknown-firmware-board":
      return {
        ok: false,
        text: `This Molty runs firmware for “${e.firmwareBoard}”, a board this site has no firmware for. Nothing was changed.`,
      };
    case "ambiguous":
      return {
        ok: false,
        text: `This device (${found}) matches more than one board: ${r.candidates.map((b) => b.title).join(", ")}. Refusing to guess. Nothing was changed.`,
      };
    case "no-evidence":
      return {
        ok: false,
        text: `The chip did not answer in its ROM bootloader (${found || "nothing read"}). Hold BOOT, press RESET (or plug in with BOOT held) and try again. Nothing was changed.`,
      };
    default:
      return {
        ok: false,
        text: `This device (${found}) is not a supported board, so there is no molty.UNO for it here. Nothing was changed. Porting it is one folder of code: see the porting guide.`,
      };
  }
}
