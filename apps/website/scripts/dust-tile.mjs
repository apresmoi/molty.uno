// The page's dust as tiles: src/assets/dust/{far,near}-{dark,light}.png, TILE px a side,
// transparent but for the specks (src/config/water.mjs has the two kinds and the rule: the
// firmware's numbers). The page repeats them as backgrounds, so the dust is there at first paint,
// with no script. scripts/tests/water.test.ts wants the committed tiles to be exactly these.
//   node scripts/dust-tile.mjs
import { fileURLToPath } from 'node:url';
import sharp from 'sharp';
import { DUST, TILE, hashU, ign, speck } from '../src/config/water.mjs';

export const TILES = ['far', 'near'].flatMap((kind) => ['dark', 'light'].map((theme) => ({ kind, theme })));
export const tilePath = ({ kind, theme }) => fileURLToPath(new URL(`../src/assets/dust/${kind}-${theme}.png`, import.meta.url));

/** A tile's pixels, RGBA. */
export function renderTile({ kind, theme }) {
  const { density, level, ordered } = DUST[kind];
  const px = Buffer.alloc(TILE * TILE * 4);
  const [r, g, b] = speck(level, theme === 'light');
  for (let y = 0; y < TILE; y++) for (let x = 0; x < TILE; x++) if ((ordered ? ign(x, y) : hashU(x, y, 1)) < density) px.set([r, g, b, 255], (y * TILE + x) * 4);
  return px;
}

if (import.meta.filename === process.argv[1])
  for (const tile of TILES) {
    await sharp(renderTile(tile), { raw: { width: TILE, height: TILE, channels: 4 } }).png({ compressionLevel: 9 }).toFile(tilePath(tile));
    console.log(`wrote ${tilePath(tile)}`);
  }
