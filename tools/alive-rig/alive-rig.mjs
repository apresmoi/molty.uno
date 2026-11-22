// The alive rig (docs/alive-proposal.html section 9): a Molty played through the firmware's own web module
// (device/firmware/web/built/molty.wasm, the website's loader) in a few scenes, each a step script in the fidelity
// script's format with a fixed seed, and scored. Writes a contact sheet a scene (the creature's dots, a frame every
// half second), the numbers as JSON and as a table. README.md says what each number is.
//
//   node tools/alive-rig/alive-rig.mjs [--molty packages/cat/cat.json] [--out tools/alive-rig/out] [--scene idle]
//
// Run from anywhere; make alive-rig. Two runs write the same bytes.
import fs from 'node:fs';
import path from 'node:path';
import { deflateSync } from 'node:zlib';
import { MoltyCore } from '../../apps/website/src/services/molty-screen/core.ts';
import { play } from '../web-molty/fidelity.mjs';
import { chunk } from '../../packages/tools/play.mjs';
import { loopEcho, stillShare, rhythmSpread, boneMotion, bodySpread, pops, firstDiff, PARTS } from './metrics.mjs';

const ROOT = path.resolve(import.meta.dirname, '../..');
const FPS = 30, DT = Math.round(1e6 / FPS), PX = 466, SEED = 20260516;
const WARM = 3 * FPS;                                  // frames before a scene is scored: the mood has settled in
const TAP = [233, 300];                                // on the cat's middle, as the fidelity script taps it

/** A scene's steps: the script's lines, the frames so far, the steps that are events (their frame). */
class Steps {
  lines = []; frame = 0; marks = [];
  wait(s) { const n = Math.round(s * FPS); if (n > 0) { this.lines.push(`frames ${n} ${DT}`); this.frame += n; } return this; }
  step(line, mark) { if (mark) this.marks.push({ frame: this.frame, what: mark, at: this.lines.length }); this.lines.push(line); return this; }
  tap() { return this.step(`touch 1 ${TAP[0]} ${TAP[1]}`, 'tap').wait(0.1).step('touch 0 0 0'); }
}

/** A loudness track as the device gets it (100 ms steps): phrases of syllables about 4 a second, a pause between
 *  phrases, a floor of room noise. Made up but fixed (its own random source). */
function voice(seconds) {
  let r = 7;
  const rnd = () => ((r = (Math.imul(r, 1103515245) + 12345) >>> 0) / 2 ** 32);
  const out = [];
  let left = 0, talking = false;
  for (let i = 0; i < seconds * 10; i++) {
    if (left <= 0) { talking = !talking; left = talking ? 15 + Math.floor(rnd() * 25) : 5 + Math.floor(rnd() * 10); }
    left--;
    out.push(talking ? 0.2 + 0.6 * Math.abs(Math.sin(i * 1.3 + rnd())) * (0.6 + 0.4 * rnd()) : 0.02 + 0.03 * rnd());
  }
  return out;
}

/** The scenes. Each: its steps (after the warm up), the half second contact sheet's interval, what today cannot do. */
const SCENES = {
  idle: { sheet: 0.5, build: (s) => s.wait(60) },
  alone: { sheet: 3, note: 'played in real time (the director\'s 5 min to sleep cannot be sped up without a firmware change); the sheet is sped up, a frame every 3 s',
    build: (s) => s.wait(360) },
  listening: { sheet: 0.5, mood: 'listening',
    note: 'listen with a level only: the listening reactions (a nod at a pause, a glance at a question: link/listen_react.c) are not in the web module, so no pause reaction is scored',
    build: (s) => { s.step('listen 1', 'listen'); for (const v of voice(20)) s.step(`level ${v.toFixed(3)}`).wait(0.1); return s.step('listen 0', 'listen end'); } },
  thinking: { sheet: 0.5, mood: 'thinking', build: (s) => s.wait(60) },
  working: { sheet: 0.5, mood: 'working', build: (s) => s.wait(60) },
  speaking: { sheet: 0.5, mood: 'speaking', note: 'the speaking mood\'s loop only: no script step feeds a voice to the creature yet (slice 2), so no lip match',
    build: (s) => s.wait(15) },
  touched: { sheet: 0.5, build: (s) => { for (let k = 0; k < 4; k++) s.wait(k ? 6.9 : 7).tap(); return s.wait(6.9).step('shake', 'shake').wait(7); } },
};

/** The script of a scene for a Molty: the creature, power on, the warm up (with the mood), the scene. */
function scene(name, molty) {
  const def = SCENES[name], s = new Steps();
  s.step(`creature ${molty.id} ${molty.file}`).step(`start ${PX} ${SEED} 0`);
  if (def.mood) s.step(`state {"mood":"${def.mood}"}`);
  s.wait(WARM / FPS);
  def.build(s);
  return s;
}

/** Plays script lines on a fresh module; a frame's record from the frame number on: the creature's picture (a value a
 *  cell), its changed share, its bones; sheet frames' pixels; the device mode's costs. */
async function record(mod, lines, { from = 0, sheet = [], device = false } = {}) {
  const core = await MoltyCore.create(mod);
  const x = core.raw;
  const at = Object.create(core);                     // the device mode is on from power on, when asked
  at.start = (...a) => { const ok = core.start(...a); if (device) x.wm_device(1); return ok; };
  const W = core.gridW || 59, out = { pics: [], change: [], bones: [], sheet: new Map(), cost: [] };
  let prev = null, H = 0;
  play(at, lines.join('\n'), (n) => {
    if (n < from) return;
    if (device) { const fi = new Float32Array(x.memory.buffer, x.wm_frame_info(), 10); out.cost.push([fi[6], fi[8]]); return; }
    H ||= core.gridH;
    const px = core.catPixels() ?? new Uint8ClampedArray(PX * PX * 4);
    const pic = new Float32Array(core.gridW * H);
    for (let j = 0; j < H; j++) for (let i = 0; i < W; i++) {
      const x0 = Math.floor(i * PX / W), x1 = Math.floor((i + 1) * PX / W), y0 = Math.floor(j * PX / H), y1 = Math.floor((j + 1) * PX / H);
      let v = 0;
      for (let y = y0; y < y1; y++) for (let q = (y * PX + x0) * 4; q < (y * PX + x1) * 4; q += 4) v += px[q] + px[q + 1] + px[q + 2];
      pic[j * W + i] = v / (3 * (x1 - x0) * (y1 - y0));
    }
    let lit = 0, diff = 0;
    for (let q = 0; q < px.length; q += 4) {
      const on = px[q] | px[q + 1] | px[q + 2], was = prev ? prev[q] | prev[q + 1] | prev[q + 2] : 0;
      if (on || was) { lit++; if (!prev || px[q] !== prev[q] || px[q + 1] !== prev[q + 1] || px[q + 2] !== prev[q + 2]) diff++; }
    }
    prev = Uint8ClampedArray.from(px);
    const b = x.wm_bones();
    out.pics.push(pic); out.change.push(lit ? diff / lit : 0);
    out.bones.push(b ? new Float32Array(x.memory.buffer, b, molty.nodes.length * 12).slice() : new Float32Array(molty.nodes.length * 12));
    if (sheet.includes(n - from)) out.sheet.set(n - from, prev);
  });
  return out;
}
let molty;

/** One contact sheet: the creature's frames on black, cropped to where it ever is, half size, each with its time. */
function contactSheet(frames, every) {
  let x0 = PX, y0 = PX, x1 = 0, y1 = 0;
  for (const px of frames.values()) for (let y = 0; y < PX; y++) for (let x = 0; x < PX; x++) {
    const q = (y * PX + x) * 4;
    if (px[q] | px[q + 1] | px[q + 2]) { x0 = Math.min(x0, x); x1 = Math.max(x1, x); y0 = Math.min(y0, y); y1 = Math.max(y1, y); }
  }
  if (x1 < x0) { x0 = y0 = 0; x1 = y1 = PX - 1; }
  x0 = Math.max(0, x0 - 6) & ~1; y0 = Math.max(0, y0 - 6) & ~1;
  const tw = Math.ceil((Math.min(PX, x1 + 7) - x0) / 2), th = Math.ceil((Math.min(PX, y1 + 7) - y0) / 2), lab = 9;
  const cols = Math.min(12, frames.size), rows = Math.ceil(frames.size / cols), gap = 4;
  const W = cols * (tw + gap) + gap, H = rows * (th + lab + gap) + gap;
  const raw = Buffer.alloc((W * 3 + 1) * H);
  const put = (x, y, c) => { if (x >= 0 && y >= 0 && x < W && y < H) raw.set(c, y * (W * 3 + 1) + 1 + x * 3); };
  [...frames.entries()].forEach(([n, px], k) => {
    const ox = gap + (k % cols) * (tw + gap), oy = gap + Math.floor(k / cols) * (th + lab + gap);
    for (let y = 0; y < th; y++) for (let x = 0; x < tw; x++) {
      const c = [0, 0, 0];
      for (let dy = 0; dy < 2; dy++) for (let dx = 0; dx < 2; dx++) {
        const sx = x0 + 2 * x + dx, sy = y0 + 2 * y + dy;
        if (sx < PX && sy < PX) for (let r = 0; r < 3; r++) c[r] += px[(sy * PX + sx) * 4 + r] / 4;
      }
      put(ox + x, oy + lab + y, c.map(Math.round));
    }
    for (let y = 0; y < th + lab; y++) { put(ox - 1, oy + y, [40, 40, 40]); put(ox + tw, oy + y, [40, 40, 40]); }
    text(`${(n / FPS).toFixed(1)}s`, ox + 2, oy + 2, put);
  });
  const head = Buffer.alloc(13); head.writeUInt32BE(W, 0); head.writeUInt32BE(H, 4); head[8] = 8; head[9] = 2;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', head), chunk('IDAT', deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}
// a 3 x 5 font for the times: a glyph's rows, 3 bits each
const FONT = { 0: [7, 5, 5, 5, 7], 1: [2, 6, 2, 2, 7], 2: [7, 1, 7, 4, 7], 3: [7, 1, 3, 1, 7], 4: [5, 5, 7, 1, 1], 5: [7, 4, 7, 1, 7],
  6: [7, 4, 7, 5, 7], 7: [7, 1, 1, 2, 2], 8: [7, 5, 7, 5, 7], 9: [7, 5, 7, 1, 7], '.': [0, 0, 0, 0, 2], s: [0, 3, 6, 3, 6] };
function text(s, x, y, put) {
  for (const ch of s) { (FONT[ch] ?? []).forEach((row, j) => { for (let i = 0; i < 3; i++) if (row >> (2 - i) & 1) put(x + i, y + j, [150, 150, 150]); }); x += 4; }
}

const r4 = (v) => (v == null ? null : Math.round(v * 1e4) / 1e4), r1 = (v) => Math.round(v * 10) / 10;
const mean = (v) => v.reduce((a, b) => a + b, 0) / (v.length || 1);

/** A scene's numbers. */
async function score(mod, name) {
  const def = SCENES[name], s = scene(name, molty), n = s.frame - WARM;
  const sheet = Array.from({ length: Math.floor(n / FPS / def.sheet) }, (_, k) => Math.round(k * def.sheet * FPS));
  const rec = await record(mod, s.lines, { from: WARM, sheet });
  const cost = (await record(mod, s.lines, { from: WARM, device: true })).cost;
  const echo = loopEcho(rec.pics, FPS, 30 * FPS, 4 * FPS);
  const motion = boneMotion(rec.bones, molty.nodes);
  const events = s.marks.filter((m) => m.frame >= WARM).map((m) => ({ ...m, frame: m.frame - WARM }));
  // a reaction: the same steps without that event (and what follows it), against the scene, from the event on
  const reactions = [];
  for (const m of events.filter((e) => e.what === 'tap' || e.what === 'shake')) {
    const twin = await record(mod, [...s.lines.slice(0, m.at), `frames ${3 * FPS} ${DT}`], { from: WARM });
    const pic = firstDiff(rec.pics, twin.pics, m.frame), bone = firstDiff(rec.bones, twin.bones, m.frame);
    reactions.push({ what: m.what, at_s: r4(m.frame / FPS), picture_ms: pic < 0 ? null : Math.round((pic - m.frame + 1) * 1000 / FPS),
      bones_ms: bone < 0 ? null : Math.round((bone - m.frame + 1) * 1000 / FPS) });
  }
  const popped = pops(motion, events.map((e) => e.frame));
  const rhythm = rhythmSpread(rec.change);
  const spread = bodySpread(motion, molty.nodes);
  return {
    numbers: {
      seconds: r4(n / FPS),
      loop_echo: echo && { echo: r4(echo.echo), lag_s: r4(echo.lag / FPS), standout: r4(echo.standout), at_4s: r4(echo.at) },
      still_share: r4(stillShare(rec.change)),
      rhythm_spread: r4(rhythm.spread), bursts: rhythm.starts.length,
      body_spread: Object.fromEntries(PARTS.map((p) => [p, r4(spread[p])])),
      reactions,
      pops: popped.length, pops_at_s: popped.map((t) => r4(t / FPS)),
      ...(def.note ? { note: def.note } : {}),
    },
    cost: { frame_ms_mean: r1(mean(cost.map((c) => c[1]))), frame_ms_max: r1(Math.max(...cost.map((c) => c[1]))),
      draw_ms_mean: r1(mean(cost.map((c) => c[0]))), draw_ms_max: r1(Math.max(...cost.map((c) => c[0]))) },
    png: contactSheet(rec.sheet, def.sheet),
  };
}

function table(all) {
  const f = (v, d = 2) => (v == null ? '-' : typeof v === 'number' ? v.toFixed(d) : String(v));
  const rows = [['scene', 's', 'echo', 'lag s', 'standout', 'at 4 s', 'still', 'rhythm', ...PARTS, 'react ms', 'pops']];
  for (const [name, x] of Object.entries(all)) {
    const e = x.loop_echo ?? {};
    rows.push([name, f(x.seconds, 0), f(e.echo, 3), f(e.lag_s), f(e.standout, 3), f(e.at_4s, 3), f(x.still_share), f(x.rhythm_spread),
      ...PARTS.map((p) => f(x.body_spread[p])), x.reactions.length ? x.reactions.map((r) => r.picture_ms ?? '-').join('/') : '-', f(x.pops, 0)]);
  }
  const notes = Object.entries(all).filter(([, x]) => x.note).map(([n, x]) => `${n}: ${x.note}`);
  return [...columns(rows), '', ...notes].join('\n') + '\n';
}
function columns(rows) {
  const w = rows[0].map((_, i) => Math.max(...rows.map((r) => String(r[i]).length)));
  return rows.map((r) => r.map((c, i) => String(c).padStart(w[i])).join('  '));
}

const opt = (k, d) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
const file = path.relative(ROOT, path.resolve(opt('--molty', path.join(ROOT, 'packages/cat/cat.json'))));
const out = path.resolve(opt('--out', path.join(ROOT, 'tools/alive-rig/out')));
const only = opt('--scene');
if (only && !SCENES[only]) { console.error(`alive-rig: no scene ${only} (${Object.keys(SCENES).join(', ')})`); process.exit(2); }
process.chdir(ROOT);                                   // the scripts' paths
const pkg = JSON.parse(fs.readFileSync(file, 'utf8'));
molty = { id: pkg.id, file, nodes: pkg.creature?.rig?.nodes ?? [] };
const mod = await WebAssembly.compile(fs.readFileSync('device/firmware/web/built/molty.wasm'));
fs.mkdirSync(out, { recursive: true });
const all = {}, costs = {};
for (const name of only ? [only] : Object.keys(SCENES)) {
  const { numbers, cost, png } = await score(mod, name);
  all[name] = numbers; costs[name] = cost;
  fs.writeFileSync(path.join(out, `${name}.png`), png);
}
const head = { molty: { id: pkg.id, version: pkg.version, file }, seed: SEED, fps: FPS };
fs.writeFileSync(path.join(out, 'numbers.json'), JSON.stringify({ ...head, scenes: all }, null, 2) + '\n');
const t = table(all);
fs.writeFileSync(path.join(out, 'table.txt'), t);
// the cost apart: the device mode's clock runs with the computer's (web_sdk.c), so it moves a little from run to run
fs.writeFileSync(path.join(out, 'cost.json'), JSON.stringify({ ...head, scenes: costs }, null, 2) + '\n');
const c = columns([['cost (device mode, varies a little)', 'frame ms', 'max', 'draw ms', 'max'],
  ...Object.entries(costs).map(([n, x]) => [n, x.frame_ms_mean, x.frame_ms_max, x.draw_ms_mean, x.draw_ms_max].map(String))]);
process.stdout.write(t + '\n' + c.join('\n') + '\n');
