// The GitHub README's hero (docs/assets/molty-hero.webp, -light.webp), drawn by the firmware's own
// renderer (device/firmware/web/built/molty.wasm, the module the site's screens run), stepped frame
// by frame in a headless browser: no wall clock, so every run gives the same pictures.
//   left   the wordmark, the landing title's word layout (src/assets/molty/words.json) as the
//          renderer draws it, and a caption that cross-fades with what the screen shows
//   right  one round screen with the site's bezel and glass (Device, GlassDome), in device mode
//          (web_device.c wm_device): every change of picture is the firmware's dissolve
// One round: the cat breathes and blinks, grooms itself, the sky chart, the rhythm clock, a
// question tapped yes and the reply, the cat again (the loop's seam is a short cross-fade).
// Every frame mapped onto one palette of 160 colours (what does not move stays bit-identical, the
// dots stay sharp), then lossless animated WebP, GitHub's page colour under it (#0d1117, #fff).
//   pnpm readme [outDir=../../docs/assets]          (README_THEMES=dark,light README_FPS=25)
import { chromium } from 'playwright';
import sharp from 'sharp';
import { readFileSync, statSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

const at = (p) => fileURLToPath(new URL(p, import.meta.url));
const OUT = process.argv[2] ?? at('../../../../docs/assets');
const THEMES = (process.env.README_THEMES ?? 'dark,light').split(',');
const FPS = Number(process.env.README_FPS ?? 25);
const COLOURS = 160;
const W = 830, H = 360, D = 296, DPR = 2, LENGTH = 12, WARMUP = 3, SEAM = 8;
const FILES = {
  'molty.wasm': at('../../../../device/firmware/web/built/molty.wasm'),
  'cat.json': at('../../../../packages/cat/cat.json'),
  'astrology.json': at('../../../../packages/astrology/astrology.json'),
  'rhythm.json': at('../../../../packages/rhythm/rhythm.json'),
  'words.json': at('../../src/assets/molty/words.json'),
};
const chart = readFileSync(at('../../../../device/firmware/test/host/fixtures/astrology-commands.jsonl'), 'utf8')
  .trim().split('\n').map((l) => JSON.parse(l)).find((c) => c.args.layout === 'chart');
// rhythm.play with one rhythm, as skills/molty-rhythm/references/commands.md writes it
const beat = { regions: [{ label: 'ARGENTINA', color: '#8cc4ff' }], pick: 0, bpm: 100, rhythms: [{
  id: 'chacarera', label: 'CHACARERA', steps: 6, per: 3, beats: [1, 4], count_in: 1, region: 0,
  layers: [{ id: 'aro', sound: 'rim', color: '#5fb0ff', hits: [1, 4] }, { id: 'parche', sound: 'head', color: '#ffa36b', hits: [3, 5], accent: [5] }],
  bars: [{ name: 'A', hits: [[1, 4], [3, 5]] }, { name: 'B', hits: [[], [3, 5]] }], order: [0, 0, 1, 0] }] };
const CAPTIONS = ['<b>breathes</b> and blinks', '<b>grooms</b> itself', '<b>reads</b> the sky', '<b>keeps</b> the beat', '<b>asks;</b> one tap answers'];
/** What happens when (s from the loop's start): a call on the screen (`do`) or a caption. */
const ROUND = [
  { t: 0.7, do: ['habit', 'groom'] }, { t: 0.8, cap: 1 },
  { t: 2.9, do: ['scene', 'astrology', chart.args] }, { t: 3.3, cap: 2 },
  { t: 5.3, do: ['scene', 'rhythm', { layout: 'clock', params: { tick_ms: 0, tick: 0 }, ttl_s: 600 }] },
  { t: 5.3, do: ['rhythm', 'rhythm.play', beat] }, { t: 5.7, cap: 3 },
  { t: 7.3, do: ['question', { kind: 'ask', job: 'readme', title: 'open the PR?', options: ['yes', 'no', 'idk'] }] }, { t: 7.7, cap: 4 },
  { t: 8.9, do: ['tap', -0.62, -0.18] },
  { t: 9.05, do: ['rhythm', 'rhythm.stop', {}] }, { t: 9.05, do: ['clear'] }, { t: 9.05, do: ['state', { state: 'text', text: 'yes.' }] },
  { t: 10.2, do: ['state', { state: 'creature' }] }, { t: 10.5, cap: 0 },
];

/** The page (the site's tokens for its device, dark and light): a word window and a screen. */
const page = (theme) => `<!doctype html><html data-theme="${theme}"><head><meta charset="utf-8"><style>
:root { --bg: #0d1117; --ink: #d8e2dc; --dim: #7d8a83; --bezel: #181d1b; --bezel-in: #0c0f0e; --bezel-out: #070908;
  --bezel-hi: rgba(215,235,225,.13); --bezel-lo: rgba(0,0,0,.62); --bezel-lip: rgba(225,242,233,.5); --gasket: #020303;
  --contact: rgba(0,0,0,.75); --off: #030504; --hair: rgba(255,255,255,.07); --sheen: rgba(255,255,255,.1);
  --spec: rgba(255,255,255,.85); --spec-soft: rgba(255,255,255,.22); --rim: rgba(255,255,255,.16); --edge: rgba(0,0,0,.5); }
[data-theme=light] { --bg: #fff; --ink: #18201c; --dim: #48544e; --bezel: #dce0de; --bezel-in: #aab3ae; --bezel-out: #8f9994;
  --bezel-hi: rgba(255,255,255,.7); --bezel-lo: rgba(24,32,28,.34); --bezel-lip: rgba(255,255,255,.95); --gasket: #3a423e;
  --contact: rgba(24,32,28,.38); --off: #1b211e; --hair: rgba(24,32,28,.1); }
html, body { margin: 0; overflow: hidden; background: var(--bg); }
#word { position: absolute; left: 1px; top: -61px; width: 472px; height: 472px;
  mask-image: radial-gradient(ellipse 56% 34% at 50% 50%, #000 58%, transparent 100%); mix-blend-mode: screen; }
[data-theme=light] #word { filter: invert(1) hue-rotate(180deg); mix-blend-mode: multiply; }
.cap { position: absolute; left: 26px; top: 224px; font: 500 17px/1.3 ui-monospace, "SF Mono", Menlo, monospace; color: var(--dim); white-space: nowrap; }
.cap b { color: var(--ink); font-weight: 600; }
/* Device.module.css + GlassDome.module.css at rest */
.dev { position: absolute; left: ${W - D - 46}px; top: ${(H - D) / 2 - 4}px; width: ${D}px; height: ${D}px; container-type: size; }
.dev > * { position: absolute; inset: 0; border-radius: 50%; }
#screen { width: 100%; height: 100%; background: var(--off); }
.shadow { inset: -5%; box-shadow: 1.5cqw 4cqw 7cqw -1cqw var(--contact); }
.bezel { inset: -5%; background: linear-gradient(135deg, var(--bezel-hi) 8%, transparent 46%, transparent 58%, var(--bezel-lo) 92%),
  radial-gradient(closest-side, var(--gasket) 90.6%, var(--bezel-in) 91.6%, var(--bezel) 94.5%, var(--bezel) 96%, var(--bezel-out) 100%);
  mask-image: radial-gradient(closest-side, transparent 90.2%, #000 90.6%, #000 99.3%, transparent 100%); }
.lip { background: radial-gradient(closest-side, transparent 94%, var(--bezel-lip) 95.6%, transparent 97.6%);
  mask-image: conic-gradient(from 180deg, transparent 60deg, #000 120deg, #000 150deg, transparent 205deg, transparent 280deg, rgba(0,0,0,.07) 315deg, transparent 350deg); }
.dome { overflow: hidden; background: radial-gradient(circle closest-side, transparent 58%, var(--edge) 100%); box-shadow: inset 0 0 0 1px var(--hair); }
.dome > * { position: absolute; inset: 0; }
.sheen { background: radial-gradient(34% 26% at 32% 29%, var(--sheen), transparent); }
.spec { transform-origin: 32% 29%; transform: rotate(-40deg); background: radial-gradient(7% 2.4% at 32% 29%, var(--spec) 0 35%, var(--spec-soft) 60%, transparent); }
.glint { background: radial-gradient(circle at 47% 45%, transparent 68.5%, var(--rim) 71.8%, transparent 74%); mask-image: linear-gradient(135deg, transparent 52%, #000 78%); }
</style></head><body>
<canvas id="word" width="1024" height="1024"></canvas>
<div class="cap" id="cap0">${CAPTIONS[0]}</div><div class="cap" id="cap1" style="opacity:0"></div>
<div class="dev"><span class="shadow"></span><canvas id="screen" width="466" height="466"></canvas><span class="bezel"></span><span class="lip"></span>
<span class="dome"><span class="sheen"></span><span class="spec"></span><span class="glint"></span></span></div></body></html>`;

/** In the browser: two firmware instances (the word, the screen) on one clock, `window.R`. */
async function boot() {
  const enc = new TextEncoder();
  const bytes = async (f) => new Uint8Array(await (await fetch(f)).arrayBuffer());
  const mod = await WebAssembly.compile(await bytes('molty.wasm'));
  const R = (window.R = { t: 0 });
  const make = async (px) => {
    let x;
    const view = () => new DataView(x.memory.buffer);
    x = (await WebAssembly.instantiate(mod, {
      env: { emscripten_notify_memory_growth: () => {} },
      wasi_snapshot_preview1: {
        clock_time_get: (_i, _p, out) => (view().setBigUint64(out, BigInt(Math.round(R.t * 1e6)), true), 0),
        fd_write: (_f, iov, n, w) => { let t = 0; for (let i = 0; i < n; i++) t += view().getUint32(iov + i * 8 + 4, true); view().setUint32(w, t, true); return 0; },
        fd_read: () => 8, fd_close: () => 0, fd_seek: () => 70,
      },
    })).exports;
    x._initialize();
    const lend = (b, f) => { const p = x.wm_alloc(b.length + 1), m = new Uint8Array(x.memory.buffer); m.set(b, p); m[p + b.length] = 0; try { return f(p, b.length); } finally { x.wm_free(p); } };
    const s = (v, f) => lend(enc.encode(typeof v === 'string' ? v : JSON.stringify(v)), f);
    return { x, px, lend, s, pack: async (f) => lend(await bytes(f), (p, n) => x.wm_package(p, n)) };
  };
  const word = await make(1024);
  await word.pack('words.json');
  word.x.wm_start(1024, 1, 0);
  word.x.wm_haze();
  word.s('molty-uno', (l) => word.x.wm_scene(l, 0, 0));
  const scr = await make(466);
  const cat = await bytes('cat.json');
  scr.s('cat', (id) => scr.lend(cat, (p, n) => scr.x.wm_creature(id, p, n)));
  const pkgs = { astrology: await bytes('astrology.json'), rhythm: await bytes('rhythm.json') };
  scr.x.wm_sense(1); // a tap does what the device does with it (answers the question)
  scr.x.wm_start(466, 20261126, 0);
  scr.x.wm_time(1764190000); // a fixed night for the sky behind the cat
  scr.x.wm_device(1);
  const draw = (e, id) => {
    const g = document.getElementById(id).getContext('2d'), img = g.createImageData(e.px, e.px);
    img.data.set(new Uint8ClampedArray(e.x.memory.buffer, e.x.wm_pixels(), e.px * e.px * 4));
    g.putImageData(img, 0, 0);
  };
  const calls = {
    habit: (n) => scr.s(n, (p) => scr.x.wm_habit(p)),
    scene: (pkg, a) => { scr.lend(pkgs[pkg], (p, n) => scr.x.wm_package(p, n)); scr.s(a.layout, (l) => scr.s(a.params ?? {}, (q) => scr.x.wm_scene(l, q, a.ttl_s ?? 0))); },
    rhythm: (name, a) => scr.s(name, (n) => scr.s(a, (q) => scr.x.wm_rhythm(n, q))),
    question: (q) => scr.s(q, (p) => scr.x.wm_question(p)),
    tap: (gx, gy) => { R.lift = 0.1; scr.x.wm_touch(1, Math.round(((gx + 1) / 2) * 466), Math.round(((1 - gy) / 2) * 466)); },
    clear: () => scr.x.wm_scene_clear(),
    state: (o) => scr.s(o, (p) => scr.x.wm_state(p)),
  };
  R.do = ([name, ...a]) => calls[name](...a);
  R.cap = (from, to, html, k) => { const a = document.getElementById(from), b = document.getElementById(to); if (html !== undefined) b.innerHTML = html; a.style.opacity = 1 - k; b.style.opacity = k; };
  // the word settles during the warm-up and then holds (calm water: the title as the page shows it)
  R.step = (ms, warm) => {
    for (let i = 0; i < 2; i++) {
      R.t += ms / 2;
      for (const e of warm ? [word, scr] : [scr]) e.x.wm_frame(Math.round((ms / 2) * 1000));
      if (R.lift !== undefined && (R.lift -= ms / 2000) <= 0) { scr.x.wm_touch(0, 0, 0); R.lift = undefined; }
    }
    draw(word, 'word');
    draw(scr, 'screen');
  };
}

/** One palette for the loop, every frame (raw RGB) mapped onto it: median cut over the colours of
 *  48 frames across it, each colour weighed by how often it shows to the power 0.6, so the
 *  screen's small bright marks (the cat's eyes, a chart's planets) keep their own colours. */
async function quantize(raw, page) {
  const count = new Map();
  for (let i = 0; i < 48; i++) {
    const data = raw[Math.floor((i * raw.length) / 48)];
    for (let j = 0; j < data.length; j += 6) {
      const k = (data[j] << 16) | (data[j + 1] << 8) | data[j + 2];
      count.set(k, (count.get(k) ?? 0) + 1);
    }
  }
  const ch = (k, c) => (k >> (16 - 8 * c)) & 255;
  const wt = (n) => n ** 0.6;
  let boxes = [[...count.keys()]];
  while (boxes.length < COLOURS) {
    let best = -1, axis = 0, at = 0;
    boxes.forEach((box, i) => {
      if (box.length < 2) return;
      for (let c = 0; c < 3; c++) {
        let lo = 255, hi = 0;
        for (const k of box) { const v = ch(k, c); if (v < lo) lo = v; if (v > hi) hi = v; }
        if (hi - lo > best) (best = hi - lo), (axis = c), (at = i);
      }
    });
    if (best <= 0) break;
    const box = boxes[at].sort((a, b) => ch(a, axis) - ch(b, axis));
    let half = 0, sum = 0;
    const total = box.reduce((t, k) => t + wt(count.get(k)), 0);
    while (half < box.length - 1 && (sum += wt(count.get(box[half]))) < total / 2) half++;
    boxes.splice(at, 1, box.slice(0, half + 1), box.slice(half + 1));
  }
  const palette = boxes.filter((b) => b.length).map((box) => {
    const m = [0, 0, 0];
    let t = 0;
    for (const k of box) { const w = wt(count.get(k)); t += w; for (let c = 0; c < 3; c++) m[c] += ch(k, c) * w; }
    return (Math.round(m[0] / t) << 16) | (Math.round(m[1] / t) << 8) | Math.round(m[2] / t);
  });
  palette.push(page); // the page colour itself, exactly (no box round the picture)
  const near = new Map();
  const map = (k) => {
    let c = near.get(k);
    if (c === undefined) {
      let best = Infinity;
      for (const p of palette) {
        const d = ((p >> 16) - (k >> 16)) ** 2 + (((p >> 8) & 255) - ((k >> 8) & 255)) ** 2 + ((p & 255) - (k & 255)) ** 2;
        if (d < best) (best = d), (c = p);
      }
      near.set(k, c);
    }
    return c;
  };
  return raw.map((data) => {
    for (let j = 0; j < data.length; j += 3) {
      const c = map((data[j] << 16) | (data[j + 1] << 8) | data[j + 2]);
      data[j] = c >> 16; data[j + 1] = (c >> 8) & 255; data[j + 2] = c & 255;
    }
    return data;
  });
}

const browser = await chromium.launch();
for (const theme of THEMES) {
  const ctx = await browser.newContext({ viewport: { width: W, height: H }, deviceScaleFactor: DPR });
  const tab = await ctx.newPage();
  tab.on('pageerror', (e) => console.error(`pageerror ${e.message}`));
  await tab.route('https://readme.local/**', (r) => {
    const name = new URL(r.request().url()).pathname.slice(1);
    if (!name) return r.fulfill({ body: page(theme), contentType: 'text/html' });
    r.fulfill({ body: readFileSync(FILES[name]), contentType: name.endsWith('.wasm') ? 'application/wasm' : 'application/json' });
  });
  await tab.goto('https://readme.local/');
  await tab.evaluate(boot);
  const dt = 1000 / FPS;
  for (let t = 0; t < WARMUP * 1000; t += dt) await tab.evaluate((ms) => R.step(ms, true), dt); // the cat forms, unseen
  const cues = ROUND.flatMap((c) => c.cap === undefined ? [c] : [0.25, 0.5, 0.75, 1].map((k, i) => ({ t: c.t + i * 0.06, cap: c.cap, k })));
  let layer = 0;
  const pngs = [];
  for (let i = 0; i < LENGTH * FPS; i++) {
    for (const c of cues.filter((c) => c.t >= i / FPS - 1e-6 && c.t < (i + 1) / FPS - 1e-6)) {
      if (c.do) await tab.evaluate((d) => R.do(d), c.do);
      else {
        const [from, to] = [`cap${layer}`, `cap${1 - layer}`];
        await tab.evaluate(([f, t, h, k]) => R.cap(f, t, h, k), [from, to, c.k === 0.25 ? CAPTIONS[c.cap] : undefined, c.k]);
        if (c.k === 1) layer = 1 - layer;
      }
    }
    await tab.evaluate((ms) => R.step(ms), dt);
    pngs.push(await tab.screenshot());
  }
  // the seam: the round's last frames cross-fade into its first (both the cat, idle)
  const frames = await Promise.all(pngs.map((p) => sharp(p).removeAlpha().raw().toBuffer()));
  for (let i = 0; i < SEAM; i++) {
    const f = frames[frames.length - SEAM + i], k = (i + 1) / (SEAM + 1);
    for (let j = 0; j < f.length; j++) f[j] = Math.round(f[j] * (1 - k) + frames[0][j] * k);
  }
  const mixed = await quantize(frames, theme === 'dark' ? 0x0d1117 : 0xffffff);
  const file = `${OUT}/molty-hero${theme === 'dark' ? '' : '-light'}.webp`;
  await sharp(Buffer.concat(mixed), { raw: { width: W * DPR, height: H * DPR * mixed.length, channels: 3, pageHeight: H * DPR }, limitInputPixels: false })
    .webp({ lossless: true, effort: 6, minSize: true, loop: 0, delay: mixed.map(() => Math.round(dt)) })
    .toFile(file);
  console.log(`wrote ${file} (${mixed.length} frames, ${LENGTH} s at ${FPS} fps, ${(statSync(file).size / 1e6).toFixed(2)} MB)`);
  await ctx.close();
}
await browser.close();
