// The landing, looked at and measured, against a running preview (`pnpm build && pnpm preview`):
//   screenshots   the hero and the moltys section at 1280 and 390 px wide, dark and light, once
//                 the cat has walked in and sat: e2e-out/molty-<section>-<width>-<theme>.png
//   the entrance  the whole window from the load on, a frame every 200 ms, at 1280 and 390 px:
//                 the device turns in, the cat walks in from beyond the window's right edge,
//                 across the page, into its screen, and sits: e2e-out/molty-entrance-<width>-sheet.png
//   one water     a tap inside the hero's screen, a frame every 200 ms: the ring crosses the rim
//                 and goes on through the page (e2e-out/molty-ripple-sheet.png); then a tap on
//                 the page beside it: the ring goes through the device and on
//                 (molty-through-sheet.png, molty-through.webp)
//   frame rate    frames the hero's screen draws a second (each putImageData on its canvas is
//                 one) while the page is tapped every 2 s, the longest gap between two, and the
//                 main thread's time a frame: at 1280 and 390 px, as is and with the CPU slowed
//                 4x. Fails under 30 a second.
//   also          nothing draws out of view; reduced motion draws stills only.
//   node e2e/molty.mjs [baseUrl=http://localhost:4330]
import { chromium } from 'playwright';
import sharp from 'sharp';
import { mkdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

const BASE = process.argv[2] ?? 'http://localhost:4330';
const OUT = fileURLToPath(new URL('../e2e-out/', import.meta.url));
mkdirSync(OUT, { recursive: true });
const failures = [];
const HERO = '[data-molty-screen]:has([data-device="arrive"])';
const SETTLED = 9000; // the device has turned in, the cat has walked in and sat

const browser = await chromium.launch();

/** Every putImageData, with what drew: "screen" (a Molty screen), "outside", "word", "water". */
const counter = () => {
  const draws = (window.__draws = []);
  const put = CanvasRenderingContext2D.prototype.putImageData;
  CanvasRenderingContext2D.prototype.putImageData = function (...args) {
    const c = this.canvas;
    const what = c.hasAttribute('data-outside') ? 'outside' : c.closest('[data-molty-screen]') ? 'screen' : c.closest('[data-molty-word]') ? 'word' : 'water';
    const draw = { t: performance.now(), what };
    if (what === 'outside') {
      // the cat alone: where its leftmost dot is on the page (its nose, walking left), if it has any
      const d = args[0].data,
        w = args[0].width;
      let min = Infinity;
      for (let i = 3; i < d.length; i += 4) if (d[i] && (i >> 2) % w < min) min = (i >> 2) % w;
      const r = c.getBoundingClientRect();
      draw.nose = min === Infinity ? null : r.left + (min * r.width) / w;
      draw.wide = document.documentElement.scrollWidth > document.documentElement.clientWidth;
    }
    draws.push(draw);
    return put.apply(this, args);
  };
};

async function open(width, height, theme, { wait = SETTLED, reducedMotion = 'no-preference' } = {}) {
  const ctx = await browser.newContext({ viewport: { width, height }, deviceScaleFactor: 1, colorScheme: theme, reducedMotion });
  const page = await ctx.newPage();
  page.on('pageerror', (e) => failures.push(`pageerror ${e.message}`));
  await page.addInitScript(counter);
  await page.goto(BASE + '/', { waitUntil: 'commit' });
  if (wait) await page.waitForTimeout(wait);
  return { ctx, page };
}
/** Frames of `clip`, every `ms`, `n` of them, as tiles `tile` px wide. */
async function strip(page, clip, n, ms, tile) {
  const frames = [];
  for (let i = 0; i < n; i++, await page.waitForTimeout(ms)) frames.push(await page.screenshot({ clip }));
  const h = Math.round((tile * clip.height) / clip.width);
  return { frames, tiles: await Promise.all(frames.map((f) => sharp(f).resize(tile, h).png().toBuffer())), w: tile, h };
}
async function sheet({ tiles, w, h }, cols, path) {
  await sharp({ create: { width: w * cols, height: h * Math.ceil(tiles.length / cols), channels: 3, background: '#000' } })
    .composite(tiles.map((input, i) => ({ input, left: (i % cols) * w, top: Math.floor(i / cols) * h })))
    .png()
    .toFile(path);
}
const differ = async (a, b) => {
  const [x, y] = await Promise.all([a, b].map((f) => sharp(f).raw().toBuffer()));
  let n = 0;
  for (let i = 0; i < x.length; i += 4) if (Math.abs(x[i] - y[i]) + Math.abs(x[i + 1] - y[i + 1]) + Math.abs(x[i + 2] - y[i + 2]) > 50) n++;
  return n;
};

// screenshots
for (const theme of ['dark', 'light'])
  for (const [width, height] of [
    [1280, 800],
    [390, 844],
  ]) {
    const { ctx, page } = await open(width, height, theme);
    await page.screenshot({ path: `${OUT}molty-hero-${width}-${theme}.png` });
    await page.evaluate(() => {
      const el = document.getElementById('moltys');
      window.scrollTo(0, el.getBoundingClientRect().top + scrollY - innerHeight * 0.1);
    });
    await page.waitForTimeout(4500);
    await page.screenshot({ path: `${OUT}molty-moltys-${width}-${theme}.png` });
    console.log(`ok   screenshots ${width} ${theme}`);
    await ctx.close();
  }

// the entrance: from the load on, the whole window in every frame
for (const [width, height] of [
  [1280, 800],
  [390, 844],
]) {
  const { ctx, page } = await open(width, height, 'dark', { wait: 0 });
  await page.waitForSelector(HERO);
  const s = await strip(page, { x: 0, y: 0, width, height }, 34, 200, width > 600 ? 320 : 160);
  await sheet(s, width > 600 ? 6 : 12, `${OUT}molty-entrance-${width}-sheet.png`);
  if (width > 600) await sharp(s.tiles, { join: { animated: true } }).webp({ delay: 200, loop: 0 }).toFile(`${OUT}molty-entrance.webp`);
  await page.waitForTimeout(3000);
  const outside = (await page.evaluate(() => window.__draws)).filter((d) => d.what === 'outside' && d.nose !== null);
  const first = outside[0];
  const crossing = outside.find((d) => d.nose < width);
  const seconds = crossing ? (outside.at(-1).t - crossing.t) / 1000 : 0;
  const box = await page.locator(HERO).boundingBox();
  // it starts wholly beyond the window's edge, its nose crosses it, it ends in the screen; the page never grows wider
  const ok = first && first.nose >= width && crossing && outside.at(-1).nose > box.x && outside.at(-1).nose < box.x + box.width && !outside.some((d) => d.wide);
  if (!ok) failures.push(`the entrance at ${width} px: first nose at ${first?.nose?.toFixed(0)} (window ${width}), crossing ${!!crossing}, ends at ${outside.at(-1)?.nose?.toFixed(0)}, page wider ${outside.some((d) => d.wide)}`);
  else console.log(`ok   the entrance at ${width} px: the cat starts beyond the window's edge (nose at ${first.nose.toFixed(0)} px), walks in and sits in its screen ${seconds.toFixed(1)} s after its nose crosses the edge; no scrollbar`);
  await ctx.close();
}

// one water: a tap inside the screen goes on through the page, a tap on the page goes through the screen
{
  const { ctx, page } = await open(1280, 800, 'dark');
  const box = await page.locator(HERO).boundingBox();
  const cx = box.x + box.width / 2,
    cy = box.y + box.height / 2,
    r = box.width / 2;
  /** The page's ring: its lit pixels outside the device, and how many of them lie right of the device's middle. */
  const ring = () =>
    page.evaluate(
      ([cx, cy, r]) => {
        const c = document.querySelector('[data-page-water]');
        const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
        let out = 0,
          past = 0;
        for (let y = 0; y < c.height; y++)
          for (let x = 0; x < c.width; x++)
            if (d[(y * c.width + x) * 4 + 3] && Math.hypot(x - cx, y - cy) > 1.1 * r) {
              out++;
              if (x > cx) past++;
            }
        return { out, past };
      },
      [cx, cy, r],
    );
  const wide = { x: box.x - box.width * 0.95, y: box.y - box.height * 0.45, width: 1280 - (box.x - box.width * 0.95), height: box.height * 1.6 };
  // inside the glass: the ring crosses the rim outward
  await page.mouse.click(cx, cy + r * 0.1);
  const inside = await strip(page, wide, 12, 200, 320);
  const a = await ring();
  await sheet(inside, 6, `${OUT}molty-ripple-sheet.png`);
  await page.waitForTimeout(7000);
  // on the page, left of the device: the ring reaches the device, goes through it and on
  await page.mouse.click(box.x - box.width * 0.6, cy);
  await page.waitForTimeout(3600); // its ring is past the device's middle, above and below it
  const b = await ring();
  await page.waitForTimeout(8000);
  await page.mouse.click(box.x - box.width * 0.6, cy);
  const through = await strip(page, wide, 24, 200, 320);
  await sheet(through, 6, `${OUT}molty-through-sheet.png`);
  await sharp(through.tiles, { join: { animated: true } }).webp({ delay: 200, loop: 0 }).toFile(`${OUT}molty-through.webp`);
  if (a.out < 300) failures.push(`a tap inside the screen did not go on through the page (${a.out} px of its ring outside the device)`);
  else if (b.past < 100) failures.push(`a tap on the page did not get past the device (${b.past} px of its ring beyond the device's middle)`);
  else console.log(`ok   one water: a tap inside the screen crosses the rim (${a.out} px of its ring outside), a tap on the page passes the device (${b.past} px beyond its middle)`);
  await ctx.close();
}

// frame rate
for (const [width, height] of [
  [1280, 800],
  [390, 844],
])
  for (const slow of [1, 4]) {
    const { ctx, page } = await open(width, height, 'dark');
    const cdp = await ctx.newCDPSession(page);
    await cdp.send('Performance.enable');
    if (slow > 1) await cdp.send('Emulation.setCPUThrottlingRate', { rate: slow });
    await page.waitForTimeout(1000);
    const metric = async (name) => (await cdp.send('Performance.getMetrics')).metrics.find((m) => m.name === name).value;
    const SECONDS = 10;
    await page.evaluate(() => (window.__draws.length = 0));
    const busy0 = await metric('TaskDuration');
    for (let i = 0; i < SECONDS / 2; i++) {
      await page.mouse.click(width * 0.3, height * 0.75); // a wave on the page all the while
      await page.waitForTimeout(2000);
    }
    const busy = (await metric('TaskDuration')) - busy0;
    const stamps = (await page.evaluate(() => window.__draws)).filter((d) => d.what === 'screen').map((d) => d.t);
    const gaps = stamps.slice(1).map((t, i) => t - stamps[i]);
    const fps = (stamps.length - 1) / ((stamps.at(-1) - stamps[0]) / 1000);
    console.log(
      `${fps >= 30 ? 'ok  ' : 'FAIL'} frame rate at ${width} px${slow > 1 ? `, CPU ${slow}x slower` : ''}: ${fps.toFixed(1)} frames a second over ${SECONDS} s ` +
        `(longest gap ${Math.max(...gaps).toFixed(1)} ms, main thread busy ${((busy / stamps.length) * 1000).toFixed(2)} ms a frame: the screen, the title, the page's waves)`,
    );
    if (fps < 30) failures.push(`frame rate ${fps.toFixed(1)} at ${width} px${slow > 1 ? `, ${slow}x` : ''}`);
    await ctx.close();
  }

// out of view: nothing draws
{
  const { ctx, page } = await open(1280, 800, 'dark');
  await page.locator('footer').last().scrollIntoViewIfNeeded();
  await page.waitForTimeout(6000); // the last section's own screen has come on and its question stands
  await page.evaluate(() => window.scrollTo(0, document.body.scrollHeight));
  await page.waitForTimeout(500);
  const top = await page.evaluate(() => {
    const visible = (c) => {
      const r = c.getBoundingClientRect();
      return r.bottom > 0 && r.top < innerHeight;
    };
    window.__unseen = [...document.querySelectorAll('canvas')].filter((c) => !visible(c) && !c.hasAttribute('data-page-water'));
    const put = CanvasRenderingContext2D.prototype.putImageData;
    window.__unseenDraws = 0;
    CanvasRenderingContext2D.prototype.putImageData = function (...args) {
      if (window.__unseen.includes(this.canvas)) window.__unseenDraws++;
      return put.apply(this, args);
    };
    return window.__unseen.length;
  });
  await page.waitForTimeout(2000);
  const n = await page.evaluate(() => window.__unseenDraws);
  if (n > 0) failures.push(`${n} frames drawn on canvases out of view`);
  else console.log(`ok   out of view: none of the ${top} canvases off screen draws`);
  await ctx.close();
}

// reduced motion: stills only
{
  const { ctx, page } = await open(1280, 800, 'dark', { reducedMotion: 'reduce', wait: 0 });
  await page.waitForSelector(`${HERO}[data-screen="live"]`, { timeout: 20000 });
  await page.waitForTimeout(2500);
  const a = await page.evaluate(() => window.__draws.length);
  await page.mouse.click(30, 780); // on the bare page: no wave
  await page.waitForTimeout(2000);
  const b = await page.evaluate(() => window.__draws.length);
  await page.screenshot({ path: `${OUT}molty-hero-1280-dark-reduced.png` });
  const entrance = await page.evaluate(() => window.__draws.filter((d) => d.what === 'outside' || d.what === 'water').length);
  if (b !== a || entrance) failures.push(`reduced motion: ${b - a} frames drawn in 2 s, ${entrance} of the entrance or a wave`);
  else console.log(`ok   reduced motion: a still page (${a} stills drawn, none after; the cat is simply there)`);
  await ctx.close();
}

await browser.close();
if (failures.length) {
  console.error(`\n${failures.length} failure(s):\n${failures.join('\n')}`);
  process.exitCode = 1;
} else console.log(`\nall Molty checks passed; pictures in ${OUT}`);
