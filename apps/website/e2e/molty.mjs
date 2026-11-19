// The Molty on the landing, looked at and measured, against a running preview
// (`pnpm build && pnpm preview`):
//   screenshots   the hero and the moltys section at 1280 and 390 px wide, dark and light, once
//                 the hero's screen has booted to the cat: e2e-out/molty-<section>-<width>-<theme>.png
//   a tap         the stage's device every 250 ms: the cat is sent for a walk, then a tap on the
//                 glass calls it back to the front: e2e-out/molty-tap-NN.png, molty-tap-sheet.png,
//                 molty-tap.webp (the same frames, moving)
//   frame rate    frames the hero's screen draws a second (each putImageData on its canvas is one),
//                 the longest gap between two, and the main thread's time a frame; again with the
//                 CPU slowed 4x. Fails under 30 a second.
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

const browser = await chromium.launch();

async function open(width, height, theme, init) {
  const ctx = await browser.newContext({ viewport: { width, height }, deviceScaleFactor: 1, colorScheme: theme });
  const page = await ctx.newPage();
  page.on('pageerror', (e) => failures.push(`pageerror ${e.message}`));
  if (init) await page.addInitScript(init);
  await page.goto(BASE + '/', { waitUntil: 'networkidle' });
  await page.waitForSelector('html[data-fluid-moltys="live"]', { timeout: 20000 });
  await page.waitForTimeout(7000); // the device has turned in, booted ("molty.uno") and the cat is up
  return { ctx, page };
}

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

// a tap on the glass: the stage's cat is sent for a walk, then called back by a tap
{
  const { ctx, page } = await open(1280, 800, 'dark');
  await page.evaluate(() => {
    const el = document.getElementById('moltys');
    window.scrollTo(0, el.getBoundingClientRect().top + scrollY - innerHeight * 0.1);
  });
  await page.waitForTimeout(4000); // its device has turned in
  const stage = page.locator('[data-stage="gallery"]');
  const box = await stage.boundingBox();
  const clip = { x: box.x - box.width * 0.06, y: box.y - box.height * 0.06, width: box.width * 1.12, height: box.height * 1.12 };
  const frames = [];
  const shoot = async (n, ms = 250) => {
    for (let i = 0; i < n; i++, await page.waitForTimeout(ms)) frames.push(await page.screenshot({ clip }));
  };
  await shoot(2);
  await page.locator('section:has(#moltys) button').filter({ hasText: /^walk$/ }).click();
  await shoot(12); // 3 s: it gets up and walks off
  const tapAt = frames.length;
  await page.mouse.click(box.x + box.width * 0.5, box.y + box.height * 0.7);
  await shoot(18); // 4.5 s: the ripple, and it comes to the front
  const T = 240;
  const tiles = await Promise.all(frames.map((f) => sharp(f).resize(T, T).png().toBuffer()));
  await Promise.all(tiles.map((t, i) => sharp(t).toFile(`${OUT}molty-tap-${String(i).padStart(2, '0')}.png`)));
  const cols = 8;
  await sharp({ create: { width: T * cols, height: T * Math.ceil(tiles.length / cols), channels: 3, background: '#000' } })
    .composite(tiles.map((input, i) => ({ input, left: (i % cols) * T, top: Math.floor(i / cols) * T })))
    .png()
    .toFile(`${OUT}molty-tap-sheet.png`);
  await sharp(tiles, { join: { animated: true } }).webp({ delay: 250, loop: 0 }).toFile(`${OUT}molty-tap.webp`);
  // it moved: the frame as it walked is not the frame before, nor the one after it came back
  const differ = async (a, b) => {
    const [x, y] = await Promise.all([a, b].map((f) => sharp(f).raw().toBuffer()));
    let n = 0;
    for (let i = 0; i < x.length; i += 4) if (Math.abs(x[i] - y[i]) + Math.abs(x[i + 1] - y[i + 1]) > 60) n++;
    return n;
  };
  const walked = await differ(frames[1], frames[tapAt - 1]);
  const came = await differ(frames[tapAt - 1], frames.at(-1));
  if (walked < 800 || came < 800) failures.push(`the walk or the tap changed too little (${walked} px, ${came} px)`);
  else console.log(`ok   the cat walks off (${walked} px differ) and a tap brings it back (${came} px differ); ${frames.length} frames, the tap before frame ${tapAt}`);
  await ctx.close();
}

// frame rate
const counter = () => {
  const stamps = (window.__draws = []);
  const put = CanvasRenderingContext2D.prototype.putImageData;
  CanvasRenderingContext2D.prototype.putImageData = function (...args) {
    stamps.push(performance.now());
    return put.apply(this, args);
  };
};
for (const slow of [1, 4]) {
  const { ctx, page } = await open(1280, 800, 'dark', counter);
  const cdp = await ctx.newCDPSession(page);
  await cdp.send('Performance.enable');
  if (slow > 1) await cdp.send('Emulation.setCPUThrottlingRate', { rate: slow });
  await page.waitForTimeout(1000);
  const metric = async (name) => (await cdp.send('Performance.getMetrics')).metrics.find((m) => m.name === name).value;
  const SECONDS = 10;
  await page.evaluate(() => (window.__draws.length = 0));
  const busy0 = await metric('TaskDuration');
  await page.waitForTimeout(SECONDS * 1000);
  const busy = (await metric('TaskDuration')) - busy0;
  const stamps = await page.evaluate(() => window.__draws);
  const gaps = stamps.slice(1).map((t, i) => t - stamps[i]);
  const fps = (stamps.length - 1) / ((stamps.at(-1) - stamps[0]) / 1000);
  const worst = Math.max(...gaps);
  console.log(
    `${fps >= 30 ? 'ok  ' : 'FAIL'} frame rate${slow > 1 ? ` (CPU ${slow}x slower)` : ''}: ${fps.toFixed(1)} frames a second over ${SECONDS} s ` +
      `(${stamps.length} drawn, longest gap ${worst.toFixed(1)} ms, main thread busy ${((busy / stamps.length) * 1000).toFixed(2)} ms a frame, page fluid included)`,
  );
  if (fps < 30) failures.push(`frame rate ${fps.toFixed(1)}${slow > 1 ? ` at ${slow}x` : ''}`);
  await ctx.close();
}

// hidden: no frames while the screen is out of view
{
  const { ctx, page } = await open(1280, 800, 'dark', counter);
  await page.evaluate(() => window.scrollTo(0, document.body.scrollHeight));
  await page.waitForTimeout(1500);
  const n0 = await page.evaluate((sel) => {
    window.__hero = document.querySelector(sel + ' canvas');
    const put = CanvasRenderingContext2D.prototype.putImageData;
    window.__heroDraws = 0;
    CanvasRenderingContext2D.prototype.putImageData = function (...args) {
      if (this.canvas === window.__hero) window.__heroDraws++;
      return put.apply(this, args);
    };
    return 0;
  }, HERO);
  await page.waitForTimeout(2000);
  const n = (await page.evaluate(() => window.__heroDraws)) - n0;
  if (n > 0) failures.push(`the hero's screen drew ${n} frames while out of view`);
  else console.log('ok   out of view: the screen draws nothing');
  await ctx.close();
}

// reduced motion: one still frame, no frames after it
{
  const ctx = await browser.newContext({ viewport: { width: 1280, height: 800 }, deviceScaleFactor: 1, colorScheme: 'dark', reducedMotion: 'reduce' });
  const page = await ctx.newPage();
  await page.addInitScript(counter);
  await page.goto(BASE + '/', { waitUntil: 'networkidle' });
  await page.waitForSelector(`${HERO}[data-live]`, { timeout: 20000 });
  await page.waitForTimeout(1500);
  const a = await page.evaluate(() => window.__draws.length);
  await page.waitForTimeout(2000);
  const b = await page.evaluate(() => window.__draws.length);
  await page.screenshot({ path: `${OUT}molty-hero-1280-dark-reduced.png` });
  if (b !== a) failures.push(`reduced motion: ${b - a} frames drawn in 2 s`);
  else console.log(`ok   reduced motion: a still frame (${a} draws in all, none after)`);
  await ctx.close();
}

await browser.close();
if (failures.length) {
  console.error(`\n${failures.length} failure(s):\n${failures.join('\n')}`);
  process.exitCode = 1;
} else console.log(`\nall Molty checks passed; pictures in ${OUT}`);
