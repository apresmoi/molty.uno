// The browser side of identifying a board on a serial port (Web Serial) and handing the
// port to ESP Web Tools' installer with that board's manifest only. The decision itself
// is features/setup/match.ts; this file only gathers the evidence.
import { ESPLoader, Transport } from "esptool-js";
import { ImprovSerial } from "improv-wifi-serial-sdk/dist/serial";
import {
  boardFromImprovChip,
  identify,
  portFilters,
  psramFromCap,
  type BoardSpec,
  type Evidence,
  type Identification,
} from "../../features/setup/match";

/** The slice of Web Serial's SerialPort used here (DOM typings do not ship it yet). */
export interface Port {
  open(options: { baudRate: number; bufferSize?: number }): Promise<void>;
  close(): Promise<void>;
  getInfo(): { usbVendorId?: number; usbProductId?: number };
  readable: unknown;
  writable: unknown;
}

interface Serial {
  requestPort(options: { filters: Array<{ usbVendorId: number; usbProductId: number }> }): Promise<Port>;
}

export const serialSupported = (): boolean => typeof navigator !== "undefined" && "serial" in navigator;

export interface Found<B extends BoardSpec> {
  port: Port;
  evidence: Evidence;
  result: Identification<B>;
}

const quiet = { log: () => {}, error: () => {}, debug: () => {} };
const OPEN = { baudRate: 115200, bufferSize: 8192 };
/** ESP32-S3 native USB-Serial/JTAG: resetting it re-enumerates (and drops) the port */
const isUsbJtag = (usb: { vid: number }) => usb.vid === 0x303a;

/** Ask for a port (only known boards' USB IDs are listed), then identify what is on it:
 *  molty firmware names its board over Improv; anything else is fingerprinted in the ROM
 *  bootloader. Null when the user picked no port. */
export async function findBoard<B extends BoardSpec>(
  boards: B[],
  say: (msg: string) => void,
): Promise<Found<B> | null> {
  const serial = (navigator as unknown as { serial: Serial }).serial;
  let port: Port;
  try {
    port = await serial.requestPort({ filters: portFilters(boards) });
  } catch {
    return null; // cancelled, or no matching port
  }
  const info = port.getInfo();
  const usb = { vid: info.usbVendorId ?? 0, pid: info.usbProductId ?? 0 };

  say("Asking the device who it is…");
  let firmwareBoard: string | undefined;
  await port.open(OPEN);
  try {
    const client = new ImprovSerial(port as never, quiet);
    const improv = await client.initialize(3000); // closes itself on failure
    firmwareBoard = boardFromImprovChip(improv?.chipFamily);
    await client.close();
  } catch {
    /* no molty firmware answering: fingerprint below */
  }
  await port.close();
  if (firmwareBoard) {
    const evidence = { usb, firmwareBoard };
    return { port, evidence, result: identify(evidence, boards) };
  }

  say("Reading the chip in its ROM bootloader…");
  const evidence: Evidence = { usb };
  const transport = new Transport(port as never, false);
  const loader = new ESPLoader({ transport, baudrate: 115200, terminal: { clean() {}, writeLine() {}, write() {} } });
  try {
    await loader.main(); // resets into the ROM bootloader (DTR/RTS, or the USB-JTAG sequence)
    evidence.chip = loader.chip.CHIP_NAME;
    const chip = loader.chip as unknown as { getPsramCap?: (l: ESPLoader) => Promise<number> };
    evidence.psram = chip.getPsramCap ? psramFromCap(await chip.getPsramCap(loader)) : "unknown";
    evidence.flash = await loader.detectFlashSize();
  } catch {
    /* evidence stays partial: identify() refuses */
  }
  const result = identify(evidence, boards);
  try {
    // Matched: stay in the bootloader, the install comes next. Refused: back to its own
    // firmware, except on native USB where a reset drops the port (unplug/replug does it).
    if (!result.ok && !isUsbJtag(usb)) await loader.after("hard_reset");
  } finally {
    await transport.disconnect();
  }
  return { port, evidence, result };
}

/** ESP Web Tools' installer on an identified port, with that board's manifest only.
 *  The dialog comes from the package's self-contained browser bundle: its unbundled
 *  dist/install-dialog.js imports @material/web files that 2.5 no longer ships. The
 *  chunk name is fixed for the pinned 10.4.0; scripts/tests/setup-match.test.ts fails
 *  loudly when a version bump renames it. */
export async function openInstaller(port: Port, manifest: string): Promise<void> {
  await import("esp-web-tools/dist/web/install-dialog-im156JnI.js");
  await port.open(OPEN);
  const dialog = document.createElement("ewt-install-dialog") as HTMLElement & { port: Port; manifestPath: string };
  dialog.port = port;
  dialog.manifestPath = manifest;
  dialog.addEventListener("closed", () => void port.close().catch(() => {}), { once: true });
  document.body.appendChild(dialog);
}
