// Nothing of a screen before its device is in place, against a running preview
// (`pnpm build && pnpm preview`). On a cold cache, a slow network and a slow CPU, at 1280 and
// 390 px wide, every frame of the load, of a scroll through every section and of a theme switch
// is sampled (requestAnimationFrame, from document start); once more at 1280 with nothing slowed. For every Molty screen on the page:
//   - its canvas and its still show only while its device stands at rest, fully there (opacity
//     1, no transform, not turning): the frame first, then the screen inside it
//   - once it shows, it never moves or changes size, and never goes away again (no flash)
// Also saves the load as a strip, a frame every 100 ms: e2e-out/in-place-<label>-<width>.png
//   node e2e/in-place.mjs [baseUrl=http://localhost:4330] [label=after]
import { chromium } from 'playwright';
import sharp from 'sharp';
import { mkdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

const BASE = process.argv[2] ?? 'http://localhost:4330';
const LABEL = process.argv[3] ?? 'after';
const OUT = fileURLToPath(new URL('../e2e-out/', import.meta.url));
mkdirSync(OUT, { recursive: true });
const failures = [];

/** Runs in the page from document start: each frame, each screen's state. */
function sampler() {
  const frames = (window.__screens = []);
  const opacity = (el) => {
    let o = 1;
    for (let e = el; e; e = e.parentElement) o *= Number(getComputedStyle(e).opacity);
    return o;
  };
  const tick = () => {
    const t = Math.round(performance.now());
    document.querySelectorAll('[data-molty-screen]').forEach((root, i) => {
      const canvas = root.querySelector('canvas:not([data-outside])');
      const still = root.querySelector('img');
      const device = root.querySelector('[data-device]');
      const face = device?.children[device.children.length - 1];
      if (!canvas || !still || !device) return;
      const r = canvas.getBoundingClientRect();
      frames.push({
        t,
        i,
        canvas: opacity(canvas),
        still: opacity(still),
        device: opacity(device),
        rest: !device.style.transform && !device.hasAttribute('data-turning') && !(face && face.style.transform),
        box: [Math.round(r.left + scrollX), Math.round(r.top + scrollY), Math.round(r.width), Math.round(r.height)].join(','),
      });
    });
    requestAnimationFrame(tick);
  };
  requestAnimationFrame(tick);
}

const browser = await chromium.launch();
for (const [width, height, slow] of [
  [1280, 800, true],
  [390, 844, true],
  [1280, 800, false], // a warm, fast machine: the renderer is there before the device has turned in
]) {
  const ctx = await browser.newContext({ viewport: { width, height }, deviceScaleFactor: 1, colorScheme: 'dark' });
  const page = await ctx.newPage();
  page.on('pageerror', (e) => failures.push(`${width}: pageerror ${e.message}`));
  const cdp = await ctx.newCDPSession(page);
  if (slow) {
    await cdp.send('Network.enable');
    await cdp.send('Network.emulateNetworkConditions', { offline: false, latency: 150, downloadThroughput: 1.6e6 / 8, uploadThroughput: 750e3 / 8 });
    await cdp.send('Emulation.setCPUThrottlingRate', { rate: 4 });
  }
  await page.addInitScript(sampler);
  // the load, as the screen showed it
  const shots = [];
  cdp.on('Page.screencastFrame', (f) => {
    shots.push({ at: Date.now(), data: f.data });
    cdp.send('Page.screencastFrameAck', { sessionId: f.sessionId }).catch(() => {});
  });
  await cdp.send('Page.startScreencast', { format: 'jpeg', quality: 70, everyNthFrame: 1 });
  const t0 = Date.now();
  await page.goto(BASE + '/', { waitUntil: 'commit' });
  await page.waitForTimeout(9000);
  await cdp.send('Page.stopScreencast');
  // every other screen: scroll through the page a section at a time
  const total = await page.evaluate(() => document.documentElement.scrollHeight);
  for (let y = 0; y < total; y += height * 0.5) {
    await page.evaluate((to) => window.scrollTo({ top: to, behavior: 'smooth' }), y);
    await page.waitForTimeout(2200);
  }
  // the theme switch, with a screen in view
  await page.evaluate(() => window.scrollTo(0, 0));
  await page.waitForTimeout(1500);
  await page.locator('button[aria-label*="theme" i]').first().click();
  await page.waitForTimeout(2500);

  const frames = await page.evaluate(() => window.__screens);
  const screens = [...new Set(frames.map((f) => f.i))];
  let bad = 0;
  for (const i of screens) {
    const own = frames.filter((f) => f.i === i);
    const early = own.filter((f) => Math.max(f.canvas, f.still) > 0.02 && !(f.device > 0.99 && f.rest));
    if (early.length) {
      bad++;
      const f = early[0];
      failures.push(`${width}: screen ${i} shows before its device is in place in ${early.length} frames (first at ${f.t} ms: canvas ${f.canvas.toFixed(2)}, still ${f.still.toFixed(2)}, device ${f.device.toFixed(2)}, at rest ${f.rest})`);
    }
    const shown = own.filter((f) => Math.max(f.canvas, f.still) > 0.02);
    const boxes = [...new Set(shown.map((f) => f.box))];
    if (boxes.length > 1) {
      bad++;
      failures.push(`${width}: screen ${i} moves or resizes while shown (${boxes.join(' → ')})`);
    }
    const full = own.findIndex((f) => Math.max(f.canvas, f.still) > 0.98);
    const gone = full < 0 ? undefined : own.slice(full).find((f) => Math.max(f.canvas, f.still) < 0.9);
    if (gone) {
      bad++;
      failures.push(`${width}: screen ${i} goes away after it showed (at ${gone.t} ms: canvas ${gone.canvas.toFixed(2)}, still ${gone.still.toFixed(2)})`);
    }
    if (full < 0) {
      bad++;
      failures.push(`${width}: screen ${i} never showed`);
    }
  }
  console.log(`${bad ? 'FAIL' : 'ok  '} ${width} px${slow ? '' : ' (fast)'}: ${screens.length} screens over ${frames.length} samples${bad ? `, ${bad} broken rule(s)` : ': each shows only inside its device at rest, holds still, stays'}`);

  // the strip: a frame every 100 ms from 0.4 s to 4.3 s
  const TILE = width > 600 ? [320, 200] : [150, 325];
  const picks = [];
  for (let ms = 400; ms < 4400; ms += 100) picks.push(shots.findLast((s) => s.at - t0 <= ms) ?? shots[0]);
  const tiles = await Promise.all(picks.filter(Boolean).map((s) => sharp(Buffer.from(s.data, 'base64')).resize(TILE[0], TILE[1]).png().toBuffer()));
  const cols = width > 600 ? 8 : 10;
  await sharp({ create: { width: TILE[0] * cols, height: TILE[1] * Math.ceil(tiles.length / cols), channels: 3, background: '#202020' } })
    .composite(tiles.map((input, k) => ({ input, left: (k % cols) * TILE[0], top: Math.floor(k / cols) * TILE[1] })))
    .png()
    .toFile(`${OUT}in-place-${LABEL}-${width}${slow ? '' : '-fast'}.png`);
  await ctx.close();
}
await browser.close();
if (failures.length) {
  console.error(`\n${failures.length} failure(s):\n${failures.join('\n')}`);
  process.exitCode = 1;
} else console.log(`\nevery screen came up inside its device; strips in ${OUT}`);
