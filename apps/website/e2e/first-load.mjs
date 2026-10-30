// First-load check against a running preview (`pnpm build && pnpm preview`): the landing's hero
// (the wordmark the fluid draws, the Molty's slot, the device that turns in) must paint its
// intro's START state from the first frame and only move toward the final one; nothing is shown
// and then hidden. Samples every frame (requestAnimationFrame, from document start) on a slow
// network and CPU, and saves timed screenshots to website/e2e-out/first-load-*.png.
//   node e2e/first-load.mjs [baseUrl=http://localhost:4330]
import { chromium } from 'playwright';
import sharp from 'sharp';
import { mkdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

const BASE = process.argv[2] ?? 'http://localhost:4330';
const OUT = fileURLToPath(new URL('../e2e-out/', import.meta.url));
mkdirSync(OUT, { recursive: true });
const failures = [];
const check = (ok, msg) => {
  if (ok) console.log(`ok   ${msg}`);
  else {
    failures.push(msg);
    console.error(`FAIL ${msg}`);
  }
};

const TITLE = 'h1[data-fluid-word]';
const STILL = '[data-fluid-run-in] > img';
const DEVICE = '[data-device="arrive"]';

/** Runs in the page from document start: each frame (just before it paints) records how much
 *  of the title's DOM text, the hero's still and its device shows (opacity down the ancestors,
 *  times the text colour's alpha), the intro state on <html>, and the layout shifts. */
function sampler([TITLE, STILL, DEVICE]) {
  const frames = (window.__frames = []);
  window.__cls = 0;
  try {
    new PerformanceObserver((list) => {
      for (const e of list.getEntries()) if (!e.hadRecentInput) window.__cls += e.value;
    }).observe({ type: 'layout-shift', buffered: true });
  } catch {}
  const opacity = (el) => {
    let o = 1;
    for (let e = el; e; e = e.parentElement) o *= Number(getComputedStyle(e).opacity);
    return o;
  };
  const alpha = (color) => {
    const parts = /\(([^)]*)\)/.exec(color)?.[1].split(/[\s,/]+/).filter(Boolean) ?? [];
    return parts.length > 3 ? Number(parts[3]) : 1;
  };
  const tick = () => {
    const title = document.querySelector(TITLE);
    const still = document.querySelector(STILL);
    const device = document.querySelector(DEVICE);
    const root = document.documentElement;
    frames.push({
      t: Math.round(performance.now()),
      fluid: root.dataset.fluid ?? null,
      moltys: root.dataset.fluidMoltys ?? null,
      title: title ? opacity(title) * alpha(getComputedStyle(title).color) : null,
      still: still ? opacity(still) : null,
      device: device ? opacity(device) : null,
      transform: device ? device.style.transform || null : null,
    });
    requestAnimationFrame(tick);
  };
  requestAnimationFrame(tick);
}

const browser = await chromium.launch();

async function open(label, { js = true, reduced = false, slow = true, block = null } = {}) {
  const ctx = await browser.newContext({
    viewport: { width: 1440, height: 900 },
    deviceScaleFactor: 1,
    javaScriptEnabled: js,
    reducedMotion: reduced ? 'reduce' : 'no-preference',
  });
  const page = await ctx.newPage();
  page.on('pageerror', (e) => check(false, `${label}: pageerror ${e.message}`));
  if (block) await page.route(block, (r) => r.abort());
  if (slow) {
    const cdp = await ctx.newCDPSession(page);
    await cdp.send('Network.enable');
    await cdp.send('Network.emulateNetworkConditions', { offline: false, latency: 150, downloadThroughput: 1.6e6 / 8, uploadThroughput: 750e3 / 8 });
    await cdp.send('Emulation.setCPUThrottlingRate', { rate: 4 });
  }
  await page.addInitScript(sampler, [TITLE, STILL, DEVICE]);
  return { ctx, page };
}

/** Timed screenshots from navigation start (each one takes a while on a slow CPU: the times
 *  are when it was asked for). */
async function frames(page, label, times) {
  const t0 = Date.now();
  const nav = page.goto(BASE + '/', { waitUntil: 'commit' }).catch(() => {});
  for (const t of times) {
    const wait = t0 + t - Date.now();
    if (wait > 0) await new Promise((r) => setTimeout(r, wait));
    await page.screenshot({ path: `${OUT}first-load-${label}-${String(t).padStart(4, '0')}ms.png` }).catch(() => {});
  }
  await nav;
}

/** Pixels in `selector`'s box that differ from the page's background (the fluid draws the
 *  title there once it has condensed). */
async function inkIn(page, selector) {
  const box = await page.locator(selector).boundingBox();
  const shot = await page.screenshot({ clip: box });
  const { data, info } = await sharp(shot).removeAlpha().raw().toBuffer({ resolveWithObject: true });
  const bg = [data[0], data[1], data[2]];
  let n = 0;
  for (let i = 0; i < data.length; i += info.channels)
    if (Math.abs(data[i] - bg[0]) + Math.abs(data[i + 1] - bg[1]) + Math.abs(data[i + 2] - bg[2]) > 60) n++;
  return n;
}

const EPS = 0.02;
const series = (fs, key) => fs.filter((f) => f[key] !== null).map((f) => f[key]);
/** The first index where a value drops below an earlier one (shown, then hidden). */
const firstDrop = (xs) => {
  let max = -1;
  for (let i = 0; i < xs.length; i++) {
    if (xs[i] < max - EPS) return i;
    max = Math.max(max, xs[i]);
  }
  return -1;
};

// Motion allowed, slow network + CPU: the start state is painted first, then it plays in.
{
  const { ctx, page } = await open('intro');
  await frames(page, 'intro', [0, 50, 100, 200, 400, 800, 1600, 3200, 6000]);
  await page.waitForFunction(() => document.documentElement.dataset.fluidMoltys === 'live', null, { timeout: 30000 }).catch(() => {});
  await page.waitForTimeout(2500); // the device has turned in, the wordmark condensed
  const fs = await page.evaluate(() => window.__frames);
  const cls = await page.evaluate(() => window.__cls);
  const seen = fs.filter((f) => f.title !== null);
  console.log(`     ${fs.length} frames sampled, ${seen.length} with the hero; intro states: ${[...new Set(fs.map((f) => f.fluid))].join(' → ')}`);
  check(seen.length > 20, `intro: the hero was sampled from early frames (${seen.length})`);
  const title = series(fs, 'title');
  check(Math.max(...title) <= EPS, `intro: the wordmark's DOM text never shows (the fluid draws it; max alpha ${Math.max(...title).toFixed(2)})`);
  const still = series(fs, 'still');
  check(Math.max(...still) <= EPS, `intro: the hero's still never shows before its Molty emerges (max opacity ${Math.max(...still).toFixed(2)})`);
  const device = series(fs, 'device');
  const drop = firstDrop(device);
  check(drop < 0, `intro: the device only ever comes in (opacity monotonic${drop < 0 ? '' : `; drops at frame ${drop}: ${device.slice(Math.max(0, drop - 3), drop + 1).map((x) => x.toFixed(2)).join(' ')}`})`);
  check(device[0] <= EPS, `intro: the device starts unseen (${device[0]?.toFixed(2)})`);
  const last = fs.at(-1);
  check(last.fluid === 'live' && last.moltys === 'live', `intro: ends live (fluid ${last.fluid}, moltys ${last.moltys})`);
  check(last.device === 1 && !last.transform, `intro: the device ends at rest (opacity ${last.device}, transform ${last.transform})`);
  const ink = await inkIn(page, TITLE);
  check(ink > 2000, `intro: the fluid has drawn the wordmark (${ink} px)`);
  check(cls < 0.01, `intro: no layout shift (CLS ${cls.toFixed(4)})`);
  await page.screenshot({ path: `${OUT}first-load-intro-end.png` });
  await ctx.close();
}

// Reduced motion: the final state from the first frame, no animation.
{
  const { ctx, page } = await open('reduced', { reduced: true });
  await frames(page, 'reduced', [0, 200, 800, 3200]);
  await page.waitForFunction(() => document.documentElement.dataset.fluidMoltys === 'live', null, { timeout: 30000 }).catch(() => {});
  await page.waitForTimeout(500);
  const fs = (await page.evaluate(() => window.__frames)).filter((f) => f.title !== null);
  check(fs.length > 5, `reduced: sampled (${fs.length} frames)`);
  check(fs.every((f) => f.device === 1 && !f.transform), 'reduced: the device is at rest on every frame');
  check(fs.every((f) => f.title >= 1 - EPS || f.fluid === 'live'), 'reduced: the wordmark shows until the fluid draws it (never hidden first)');
  check(fs.every((f) => f.still >= 1 - EPS || f.moltys === 'live'), 'reduced: the still shows until the Molty is drawn (never hidden first)');
  await ctx.close();
}

// No JS: the page shows its content at rest (nothing left hidden).
{
  const { ctx, page } = await open('nojs', { js: false, slow: false });
  await page.goto(BASE + '/', { waitUntil: 'load' });
  await page.waitForTimeout(1000);
  const st = await page.evaluate(
    ([TITLE, STILL, DEVICE]) => {
      const cs = (s) => getComputedStyle(document.querySelector(s));
      return { title: cs(TITLE).color, still: cs(STILL).opacity, device: cs(DEVICE).opacity, fluid: document.documentElement.dataset.fluid ?? null };
    },
    [TITLE, STILL, DEVICE],
  );
  check(st.fluid === null, `nojs: no intro state (${st.fluid})`);
  check(!/rgba\(0, 0, 0, 0\)|transparent/.test(st.title), `nojs: the wordmark shows (${st.title})`);
  check(st.still === '1' && st.device === '1', `nojs: the still and the device show (${st.still}, ${st.device})`);
  check((await inkIn(page, TITLE)) > 2000, 'nojs: the wordmark paints');
  await page.screenshot({ path: `${OUT}first-load-nojs.png` });
  await ctx.close();
}

// JS on, but no script ever takes over (the bundles fail): the intro gives up and shows at rest.
{
  const { ctx, page } = await open('blocked', { slow: false, block: '**/_astro/*.js' });
  await page.goto(BASE + '/', { waitUntil: 'load' });
  await page.waitForTimeout(9000);
  const f = (await page.evaluate(() => window.__frames)).at(-1);
  check(f.fluid === 'static', `blocked: the intro ends static (${f.fluid})`);
  check(f.title >= 1 - EPS && f.still >= 1 - EPS && f.device >= 1 - EPS, `blocked: the hero shows at rest (title ${f.title}, still ${f.still}, device ${f.device})`);
  await ctx.close();
}

await browser.close();
if (failures.length) {
  console.error(`\n${failures.length} failure(s)`);
  process.exitCode = 1;
} else console.log(`\nall first-load checks passed; frames in ${OUT}`);
