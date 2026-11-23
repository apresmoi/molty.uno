// The WebAssembly Molty through a script (device/firmware/web/web_main.c has the format and is
// the native player of the same script): prints each frame's number and the hash of its pixels.
//   node tools/web-molty/fidelity.mjs <molty.wasm> <script> [dump dir]
// Run from the repo root (the script's paths are relative to it). The module is driven through
// the website's own loader, so the check covers what the page runs.
import fs from 'node:fs';
import path from 'node:path';
import { MoltyCore } from '../../apps/website/src/services/molty-screen/core.ts';

/** FNV-1a, 64 bit, as web_main.c: two 32 bit halves by hand (no BigInt per byte). */
export function hashPixels(px) {
  let lo = 0x84222325, hi = 0xcbf29ce4;
  for (let i = 0; i < px.length; i++) {
    lo ^= px[i];
    // × 0x100000001b3 = (hi, lo) × 0x1b3 + (lo << 40)
    const a = (lo >>> 0) * 0x1b3;
    const carry = Math.floor(a / 0x100000000);
    hi = (Math.imul(hi, 0x1b3) + carry + ((lo << 8) >>> 0)) >>> 0;
    lo = a >>> 0;
  }
  return (hi >>> 0).toString(16).padStart(8, '0') + (lo >>> 0).toString(16).padStart(8, '0');
}

/** Run `script` on `core`; onFrame(n, pixels) after each frame. */
export function play(core, script, onFrame) {
  let frame = 0;
  for (const raw of script.split('\n')) {
    const line = raw.trim();
    if (!line || line.startsWith('#')) continue;
    const [op, ...args] = line.split(/\s+/);
    const rest = line.slice(op.length).trim();
    let ok = true;
    if (op === 'creature') ok = core.creature(args[0], fs.readFileSync(args[1]));
    else if (op === 'package') ok = core.pack(fs.readFileSync(args[0]));
    else if (op === 'start') ok = core.start(Number(args[1]), args[2] === '1', Number(args[0]));
    else if (op === 'frames')
      for (let i = 0; i < Number(args[0]); i++, frame++) {
        core.frame(Number(args[1]));
        onFrame(frame, core.pixels());
      }
    else if (op === 'touch') core.touch(args[0] === '1', Number(args[1]), Number(args[2]));
    else if (op === 'shake') core.shake();
    else if (op === 'state') ok = core.state(JSON.parse(rest));
    else if (op === 'listen') core.listen(args[0] === '1');
    else if (op === 'level') core.level(Number(args[0]));
    else if (op === 'ask') ok = core.ask(JSON.parse(rest));
    else if (op === 'ask_end') core.askEnd();
    else if (op === 'scene') ok = core.scene(args[0], args[2] ? JSON.parse(rest.split(/\s+/).slice(2).join(' ')) : undefined, Number(args[1]));
    else if (op === 'scene_clear') core.sceneClear();
    else if (op === 'time') core.time(Number(args[0]));
    else if (op === 'weather') ok = core.weather(Number(args[0]), Number(args[1]));
    else ok = false;
    if (!ok) throw new Error(`step failed: ${line}`);
  }
}

/** The script on a fresh Molty of `wasm` (the module's bytes): its listing, a line a frame ("<n> <hash>"), as
 *  web_main.c prints it. dump: a folder for every frame as a PPM. */
export async function listing(wasm, script, dump) {
  const core = await MoltyCore.create(await WebAssembly.compile(wasm));
  const out = [];
  play(core, script, (n, px) => {
    out.push(`${n} ${hashPixels(px)}`);
    if (!dump) return;
    const side = Math.sqrt(px.length / 4);
    const rgb = Buffer.alloc(side * side * 3);
    for (let i = 0; i < side * side; i++) rgb.set(px.subarray(i * 4, i * 4 + 3), i * 3);
    fs.writeFileSync(path.join(dump, `${String(n).padStart(5, '0')}.ppm`), Buffer.concat([Buffer.from(`P6\n${side} ${side}\n255\n`), rgb]));
  });
  return out.join('\n') + '\n';
}

if (import.meta.filename === path.resolve(process.argv[1] ?? '')) {
  const [wasm, script, dump] = process.argv.slice(2);
  if (!wasm || !script) {
    console.error('usage: node tools/web-molty/fidelity.mjs <molty.wasm> <script> [dump dir]');
    process.exit(2);
  }
  process.stdout.write(await listing(fs.readFileSync(wasm), fs.readFileSync(script, 'utf8'), dump));
}
