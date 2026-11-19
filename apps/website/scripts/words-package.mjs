// The page's words as a scenes package for the firmware's renderer (src/config/words.mjs
// has the letters): writes src/assets/molty/words.json and checks the renderer takes it.
//   node scripts/words-package.mjs [preview dir]   (preview: each word as the renderer draws it, PNG)
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import sharp from 'sharp';
import { wordsPackage, WORDS } from '../src/config/words.mjs';
import { MoltyCore } from '../src/services/molty-screen/core.ts';

const OUT = fileURLToPath(new URL('../src/assets/molty/words.json', import.meta.url));
const json = JSON.stringify(wordsPackage());
const module = await WebAssembly.compile(readFileSync(fileURLToPath(new URL('../../../device/firmware/web/built/molty.wasm', import.meta.url))));
for (const [name] of WORDS) {
  const core = await MoltyCore.create(module);
  if (!core.pack(Buffer.from(json))) throw new Error('the renderer refuses the package');
  if (!core.start(1, false, 504) || !core.scene(name, undefined, 600)) throw new Error(`the renderer refuses the layout ${name}`);
  if (process.argv[2]) {
    mkdirSync(process.argv[2], { recursive: true });
    for (let i = 0; i < 150; i++) core.frame(33333);
    await sharp(Buffer.from(core.pixels()), { raw: { width: 504, height: 504, channels: 4 } }).png().toFile(`${process.argv[2]}/${name}.png`);
  }
}
writeFileSync(OUT, json + '\n');
console.log(`wrote ${OUT} (${json.length} bytes, ${WORDS.length} words)`);
