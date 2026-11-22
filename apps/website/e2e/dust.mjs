// The page's dust against a running preview (`pnpm build && pnpm preview`).
//   scrolls      every layer of the page's dust (the loose tile, the denser dust round devices
//                and headings, the drifting dust) moves with the page: a picture of the dust
//                alone, scrolled about 300 px, is the first one moved up as far, to the pixel (the
//                drift held still for it), on the landing, /build/ and /showcase/, wide and on a
//                phone, dark and light; the pairs and what did not move: e2e-out/dust-*.png
//   a control    a tap on a link or a button raises a fainter wave than a tap on bare page beside
//                it (config/water.mjs BUTTON): the dust it moves at the same moment after the
//                tap, both pictures: e2e-out/dust-tap-*.png
//   node e2e/dust.mjs [baseUrl=http://localhost:4330] [outDir=e2e-out]
import { chromium } from 'playwright';
import sharp from 'sharp';
import { mkdirSync } from 'node:fs';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const BASE = process.argv[2] ?? 'http://localhost:4330';
const OUT = process.argv[3] ? resolve(process.argv[3]) + '/' : fileURLToPath(new URL('../e2e-out/', import.meta.url));
mkdirSync(OUT, { recursive: true });
const failures = [];
const pass = (ok, line) => (console.log(`${ok ? 'ok  ' : 'FAIL'} ${line}`), ok || failures.push(line));
const SCROLL = 300;

/** Runs in the page from document start: the display's frames by hand once __manual is set
 *  (e2e/water.mjs), and how much of the page's canvas the water has moved. */
function tools() {
  const real = window.requestAnimationFrame.bind(window);
  let queue = [],
    now = 0;
  window.requestAnimationFrame = (cb) => (window.__manual ? queue.push(cb) : real(cb));
  window.__pump = (n = 1) => {
    for (let f = 0; f < n; f++) {
      now = (now || performance.now()) + 1000 / 60;
      const run = queue;
      queue = [];
      for (const cb of run) cb(now);
    }
  };
  window.__moved = () => {
    const c = document.querySelector('[data-page-water]');
    const px = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
    let n = 0;
    for (let i = 3; i < px.length; i += 4) if (px[i]) n++;
    return n;
  };
}

const browser = await chromium.launch();
async function open(path, width, height, colorScheme, reducedMotion = 'no-preference') {
  const ctx = await browser.newContext({
    viewport: { width, height },
    deviceScaleFactor: 1,
    colorScheme,
    reducedMotion,
  });
  const page = await ctx.newPage();
  page.on('pageerror', (e) => failures.push(`${path}: pageerror ${e.message}`));
  await page.addInitScript(tools);
  await page.goto(BASE + path, { waitUntil: 'load' });
  await page.waitForTimeout(5000);
  return { ctx, page };
}
const raw = async (buffer) => sharp(buffer).raw().toBuffer({ resolveWithObject: true });

// ---------------------------------------------------------------- every dot scrolls with the page
const cases = ['/', '/build/', '/showcase/'].flatMap((path) =>
  [
    [1440, 900],
    [390, 844],
  ].flatMap(([width, height]) => ['dark', 'light'].map((scheme) => [path, width, height, scheme])),
);
// reduced motion: the dust is laid, nothing drifts, and it scrolls all the same
cases.push(['/', 1440, 900, 'dark', 'reduce']);
for (const [path, width, height, scheme, motion] of cases) {
  const { ctx, page } = await open(path, width, height, scheme, motion);
  // the dust alone (everything else hidden), the drift held where it is, no snap to a section
  const cdp = await ctx.newCDPSession(page);
  await cdp.send('Animation.enable');
  await cdp.send('Animation.setPlaybackRate', { playbackRate: 0 });
  await page.addStyleTag({
    content: 'html { scroll-snap-type: none !important } body * { visibility: hidden !important } [data-water], [data-water] > astro-island > span { visibility: visible !important }',
  });
  await page.waitForTimeout(300);
  const a = await page.screenshot();
  await page.evaluate((y) => window.scrollTo(0, y), SCROLL);
  await page.waitForTimeout(300);
  const dy = Math.round(await page.evaluate(() => scrollY));
  const b = await page.screenshot();
  const [A, B] = [await raw(a), await raw(b)];
  const { width: w, height: h, channels: n } = A.info;
  const ground = [...A.data.subarray(0, n)];
  // a pixel of the dust that did not move with the page: it differs from the first picture's pixel dy down
  const pinned = Buffer.alloc(w * (h - dy) * 3);
  let dust = 0,
    stuck = 0;
  for (let y = 0; y < h - dy; y++)
    for (let x = 0; x < w; x++) {
      const i = (y * w + x) * n,
        j = ((y + dy) * w + x) * n;
      if (Math.max(...[0, 1, 2].map((c) => Math.abs(B.data[i + c] - ground[c]))) > 12) dust++;
      const off = Math.max(...[0, 1, 2].map((c) => Math.abs(B.data[i + c] - A.data[j + c])));
      if (off > 12) stuck++;
      pinned.fill(off > 12 ? 255 : 0, (y * w + x) * 3, (y * w + x) * 3 + 3);
    }
  const name = `dust-${path.replaceAll('/', '') || 'home'}-${width}-${scheme}${motion ? '-still' : ''}`;
  await sharp(a).toFile(`${OUT}${name}-0.png`);
  await sharp(b).toFile(`${OUT}${name}-scrolled.png`);
  await sharp(pinned, { raw: { width: w, height: h - dy, channels: 3 } })
    .png()
    .toFile(`${OUT}${name}-pinned.png`);
  pass(dy >= 100 && stuck <= 0.002 * dust, `${path} ${width} x ${height} ${scheme}${motion ? ', reduced motion' : ''}: scrolled ${dy} px, ${stuck} of ${dust} dust pixels did not move with the page`);
  await ctx.close();
}

// ---------------------------------------------------------------- a tap on a control is fainter
{
  /** The dust a tap at (x, y) moves 0.5 s after it, and a picture of the water then. The tap is
   *  a press only (no click: a link is not followed). */
  const tap = async (at, name) => {
    const { ctx, page } = await open('/', 1440, 900, 'dark');
    await page.evaluate(() => (window.__manual = true));
    await page.mouse.move(...at(await page.evaluate(spot)));
    await page.mouse.down();
    await page.waitForTimeout(800); // the water's module loads, its frames wait for the pump
    await page.evaluate(() => window.__pump(30));
    const moved = await page.evaluate(() => window.__moved());
    const { x, y } = await page.evaluate(spot);
    await sharp(
      await page.screenshot({
        clip: {
          x: Math.max(0, x - 450),
          y: Math.max(0, y - 250),
          width: 900,
          height: 500,
        },
      }),
    ).toFile(`${OUT}dust-tap-${name}.png`);
    await ctx.close();
    return moved;
  };
  // the landing's first link or button in the viewport, off any screen, with bare page beside it
  const spot = () => {
    const free = (x, y) => !document.elementFromPoint(x, y)?.closest('a, button, input, label, summary, [role], [tabindex], [data-molty-screen]');
    for (const el of document.querySelectorAll('[data-water] section a[href], [data-water] section button')) {
      const r = el.getBoundingClientRect();
      const x = r.left + r.width / 2,
        y = r.top + r.height / 2;
      if (!r.width || r.top < 0 || r.bottom > innerHeight || el.closest('[data-molty-screen]')) continue;
      const side = [x + 200, x - 200].find((s) => s > 0 && s < innerWidth && free(s, y));
      if (side)
        return {
          x,
          y,
          side,
          bare: true,
          what: el.textContent.trim().slice(0, 24),
        };
    }
    return { bare: false, what: 'no link or button' };
  };
  const { bare: free, what } = await (async () => {
    const { ctx, page } = await open('/', 1440, 900, 'dark');
    const s = await page.evaluate(spot);
    await ctx.close();
    return s;
  })();
  pass(free, `beside "${what}" is bare page`);
  const control = await tap((s) => [s.x, s.y], 'control');
  const bare = await tap((s) => [s.side, s.y], 'bare');
  pass(control > 0 && control < 0.6 * bare, `a tap on a control moves ${control} px of dust, on bare page beside it ${bare} (${((100 * control) / bare).toFixed(0)}%), 0.5 s after it`);
}

await browser.close();
if (failures.length) {
  console.error(`\n${failures.length} failure(s):\n${failures.join('\n')}`);
  process.exitCode = 1;
} else console.log(`\nthe page's dust passed; pictures in ${OUT}`);
