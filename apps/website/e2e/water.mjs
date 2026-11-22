// The page's water, looked at and checked, against a running preview (`pnpm build && pnpm preview`).
// The display's frames are run by hand here (one requestAnimationFrame round at a time, 1/60 s
// each), so a strip shows the frames it says and a front is where its speed puts it.
//   one front    a tap on the page beside the hero's device: the water in the glass answers when
//                the front gets to its rim, not before; the strip has the front half in the glass
//                and half out: e2e-out/water-rim-sheet.png
//   headings     every big dust text of the site (the landing's title and section headings, the
//                showcase's, the build page's): a tap beside it loosens its lines as the front
//                arrives, and they sharpen back
//   the title    taps from six places (on it, beside it, far left, far right, inside the hero's
//                screen, inside another section's screen): it answers each as the front arrives;
//                the strip is the far right one: e2e-out/water-title-sheet.png
//   a change     on the showcase, the cat to the chart and a tapped sign's close-up: the picture
//                changes as a wave through its dust, from the middle and from the finger, never
//                as a cut: e2e-out/water-change-sheet.png
//   node e2e/water.mjs [baseUrl=http://localhost:4330]
import { chromium } from 'playwright';
import sharp from 'sharp';
import { mkdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

const BASE = process.argv[2] ?? 'http://localhost:4330';
const OUT = fileURLToPath(new URL('../e2e-out/', import.meta.url));
mkdirSync(OUT, { recursive: true });
const failures = [];
const pass = (ok, line) => (console.log(`${ok ? 'ok  ' : 'FAIL'} ${line}`), ok || failures.push(line));

/** Runs in the page from document start. */
function tools() {
  // the display's frames by hand once __manual is set: __pump(n) runs n of them, 1/60 s apart
  const real = window.requestAnimationFrame.bind(window);
  const cancel = window.cancelAnimationFrame.bind(window);
  let queue = [],
    id = 0,
    now = 0;
  window.requestAnimationFrame = (cb) => (window.__manual ? (queue.push([++id, cb]), id) : real(cb));
  window.cancelAnimationFrame = (h) => {
    queue = queue.filter(([i]) => i !== h);
    cancel(h);
  };
  window.__pump = (n = 1) => {
    for (let f = 0; f < n; f++) {
      now = (now || performance.now()) + 1000 / 60;
      const run = queue;
      queue = [];
      for (const [, cb] of run) cb(now);
    }
  };
  const pixels = (c) => c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
  const light = (px, i) => Math.max(px[i], px[i + 1], px[i + 2]);
  // a heading's word: its lines' pixels on flat water (its colour at its strongest), where the
  // nearest of them is from a point of the viewport, and how many of them still stand
  const words = (window.__words = new Map());
  window.__word = (n) => {
    const c = document.querySelectorAll('[data-dust-focus="word"]')[n].querySelector('canvas');
    const px = pixels(c);
    let full = 0;
    for (let i = 0; i < px.length; i += 4) full = Math.max(full, light(px, i));
    const at = [];
    for (let i = 0; i < px.length; i += 4) if (light(px, i) >= 0.7 * full) at.push(i);
    words.set(n, { c, at, full });
    return at.length;
  };
  window.__nearest = (n, x, y) => {
    const { c, at } = words.get(n);
    const r = c.getBoundingClientRect(),
      k = r.width / c.width;
    let best = Infinity;
    for (const i of at) best = Math.min(best, Math.hypot(r.left + ((i / 4) % c.width) * k - x, r.top + Math.floor(i / 4 / c.width) * k - y));
    return best;
  };
  window.__kept = (n) => {
    const { c, at, full } = words.get(n);
    const px = pixels(c);
    let kept = 0;
    for (const i of at) if (light(px, i) >= 0.5 * full) kept++;
    return kept / at.length;
  };
  // the page's own canvas: how much of it the water has moved
  window.__moved = () => {
    const c = document.querySelector('[data-page-water]');
    const px = pixels(c);
    let n = 0;
    for (let i = 3; i < px.length; i += 4) if (px[i]) n++;
    return n;
  };
  // a screen's picture: kept, and how many pixels of a part of it (fractions of its side) differ from the kept one
  let kept = null;
  window.__keep = (sel) => (kept = pixels(document.querySelector(sel)).slice());
  window.__differ = (sel, x0 = 0, y0 = 0, x1 = 1, y1 = 1) => {
    const c = document.querySelector(sel);
    const px = pixels(c),
      w = c.width;
    let n = 0,
      sx = 0,
      sy = 0;
    for (let y = Math.floor(y0 * w); y < y1 * w; y++)
      for (let x = Math.floor(x0 * w); x < x1 * w; x++) {
        const i = (y * w + x) * 4;
        if (px[i] !== kept[i] || px[i + 1] !== kept[i + 1] || px[i + 2] !== kept[i + 2]) {
          n++;
          sx += x;
          sy += y;
        }
      }
    return { n, x: n ? sx / n / w : 0, y: n ? sy / n / w : 0 };
  };
}

const browser = await chromium.launch();
async function open(path, width, height, wait = 9000) {
  const ctx = await browser.newContext({ viewport: { width, height }, deviceScaleFactor: 1, colorScheme: 'dark' });
  const page = await ctx.newPage();
  page.on('pageerror', (e) => failures.push(`${path}: pageerror ${e.message}`));
  await page.addInitScript(tools);
  await page.goto(BASE + path, { waitUntil: 'commit' });
  await page.waitForTimeout(wait);
  return { ctx, page };
}
const pump = (page, n = 1) => page.evaluate((n) => window.__pump(n), n);
/** A wave's speed on this page, CSS px a frame (services/page-water: the device's own on the page's reference screen). */
const speed = (page) => page.evaluate(() => (Math.sqrt(0.11) * ((466 * parseFloat(getComputedStyle(document.querySelector('[data-water]')).backgroundSize)) / 1040)) / 59);
/** Frames until the water is flat again (the page's canvas clear), at most `most`. */
async function calm(page, most = 1500) {
  for (let f = 0; f < most; f += 30) {
    await pump(page, 30);
    if ((await page.evaluate(() => window.__moved())) === 0) return f;
  }
  return most;
}
async function sheet(shots, cols, tile, path) {
  const meta = await sharp(shots[0]).metadata();
  const th = Math.round((tile * meta.height) / meta.width);
  const tiles = await Promise.all(shots.map((b) => sharp(b).resize(tile, th).png().toBuffer()));
  await sharp({ create: { width: tile * cols, height: th * Math.ceil(tiles.length / cols), channels: 3, background: '#202020' } })
    .composite(tiles.map((input, i) => ({ input, left: (i % cols) * tile, top: Math.floor(i / cols) * th })))
    .png()
    .toFile(OUT + path);
}

/** A tap at (x, y), then the heading `n` watched: its lines stand until the front is due at the
 *  nearest of them, loosen as it passes, and are back once the water is flat. `shots`: a picture
 *  of `clip` every this many frames from a little before the front is due. */
async function rides(page, n, x, y, what, shots) {
  const px = await speed(page);
  await page.evaluate((n) => window.__word(n), n);
  const due = (await page.evaluate(([n, x, y]) => window.__nearest(n, x, y), [n, x, y])) / px;
  await page.mouse.click(x, y);
  let before = 1,
    least = 1,
    when = 0;
  const pictures = [];
  for (let f = 0; f < due + 300; f += 4) {
    await pump(page, 4);
    const k = await page.evaluate((n) => window.__kept(n), n);
    if (f < due - 40) before = Math.min(before, k);
    else if (k < least) [least, when] = [k, f];
    if (shots && f > due - 30 && pictures.length < shots.n && (f - Math.ceil(due - 30)) % shots.every < 4) pictures.push(await page.screenshot({ clip: shots.clip }));
  }
  await calm(page);
  await pump(page, 60);
  const after = await page.evaluate((n) => window.__kept(n), n);
  if (shots) await sheet(pictures, shots.cols, shots.tile, shots.path);
  const ok = before > 0.99 && least < 0.985 && least > 0.3 && after > 0.99;
  pass(
    ok,
    `${what}: its lines stand until the front is due (${(due / 60).toFixed(1)} s; ${(before * 100).toFixed(1)}% before), ` +
      `loosen as it passes (${(least * 100).toFixed(1)}% stand at the least, ${(when / 60).toFixed(1)} s after the tap), and are back (${(after * 100).toFixed(1)}%)`,
  );
  return ok;
}

// ---------------------------------------------------------------- one front at the hero's rim
{
  const { ctx, page } = await open('/', 1280, 800);
  await page.evaluate(() => (window.__manual = true));
  await pump(page, 30);
  const HERO = '[data-molty-screen] canvas:not([data-outside])';
  const box = await page.locator(HERO).first().boundingBox();
  const px = await speed(page);
  const tap = [box.x - box.width * 0.5, box.y + box.height * 0.62];
  // the floor at the glass's lower left (nothing of the cat there): calm before the front is due at the rim
  const part = [HERO, 0.08, 0.6, 0.4, 0.85];
  const due = (box.width * 0.5 + box.width * 0.08) / px;
  await page.evaluate((sel) => window.__keep(sel), HERO);
  await pump(page, 20);
  const idle = (await page.evaluate((a) => window.__differ(...a), part)).n;
  await page.mouse.click(...tap);
  await page.evaluate((sel) => window.__keep(sel), HERO);
  const clip = { x: Math.max(0, tap[0] - 60), y: box.y - 30, width: Math.min(1280, box.x + box.width + 20) - Math.max(0, tap[0] - 60), height: box.height + 60 };
  const shots = [];
  let early = 0,
    late = 0,
    moved = 0;
  for (let f = 0; f < due + 160; f += 8) {
    await pump(page, 8);
    const d = (await page.evaluate((a) => window.__differ(...a), part)).n;
    if (f < due - 30) early = Math.max(early, d);
    else late = Math.max(late, d);
    moved = Math.max(moved, await page.evaluate(() => window.__moved()));
    if (f % 24 === 0 && shots.length < 12) shots.push(await page.screenshot({ clip }));
  }
  await sheet(shots, 3, Math.round(clip.width * 1.5), 'water-rim-sheet.png');
  pass(moved > 2000, `a tap on the page moves its dust (${moved} px of the page's canvas at the most)`);
  pass(
    early <= idle + 40 && late > early + 150,
    `the water in the hero's glass answers when the front gets to its rim: ${early} px of its floor moved before the front was due (${idle} idle), ${late} after`,
  );
  const left = await calm(page);
  pass(left < 1500 && (await page.evaluate(() => window.__moved())) === 0, `the water is flat again and the page's canvas clear ${(left / 60).toFixed(0)} s later`);
  await ctx.close();
}

// ---------------------------------------------------------------- every heading rides the page's water
const headings = [];
for (const [path, width, height] of [
  ['/', 1280, 800],
  ['/showcase/', 1280, 800],
  ['/build/', 1280, 800],
]) {
  const { ctx, page } = await open(path, width, height, 5000);
  const names = await page.evaluate(() => [...document.querySelectorAll('[data-dust-focus="word"]')].map((h) => h.childNodes[0].textContent.trim()));
  for (let n = 0; n < names.length; n++) {
    await page.evaluate((n) => {
      window.__manual = false;
      const h = document.querySelectorAll('[data-dust-focus="word"]')[n];
      window.scrollTo(0, h.getBoundingClientRect().top + scrollY - innerHeight * 0.4);
    }, n);
    await page.waitForFunction((n) => document.querySelectorAll('[data-dust-focus="word"]')[n].querySelector('[data-word="live"]'), n, { timeout: 20000 }).catch(() => {});
    await page.waitForTimeout(2500);
    await page.evaluate(() => (window.__manual = true));
    await pump(page, 90);
    const r = await page.evaluate((n) => {
      const b = document.querySelectorAll('[data-dust-focus="word"]')[n].getBoundingClientRect();
      return [b.left, b.right, b.top + b.height / 2];
    }, n);
    // beside it: 160 px off its nearer free side
    const x = r[1] + 160 < width - 10 ? r[1] + 160 : r[0] - 160;
    headings.push([`${path} "${names[n]}"`, await rides(page, n, x, r[2], `${path} "${names[n]}", a tap beside it`)]);
  }
  await ctx.close();
}

// ---------------------------------------------------------------- the title, from six places
const places = [];
const where = (page) =>
  page.evaluate(() => {
    const b = document.querySelector('[data-dust-focus="word"]').getBoundingClientRect();
    const screens = [...document.querySelectorAll('[data-molty-screen]')].map((s) => s.getBoundingClientRect()).map((b) => ({ x: b.left + b.width / 2, top: b.top, y: b.top + b.height / 2 }));
    return { title: { left: b.left, right: b.right, mid: (b.top + b.bottom) / 2 }, screens };
  });
{
  const { ctx, page } = await open('/', 1280, 800);
  await page.evaluate(() => (window.__manual = true));
  await pump(page, 60);
  const { title, screens } = await where(page);
  for (const [what, x, y, shots] of [
    ['on it', (title.left + title.right) / 2, title.mid],
    ['beside it', title.right + 120, title.mid],
    ['far left', 8, 760],
    ['far right', 1270, title.mid + 60, { n: 12, every: 30, cols: 3, tile: 840, path: 'water-title-sheet.png', clip: { x: 0, y: 60, width: 1280, height: 420 } }],
    ["inside the hero's screen", screens[0].x, screens[0].y - 40],
  ])
    places.push([what, await rides(page, 0, x, y, `the title, a tap ${what} (${Math.round(x)}, ${Math.round(y)})`, shots)]);
  await ctx.close();
}
{
  // the next section's screen and the title share a short window, scrolled till the screen's top is in it
  const { ctx, page } = await open('/', 1280, 640, 5000);
  await page.evaluate(() => {
    const s = document.querySelectorAll('[data-molty-screen]')[1].getBoundingClientRect();
    window.scrollTo(0, s.top + scrollY - innerHeight + 70);
  });
  await page.waitForTimeout(4000); // the talk section's device turns in and its screen comes on
  await page.evaluate(() => (window.__manual = true));
  await pump(page, 60);
  const { screens } = await where(page);
  const [x, y] = [screens[1].x, screens[1].top + 50];
  places.push(["inside the talk section's screen", await rides(page, 0, x, y, `the title, a tap inside the talk section's screen (${Math.round(x)}, ${Math.round(y)})`)]);
  await ctx.close();
}

// ---------------------------------------------------------------- a change of picture is a wave through the dust
{
  const { ctx, page } = await open('/showcase/', 1280, 900, 6000);
  const SCREEN = '[data-stage="showcase"] canvas:not([data-outside])';
  const chart = page.getByRole('button', { name: 'chart' });
  await chart.scrollIntoViewIfNeeded();
  await page.waitForTimeout(500);
  await page.evaluate(() => (window.__manual = true));
  await pump(page, 30);
  const box = await page.locator(SCREEN).first().boundingBox();
  const clip = { x: box.x - 20, y: box.y - 20, width: box.width + 40, height: box.height + 40 };
  const shots = [];
  /** The frames of a change: how much of the glass differs from the picture before it, frame by frame. */
  const change = async (start, frames, part = [0, 0, 1, 1]) => {
    await page.evaluate((sel) => window.__keep(sel), SCREEN);
    await start();
    const seen = [];
    for (let f = 0; f < frames; f++) {
      await pump(page, 1);
      seen.push(await page.evaluate((a) => window.__differ(...a), [SCREEN, ...part]));
      if (f % 3 === 1 && f < 36) shots.push(await page.screenshot({ clip }));
    }
    return seen;
  };
  // the cat to the chart: from the glass's middle
  const toChart = await change(() => chart.click(), 90);
  // a tapped sign: from the finger (the sign at the chart's upper left; its reading's words come at the foot of the
  // glass, so the change is watched in the upper half)
  await pump(page, 120);
  const at = [0.4, 0.095];
  const toSign = await change(() => page.mouse.click(box.x + box.width * at[0], box.y + box.height * at[1]), 90, [0, 0, 1, 0.55]);
  await sheet(shots, 12, 200, 'water-change-sheet.png');
  for (const [what, seen, from] of [
    ['the cat to the chart', toChart, [0.5, 0.5]],
    ["a tapped sign's close-up", toSign, at],
  ]) {
    const all = Math.max(...seen.map((s) => s.n));
    // frames in which the change is part way: a cut has none
    const part = seen.filter((s) => s.n > 0.1 * all && s.n < 0.9 * all).length;
    // ... and where it begins: the first frame a good many pixels have changed
    const first = seen.find((s) => s.n > 0.02 * all) ?? { x: 0, y: 0 };
    const off = Math.hypot(first.x - from[0], first.y - from[1]);
    pass(part >= 8 && off < 0.2, `${what}: a wave through the dust, part way in ${part} frames (a cut: 0), beginning ${(off * 100).toFixed(0)}% of the glass from ${from[0] === 0.5 ? 'its middle' : 'the finger'}`);
  }
  await ctx.close();
}

await browser.close();
console.log(`\nheadings: ${headings.map(([name, ok]) => `${name} ${ok ? 'pass' : 'FAIL'}`).join(', ')}`);
console.log(`the title from six places: ${places.map(([name, ok]) => `${name} ${ok ? 'pass' : 'FAIL'}`).join(', ')}`);
if (failures.length) {
  console.error(`\n${failures.length} failure(s):\n${failures.join('\n')}`);
  process.exitCode = 1;
} else console.log(`\nthe page's water passed; strips in ${OUT}`);
