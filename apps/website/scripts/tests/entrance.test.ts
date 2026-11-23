// The hero's entrance (src/services/molty-screen/entrance.ts) on the committed module, stepped as
// the page steps it: the screen's Molty and the window beside it, the page's 60 frames a second.
// On every window width the cat walks into the glass and to its place as one cat: the window's
// cat starts beyond the window's edge, is the screen's to the hand-over, never jumps, never goes blank, and it ends at its place.
// Run: pnpm test (node --test; Node 22 strips the types).
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { MoltyCore, PANEL } from "../../src/services/molty-screen/core.ts";
import { ENTER, Entrance } from "../../src/services/molty-screen/entrance.ts";

const ROOT = fileURLToPath(new URL("../../../../", import.meta.url));
const read = (path: string) => readFileSync(join(ROOT, path));
const module = await WebAssembly.compile(read("device/firmware/web/built/molty.wasm"));
const cat = read("packages/cat/cat.json");
const PAGE_MS = 1000 / 60;

/** World units from the screen's middle to the window's right edge (screen.ts), measured on the
 *  home page in a headless browser. A wide, short window starts beyond the measured table. */
// prettier-ignore
const WINDOWS: Array<[string, number]> = [
  ["390 x 844", 1.01], ["768 x 1024", 1.49], ["1024 x 768", 0.96], ["1280 x 800", 1.2], ["1440 x 900", 1.52],
  ["1680 x 1050", 1.98], ["1920 x 1080", 2.48], ["2560 x 1440", 3.82], ["1440 x 500 (short)", 2.04], ["2560 x 600 (short)", 5.55],
  ["3440 x 600 (short)", 8.47],
];
/** The most the cat's place moves in a page frame: its pace (creature_clip.c CC_PACE, 0.6 a
 *  second) on the sped-up clock, and the pull home while it attends (web_cat.c). */
const STEP = 0.06;

async function molty(seed: number) {
  const core = await MoltyCore.create(module);
  assert.ok(core.creature("cat", cat) && core.start(seed, false, PANEL));
  return core;
}
const inGlass = (x: number, y: number) => (x + 0.5 - PANEL / 2) ** 2 + (y + 0.5 - PANEL / 2) ** 2 <= (PANEL / 2) ** 2;
/** Lit pixels of a cat picture carried `shift` px to the right that fall inside the glass. */
function litInGlass(px: Uint8ClampedArray, shift: number) {
  let n = 0;
  for (let y = 0; y < PANEL; y++)
    for (let x = Math.max(0, shift); x < Math.min(PANEL, PANEL + shift); x++)
      if (px[(y * PANEL + x - shift) * 4 + 3] && inGlass(x, y)) n++;
  return n;
}
/** The cat picture's leftmost lit column. */
function nose(px: Uint8ClampedArray) {
  let min = PANEL;
  for (let i = 3; i < px.length; i += 4) if (px[i]) min = Math.min(min, (i >> 2) % PANEL);
  return min;
}
/** Cells the screen blanks for its own cat (lit there, black in its world) with nothing of the
 *  window's cat drawn over them. */
function blankCells(core: MoltyCore, cat: Uint8ClampedArray, shift: number) {
  const own = core.pixels(), world = core.worldPixels();
  const W = core.gridW, H = core.gridH;
  let n = 0;
  for (let j = 0; j < H; j++)
    for (let i = 0; i < W; i++) {
      let hidden = false, drawn = false;
      for (let y = Math.floor((j * PANEL) / H); y < Math.floor(((j + 1) * PANEL) / H); y++)
        for (let x = Math.floor((i * PANEL) / W); x < Math.floor(((i + 1) * PANEL) / W); x++) {
          const q = (y * PANEL + x) * 4;
          if (inGlass(x, y) && own[q]! + own[q + 1]! + own[q + 2]! > 0 && world[q]! + world[q + 1]! + world[q + 2]! === 0) hidden = true;
          const sx = x - shift;
          if (sx >= 0 && sx < PANEL && cat[(y * PANEL + sx) * 4 + 3]) drawn = true;
        }
      if (hidden && !drawn) n++;
    }
  return n;
}

for (const [name, aside] of WINDOWS) {
  test(`the cat walks into place as one cat: a ${name} window`, async () => {
    const seed = 20260516;
    const walk = new Entrance(await molty(seed), await molty(seed), aside);
    for (const _ of walk.lead());
    const { core, twin } = walk;
    const glassS = walk.glassS(); // the least it takes to the glass (the opening times its start on it)
    assert.ok(walk.walking && twin);
    // it starts wholly beyond the window's right edge (panel px from the screen's left)
    const edge = PANEL / 2 + (aside / ENTER.view) * PANEL;
    assert.ok(nose(twin.catPixels()!) + walk.aside >= edge, "the cat starts beyond the window's edge");
    let frame = 0, last = core.across(), entered = false, dots = 0;
    for (let walking = true; walking; frame++) {
      assert.ok(frame < 60 * 20, "it arrives within 20 s");
      walking = walk.step(PAGE_MS);
      const at = `frame ${frame}`;
      const across = core.across();
      assert.ok(Math.abs(across - last) <= STEP, `${at}: the cat jumps ${(across - last).toFixed(3)} across`);
      last = across;
      if (!walking) break;
      assert.ok(Math.abs(twin.across() - across) < 1e-3, `${at}: the window's cat at ${twin.across().toFixed(3)}, the screen's at ${across.toFixed(3)}`);
      assert.ok(Math.abs(twin.back() - core.back()) < 1e-3, `${at}: the two cats part, back`);
      const shown: Uint8ClampedArray = twin.catPixels()!;
      dots = litInGlass(shown, walk.aside);
      if (dots && !entered) assert.ok((frame + 1) * PAGE_MS >= glassS * 1000, `${at}: at the glass sooner than ${glassS.toFixed(2)} s`);
      if (dots) entered = true;
      else assert.ok(!entered, `${at}: the cat in the glass is gone`);
      assert.equal(blankCells(core, shown, walk.aside), 0, `${at}: cells blanked with no cat drawn`);
    }
    assert.ok(entered, "the cat came into the glass");
    // the hand-over: the screen's own cat is the one the window drew, where it drew it
    const own = litInGlass(core.catPixels()!, 0);
    assert.ok(Math.abs(own - dots) <= dots / 4, `the hand-over: ${dots} lit px, then ${own}`);
    assert.ok(Math.abs(core.across()) < 1e-3 && Math.abs(core.back()) < 1e-3, "it sits at its place");
  });
}
