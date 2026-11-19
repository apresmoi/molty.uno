// The still that stands in for a Molty screen (no JavaScript, before the screen is live, a
// renderer that cannot load): one frame of the cat from the firmware's own renderer, the module
// the page runs (device/firmware/web/built/molty.wasm), after it has settled.
//   node scripts/molty-still.mjs            write src/assets/molty/cat.webp (lossless)
// scripts/tests/molty.test.ts renders the same frame and wants the committed still to be it.
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import sharp from 'sharp';
import { MoltyCore, PANEL } from '../src/services/molty-screen/core.ts';

const repo = (path) => fileURLToPath(new URL(`../../../${path}`, import.meta.url));
export const STILL = fileURLToPath(new URL('../src/assets/molty/cat.webp', import.meta.url));
const SEED = 20260516;
const FRAMES = 150; // 5 s at the device's pace: formed, sitting, eyes open

/** The still's pixels: PANEL x PANEL, RGBA. */
export async function renderStill() {
  const core = await MoltyCore.create(await WebAssembly.compile(readFileSync(repo('device/firmware/web/built/molty.wasm'))));
  if (!core.creature('cat', readFileSync(repo('packages/cat/cat.json'))) || !core.start(SEED, false)) throw new Error('the cat did not start');
  for (let i = 0; i < FRAMES; i++) core.frame(33333);
  return Buffer.from(core.pixels());
}

if (import.meta.filename === process.argv[1]) {
  await sharp(await renderStill(), { raw: { width: PANEL, height: PANEL, channels: 4 } }).webp({ lossless: true }).toFile(STILL);
  console.log(`wrote ${STILL}`);
}
