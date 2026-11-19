// The page's calm water as two tiles (dark page, paper): src/assets/dust/dark.png and light.png,
// TILE px a side, transparent but for the specks (src/config/water.mjs has the rule: the
// firmware's). The page repeats one as its background, so the dust is there at first paint, with
// no script. scripts/tests/water.test.ts wants the committed tiles to be exactly these.
//   node scripts/dust-tile.mjs
import { fileURLToPath } from 'node:url';
import sharp from 'sharp';
import { CALM, FINE, TILE, ign, speck } from '../src/config/water.mjs';

export const tilePath = (theme) => fileURLToPath(new URL(`../src/assets/dust/${theme}.png`, import.meta.url));

/** The tile's pixels, RGBA. */
export function renderTile(theme) {
  const px = Buffer.alloc(TILE * TILE * 4);
  const [r, g, b] = speck(FINE.LEVEL_FAR, theme === 'light');
  for (let y = 0; y < TILE; y++) for (let x = 0; x < TILE; x++) if (ign(x, y) < CALM) px.set([r, g, b, 255], (y * TILE + x) * 4);
  return px;
}

if (import.meta.filename === process.argv[1])
  for (const theme of ['dark', 'light']) {
    await sharp(renderTile(theme), { raw: { width: TILE, height: TILE, channels: 4 } }).png({ compressionLevel: 9 }).toFile(tilePath(theme));
    console.log(`wrote ${tilePath(theme)}`);
  }
