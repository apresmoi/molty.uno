// The page's water and words against the firmware they follow. The dust between the screens is
// page-side (a background tile and a wave layer), so its numbers are pinned to the firmware's
// here: a change there fails this until the page follows. The words are a scenes package the
// firmware's renderer must take, each within what it draws in high definition.
// Run: pnpm test (node --test; Node 22 strips the types).
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import sharp from "sharp";
import { FINE, TILE, WAVE, hashU, ign } from "../../src/config/water.mjs";
import { WORDS, wordsPackage } from "../../src/config/words.mjs";
import { TILES, renderTile, tilePath } from "../dust-tile.mjs";
import { MoltyCore } from "../../src/services/molty-screen/core.ts";

const repo = (path: string) => readFileSync(fileURLToPath(new URL(`../../../../${path}`, import.meta.url)));
const define = (source: string, name: string) => {
  const m = new RegExp(`#define\\s+${name}\\s+([-0-9.]+)f?\\b`).exec(source);
  assert.ok(m, `${name} is defined`);
  return Number(m[1]);
};

test("the page's dust has the firmware's numbers (fluid_fine.h)", () => {
  const h = repo("device/firmware/main/fluid/fluid_fine.h").toString();
  assert.equal(FINE.AMBIENT, define(h, "FINE_AMBIENT"));
  assert.equal(FINE.PEAK, define(h, "FINE_PEAK"));
  assert.equal(FINE.LEVEL_FAR, define(h, "FINE_LEVEL_FAR"));
  assert.equal(FINE.LIGHT_FLOOR, define(h, "FINE_LIGHT_FLOOR"));
  assert.deepEqual(FINE.WATER, ["R", "G", "B"].map((c) => define(h, `FINE_WATER_${c}`)));
});

test("... and its threshold is the firmware's noise (fluid_fine.c ign)", () => {
  const c = repo("device/firmware/main/fluid/fluid_fine.c").toString();
  assert.match(c, /0\.06711056f \* \(float\)x \+ 0\.00583715f \* \(float\)y, v = 52\.9829189f \* \(a - floorf\(a\)\)/);
  assert.ok(Math.abs(ign(3, 7) - ((52.9829189 * ((0.06711056 * 3 + 0.00583715 * 7) % 1)) % 1)) < 1e-4);
});

test("the page's wave has the firmware's speed and decay (fluid_sim.c)", () => {
  const c = repo("device/firmware/main/fluid/fluid_sim.c").toString();
  assert.match(c, new RegExp(`p->c2 = ${WAVE.C2}f;`));
  assert.match(c, new RegExp(`p->leak = ${WAVE.LEAK}f;`));
  assert.match(c, new RegExp(`${WAVE.CELLS}\\.f \\* \\(float\\)stage / FLUID_STAGE_REF`));
  assert.equal(WAVE.STAGE, define(repo("device/firmware/main/fluid/fluid_sim.h").toString(), "FLUID_STAGE_REF"));
});

test("... and its loose noise is the firmware's hash (fluid_fine.c hash_u)", () => {
  const c = repo("device/firmware/main/fluid/fluid_fine.c").toString();
  assert.match(c, /\(uint32_t\)a \* 0x27d4eb2du \^ \(uint32_t\)b \* 0x165667b1u \^ c \* 0x9e3779b1u;\s*h \^= h >> 15; h \*= 0x85ebca6bu; h \^= h >> 13;/);
  assert.ok(hashU(3, 7, 1) >= 0 && hashU(3, 7, 1) < 1 && hashU(3, 7, 1) !== hashU(7, 3, 1));
});

test("the dust tiles are what the rule draws (node scripts/dust-tile.mjs)", async () => {
  for (const tile of TILES) {
    const png = await sharp(tilePath(tile)).ensureAlpha().raw().toBuffer({ resolveWithObject: true });
    assert.equal(png.info.width, TILE);
    assert.ok(png.data.equals(renderTile(tile)), `src/assets/dust/${tile.kind}-${tile.theme}.png is stale`);
  }
});

test("the words are a package the renderer takes, each drawn in high definition", async () => {
  const json = JSON.stringify(wordsPackage());
  assert.equal(repo("apps/website/src/assets/molty/words.json").toString().trim(), json, "words.json is stale: node scripts/words-package.mjs");
  const core = await MoltyCore.create(await WebAssembly.compile(repo("device/firmware/web/built/molty.wasm")));
  assert.ok(core.pack(Buffer.from(json)));
  for (const [name] of WORDS) {
    const layout = wordsPackage().layouts[name]!;
    // a line is a shape a segment; the renderer draws at most 256 in high definition
    const shapes = layout.items.reduce((n: number, it: { pts?: unknown[] }) => n + (it.pts ? it.pts.length - 1 : 1), 0);
    assert.ok(shapes <= 256, `${name}: ${shapes} shapes`);
  }
});
