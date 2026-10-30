// The /setup board matcher (src/features/setup/match.ts) against the real board.json files:
// every board identifies as itself and only itself; anything unknown or ambiguous is refused.
// Run: pnpm test (node --test; Node 22 strips the types).
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, readdirSync, existsSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { identify, portFilters, usbNumber, psramFromCap, boardFromImprovChip, type BoardSpec } from "../../src/features/setup/match.ts";

const BOARDS = fileURLToPath(new URL("../../../firmware/boards/", import.meta.url));

function boards(): BoardSpec[] {
  const out: BoardSpec[] = [];
  const walk = (dir: string) => {
    for (const e of readdirSync(dir, { withFileTypes: true })) {
      if (!e.isDirectory() || e.name.startsWith("_")) continue;
      const sub = join(dir, e.name);
      if (existsSync(join(sub, "board.json"))) {
        const j = JSON.parse(readFileSync(join(sub, "board.json"), "utf8"));
        out.push({ name: j.name, detect: j.detect, usb: j.usb.map((u: { vid: string; pid: string }) => ({ vid: usbNumber(u.vid), pid: usbNumber(u.pid) })) });
      } else walk(sub);
    }
  };
  walk(BOARDS);
  return out;
}

const all = boards();

test("there are boards to test", () => {
  assert.ok(all.length >= 2, `found ${all.length}`);
});

test("each board's own fingerprint identifies it and only it", () => {
  for (const b of all)
    for (const usb of b.usb) {
      const r = identify({ usb, ...b.detect }, all);
      assert.ok(r.ok, `${b.name} via ${usb.vid}:${usb.pid}: ${!r.ok && r.reason}`);
      assert.equal(r.ok && r.board.name, b.name);
      assert.equal(r.ok && r.via, "fingerprint");
    }
});

test("molty firmware naming its board is definitive", () => {
  for (const b of all) {
    const r = identify({ firmwareBoard: b.name, usb: { vid: 0, pid: 0 } }, all);
    assert.ok(r.ok && r.board.name === b.name && r.via === "firmware");
  }
  assert.deepEqual(identify({ firmwareBoard: "acme-unknown-board" }, all), { ok: false, reason: "unknown-firmware-board", candidates: [] });
});

test("the 1.28 and the 1.46 never stand in for each other", () => {
  const b128 = all.find((b) => b.name === "waveshare-esp32-s3-touch-lcd-1.28")!;
  const b146 = all.find((b) => b.name === "waveshare-esp32-s3-touch-lcd-1.46")!;
  // the 1.28's chip behind the 1.46's USB ID, and the reverse: no board has that combination
  assert.deepEqual(identify({ usb: b146.usb[0], ...b128.detect }, all).ok, false);
  assert.deepEqual(identify({ usb: b128.usb[0], ...b146.detect }, all).ok, false);
});

test("unknown hardware is refused", () => {
  const usb = all[0].usb[0];
  for (const e of [
    { usb, chip: "ESP32", psram: "none", flash: "4MB" },
    { usb, chip: "ESP32-S3", psram: "none", flash: "16MB" }, // a module with no PSRAM
    { usb, chip: "ESP32-S3", psram: all[0].detect.psram, flash: "8MB" },
    { usb: { vid: 0x10c4, pid: 0xea60 }, ...all[0].detect }, // a CP2102 bridge no board has
  ]) {
    const r = identify(e, all);
    assert.ok(!r.ok && r.reason === "no-match", JSON.stringify(e));
  }
  assert.ok(!identify({ usb }, all).ok, "no fingerprint at all must be refused");
});

test("two boards with the same fingerprint are refused, not guessed", () => {
  const twin = { ...all[0], name: "someone-twin-board" };
  const r = identify({ usb: all[0].usb[0], ...all[0].detect }, [...all, twin]);
  assert.ok(!r.ok && r.reason === "ambiguous" && r.candidates.length === 2);
});

test("port filters cover every board USB ID once", () => {
  const f = portFilters(all);
  const ids = all.flatMap((b) => b.usb.map((u) => `${u.vid}:${u.pid}`));
  assert.equal(f.length, new Set(ids).size);
  for (const id of ids) assert.ok(f.some((x) => `${x.usbVendorId}:${x.usbProductId}` === id));
});

test("helpers", () => {
  assert.equal(psramFromCap(1), "8MB");
  assert.equal(psramFromCap(2), "2MB");
  assert.equal(psramFromCap(0), "none");
  assert.equal(boardFromImprovChip("waveshare-esp32-s3-touch-lcd-1.28/esp32-s3"), "waveshare-esp32-s3-touch-lcd-1.28");
  assert.equal(boardFromImprovChip("ESP32-S3"), undefined); // molty firmware before board ids, or another firmware
  assert.equal(usbNumber("0x1a86"), 0x1a86);
});

test("the ESP Web Tools installer chunk /setup imports exists in the pinned version", () => {
  const chunk = fileURLToPath(new URL("../../node_modules/esp-web-tools/dist/web/install-dialog-im156JnI.js", import.meta.url));
  assert.ok(existsSync(chunk), "esp-web-tools was bumped: update the chunk name in src/services/serial-setup/identify.ts and src/env.d.ts");
  assert.match(readFileSync(chunk, "utf8"), /customElements\.define\("ewt-install-dialog"/);
});

