// The page's dust as tiles: src/assets/dust/{far,near}-{dark,light}.png, 1040 px a side, clear
// but for the dots. They are pictures of the water's own field at rest (the firmware's drawer
// showing the page's dust: services/molty-screen field.ts, device/firmware/web/web_field.c):
// "far" the loose dust that is everywhere, "near" the dust in focus round a device or a heading.
// The page repeats them as backgrounds, so the dust is there at first paint, with no script, and
// the canvas the water draws on shows the same dots moved. scripts/tests/water.test.ts wants the
// committed tiles to be exactly these.
//   node scripts/dust-tile.mjs
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import sharp from 'sharp';
import { instantiate } from '../src/services/molty-screen/core.ts';
import { CELL, MoltyField, TILE, WATER } from '../src/services/molty-screen/field.ts';

export const TILES = ['far', 'near'].flatMap((kind) => ['dark', 'light'].map((theme) => ({ kind, theme })));
export const tilePath = ({ kind, theme }) => fileURLToPath(new URL(`../src/assets/dust/${kind}-${theme}.png`, import.meta.url));
const wasm = fileURLToPath(new URL('../../../device/firmware/web/built/molty.wasm', import.meta.url));

/** A tile's pixels, RGBA. */
export async function renderTile({ kind, theme }) {
  const field = MoltyField.open(await instantiate(await WebAssembly.compile(readFileSync(wasm))), {
    cols: TILE / CELL.w,
    rows: TILE / CELL.h,
    background: theme === 'light' ? 0xffffff : 0,
    paper: theme === 'light',
    c2: WATER.c2,
    leak: WATER.leak,
    damp: WATER.damp,
  });
  return Buffer.from(field.picture(kind === 'near' ? 1 : 0));
}

/** A tile file's pixels, RGBA (clear where it is clear, whatever the file keeps there). */
export async function readTile(tile) {
  const px = await sharp(tilePath(tile)).ensureAlpha().raw().toBuffer();
  for (let i = 0; i < px.length; i += 4) if (!px[i + 3]) px.writeUInt32LE(0, i);
  return px;
}

if (import.meta.filename === process.argv[1])
  for (const tile of TILES) {
    await sharp(await renderTile(tile), { raw: { width: TILE, height: TILE, channels: 4 } }).png({ compressionLevel: 9, palette: true }).toFile(tilePath(tile));
    console.log(`wrote ${tilePath(tile)}`);
  }
