// The GitHub README's hero, rendered by the site's REAL runtime (the approach of scripts/og:
// the built landing's own <head> and React islands, recomposed into a small page on the
// preview's origin and captured as it plays). Nothing here is hand-drawn.
//   left   the landing's braille "molty.UNO" heading (FluidField, the page fluid)
//   right  the hero's Molty slot in its round Device, and over it the talk demo's screen
//          (SpeedReader, the same Device) for the question; one at a time, swapped while
//          both screens are only fluid
// One scripted round, recorded as it plays (the screen @2x, ~40 captures/s):
//   the Molty condenses out of the fluid and idles → melts → "Build passed. Open the PR?"
//   word by word → yes / no / idk sectors → a tap on yes → "Opening it now." → the Molty
//   condenses again, happy → melts (back to the first frame: a seamless loop)
// → <outDir>/molty-hero.webp (animated, ~2 MB) + molty-hero.png (the question, still). Dark
// only by default: on paper the wordmark washes out, and the README shows the dark hero in both
// GitHub themes (README_THEMES=dark,light renders a -light pair to compare).
// Re-run when the look changes; the outputs are committed to docs/assets/.
//   pnpm build && pnpm preview          (another terminal)
//   pnpm readme [baseUrl=http://localhost:4330] [outDir=../../docs/assets]
//   README_THEMES=dark,light README_MOLTY=cute-frog README_FPS=12 README_COLOURS=256 README_SCREEN=300 …
import { chromium } from 'playwright';
import sharp from 'sharp';
import { mkdirSync, statSync } from 'node:fs';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const BASE = process.argv[2] ?? 'http://localhost:4330';
const OUT = resolve(process.argv[3] ?? fileURLToPath(new URL('../../../../docs/assets/', import.meta.url)));
const THEMES = (process.env.README_THEMES ?? 'dark').split(',');
const MOLTY = process.env.README_MOLTY ?? 'cute-frog';
const FPS = Number(process.env.README_FPS ?? 12);
const QUALITY = Number(process.env.README_Q ?? 80);
const COLOURS = Number(process.env.README_COLOURS ?? 256); // 0: lossy WebP at README_Q instead
const QUESTION = 'Build passed. Open the PR?';
const ANSWER = 'Opening it now.';
const W = 830, H = 400, D = Number(process.env.README_SCREEN ?? 300), DPR = 2; // GitHub's README column, @2x; D: the screen
mkdirSync(OUT, { recursive: true });

const landing = await (await fetch(BASE + '/')).text();
const browser = await chromium.launch();

/** Runs in the browser: the landing's HTML → the hero, built from its own parts. */
function compose([html, question, answer, W, H, D]) {
  const doc = new DOMParser().parseFromString(html, 'text/html');
  const island = (name) => doc.querySelector(`astro-island[component-export="${name}"]`);
  // the island runtime Astro inlines in the body (its scripts and the islands' display: contents)
  const runtime = [...doc.body.querySelectorAll('script, style')].filter((s) => !s.closest('astro-island') && (!s.type || s.type === 'module'));
  const main = doc.createElement('main');
  main.className = 'rm';
  const mark = doc.querySelector('h1[data-fluid-word]').cloneNode(true);
  mark.dataset.fluidDelay = '0';
  // the hero's slot: no Molty until the script asks for one
  const hero = doc.querySelector('[data-fluid-molty]');
  const slot = hero.cloneNode(false);
  slot.dataset.fluidMolty = '';
  for (const k of ['fluidEvery', 'fluidRunIn', 'fluidDelay']) delete slot.dataset[k];
  slot.append(hero.querySelector('astro-island[component-export="Device"]').cloneNode(true));
  // the talk demo, asking our question (its server-rendered text too, or hydration would not match)
  const reader = island('SpeedReader');
  const props = JSON.parse(reader.getAttribute('props'));
  const said = props.text[1];
  props.text = [0, question];
  props.answers[1].yes = [0, answer];
  reader.setAttribute('props', JSON.stringify(props));
  const texts = doc.createTreeWalker(reader, NodeFilter.SHOW_TEXT);
  while (texts.nextNode()) if (texts.currentNode.data.includes(said)) texts.currentNode.data = texts.currentNode.data.replace(said, question);
  const stage = doc.createElement('div');
  stage.className = 'rm-stage';
  stage.append(slot, reader);
  main.append(island('FluidField'), mark, stage);
  const style = doc.createElement('style');
  style.textContent = `
    html, body { margin: 0; overflow: hidden; background: var(--m-bg); }
    .rm { position: relative; box-sizing: border-box; width: ${W}px; height: ${H}px;
      display: grid; grid-template-columns: 1fr ${D}px; align-items: center; column-gap: 20px; padding: 0 28px 0 30px; }
    .rm > :not(astro-island) { position: relative; z-index: 1; }
    .rm h1 { font-size: 84px; }
    .rm-stage { position: relative; width: ${D}px; height: ${D}px; }
    .rm-stage > [data-fluid-molty], .rm-stage [data-reader-glass] { position: absolute; inset: 0; width: ${D}px; margin: 0; }
    .rm-stage [data-speed-reader] > :not([data-reader-glass]) { display: none; }
    /* one screen at a time: the script swaps them while both are only fluid */
    .rm-stage[data-show="molty"] [data-reader-glass], .rm-stage[data-show="ask"] > [data-fluid-molty] { opacity: 0; }
  `;
  doc.head.append(style);
  doc.body.replaceChildren(...runtime, main);
  return '<!doctype html>\n' + doc.documentElement.outerHTML;
}

/** The round screen, captured @2x as fast as it paints (~40/s), while `run` plays. Only the
 *  screen moves in the loop: the rest of the hero (the wordmark, the field, the bezel) is one
 *  frozen frame, so the animation stays small. */
async function record(page, clip, run) {
  const frames = [];
  let done = false;
  const grab = (async () => {
    while (!done) {
      const t = Date.now();
      frames.push({ t, png: await page.screenshot({ clip, animations: 'allow', caret: 'initial' }) });
    }
  })();
  const marks = await run();
  done = true;
  await grab;
  return { frames, marks };
}

/** The frames shown at a steady FPS across [from, to] (ms, wall clock). */
function resample(frames, from, to) {
  const out = [];
  let i = 0;
  for (let t = from; t < to; t += 1000 / FPS) {
    while (i + 1 < frames.length && frames[i + 1].t <= t) i++;
    out.push(frames[i].png);
  }
  return out;
}

/** One palette for the whole loop (COLOURS, from 16 frames spread across it), every
 *  frame mapped onto it: the WebP is then lossless (crisp braille, no ringing) and about half
 *  the size of a lossy one, and what does not move stays bit-identical between frames. */
async function quantize(pngs, colours) {
  if (!colours) return pngs;
  const raw = await Promise.all(pngs.map((p) => sharp(p).removeAlpha().raw().toBuffer({ resolveWithObject: true })));
  const { width, height } = raw[0].info;
  const picks = Array.from({ length: 16 }, (_, i) => raw[Math.floor((i * raw.length) / 16)]);
  const sheet = await sharp({ create: { width, height: height * picks.length, channels: 3, background: '#000' } })
    .composite(picks.map((r, i) => ({ input: r.data, raw: { width, height, channels: 3 }, left: 0, top: i * height })))
    .png({ palette: true, colours, dither: 0 })
    .toBuffer();
  const { data: pal } = await sharp(sheet).removeAlpha().raw().toBuffer({ resolveWithObject: true });
  const set = new Set();
  for (let j = 0; j < pal.length; j += 3) set.add((pal[j] << 16) | (pal[j + 1] << 8) | pal[j + 2]);
  const palette = [...set].map((c) => [c >> 16, (c >> 8) & 255, c & 255]);
  const near = new Map();
  const map = (r, g, b) => {
    const k = (r << 16) | (g << 8) | b;
    let c = near.get(k);
    if (c === undefined) {
      let best = Infinity;
      for (const p of palette) {
        const d = (p[0] - r) ** 2 + (p[1] - g) ** 2 + (p[2] - b) ** 2;
        if (d < best) (best = d), (c = p);
      }
      near.set(k, c);
    }
    return c;
  };
  return Promise.all(
    raw.map(({ data }) => {
      const out = Buffer.alloc(data.length);
      for (let j = 0; j < data.length; j += 3) {
        const c = map(data[j], data[j + 1], data[j + 2]);
        out[j] = c[0];
        out[j + 1] = c[1];
        out[j + 2] = c[2];
      }
      return sharp(out, { raw: { width, height, channels: 3 } }).png().toBuffer();
    }),
  );
}

/** Fades the round's last frames into its first (both only fluid), so the seam does not jump. */
async function seam(pngs, n = 4) {
  const raw = await Promise.all(pngs.map((p) => sharp(p).removeAlpha().raw().toBuffer({ resolveWithObject: true })));
  const { width, height } = raw[0].info;
  for (let i = 0; i < n; i++) {
    const a = raw[raw.length - n + i].data, b = raw[0].data, k = (i + 1) / (n + 1);
    const mix = Buffer.alloc(a.length);
    for (let j = 0; j < a.length; j++) mix[j] = Math.round(a[j] * (1 - k) + b[j] * k);
    raw[raw.length - n + i].data = mix;
  }
  return Promise.all(raw.map(({ data }) => sharp(data, { raw: { width, height, channels: 3 } }).png().toBuffer()));
}

for (const theme of THEMES) {
  const suffix = theme === 'dark' ? '' : '-light';
  const ctx = await browser.newContext({ viewport: { width: W, height: H }, deviceScaleFactor: DPR, colorScheme: theme });
  const page = await ctx.newPage();
  page.on('pageerror', (e) => console.error(`pageerror ${e.message}`));
  await page.goto(BASE + '/robots.txt');
  const html = await page.evaluate(compose, [landing, QUESTION, ANSWER, W, H, D]);
  const url = `${BASE}/__readme/hero-${theme}`;
  await page.route(url, (r) => r.fulfill({ body: html, contentType: 'text/html; charset=utf-8' }));
  await page.goto(url, { waitUntil: 'networkidle' });
  await page.waitForFunction(() => document.documentElement.dataset.fluid === 'live', null, { timeout: 20000 });
  await page.waitForSelector('[data-reader-glass][data-live]', { timeout: 30000 });
  const set = (sel, k, v) => page.evaluate(([sel, k, v]) => (document.querySelector(sel).dataset[k] = v), [sel, k, v]);
  await set('.rm-stage', 'show', 'molty');
  /** The talk demo's own "type your own" line: its round starts over with it. */
  const say = (line) =>
    page.evaluate((line) => {
      const root = document.querySelector('[data-speed-reader]');
      if (!root.querySelector('form')) [...root.querySelectorAll('button')].find((b) => b.textContent.includes('type your own')).click();
      return new Promise((done) =>
        requestAnimationFrame(() => {
          const input = root.querySelector('form input');
          Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set.call(input, line);
          input.dispatchEvent(new Event('input', { bubbles: true }));
          requestAnimationFrame(() => (input.form.requestSubmit(), done()));
        }),
      );
    }, line);
  // the wordmark condensed, the devices turned in; the hidden talk screen keeps to a "." (no
  // question: it never listens), so it is only fluid when it takes over
  await page.waitForTimeout(3000);
  await say('.');
  await page.waitForTimeout(3500);

  // the screen: a circle in the stage (the bezel reaches past it and is static)
  const box = await page.locator('.rm-stage').boundingBox();
  const clip = { x: box.x, y: box.y, width: box.width, height: box.height };
  const base = await page.screenshot(); // the frozen hero, the screen only fluid
  let still = null;
  const { frames, marks } = await record(page, clip, async () => {
    const at = {};
    await page.waitForTimeout(300);
    at.start = Date.now();
    await set('[data-fluid-molty]', 'fluidMolty', MOLTY); // condenses (~1 s), idles
    await page.waitForTimeout(1900);
    await set('[data-fluid-molty]', 'fluidMolty', ''); // melts
    await page.waitForTimeout(800);
    // the talk screen takes over while both are only fluid, and asks
    await say(QUESTION);
    await page.waitForTimeout(150); // its last line gives way to the first word
    await set('.rm-stage', 'show', 'ask');
    await page.waitForSelector('[data-reader-glass][data-listening]', { timeout: 20000 });
    await page.waitForTimeout(1100); // the sectors read
    still = await page.screenshot();
    const word = await page.locator('[data-choice="yes"] span span').first().boundingBox();
    await page.mouse.click(word.x + word.width / 2, word.y + word.height / 2);
    await page.waitForTimeout(2500); // the tapped word holds, "Opening it now.", melts
    await set('.rm-stage', 'show', 'molty');
    await set('[data-fluid-molty]', 'fluidMolty', MOLTY);
    await set('[data-fluid-molty]', 'fluidAct', 'mood:happy#1');
    await page.waitForTimeout(2300);
    await set('[data-fluid-molty]', 'fluidMolty', ''); // melts: the round's first frame again
    await page.waitForTimeout(900);
    at.end = Date.now();
    await page.waitForTimeout(300);
    return at;
  });
  // each frame: the frozen hero with the live screen set in (a soft-edged circle)
  const s = DPR, r = Math.round((clip.width / 2) * s);
  const ring = Buffer.from(`<svg width="${2 * r}" height="${2 * r}"><defs><radialGradient id="g"><stop offset="${(r - 3) / r}" stop-color="#fff"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></radialGradient></defs><circle cx="${r}" cy="${r}" r="${r}" fill="url(#g)"/></svg>`);
  const at = { left: Math.round(clip.x * s), top: Math.round(clip.y * s) };
  // the banner: the device and a little air above and below it (the page is laid out taller,
  // so nothing of the bezel or its shadow is cut)
  const top = Math.max(0, Math.round((clip.y - 36) * s));
  const band = { left: 0, top, width: W * s, height: Math.min(H * s, Math.round((clip.y + clip.height + 36) * s)) - top };
  const shown = resample(frames, marks.start, marks.end);
  const cache = new Map();
  const pngs = await quantize(await seam(
    await Promise.all(
      shown.map(async (png) => {
        if (!cache.has(png)) {
          const screen = await sharp(png).resize(2 * r, 2 * r).ensureAlpha().composite([{ input: ring, blend: 'dest-in' }]).png().toBuffer();
          const full = await sharp(base).composite([{ input: screen, ...at }]).removeAlpha().png().toBuffer();
          cache.set(png, await sharp(full).extract(band).png().toBuffer());
        }
        return cache.get(png);
      }),
    ),
  ), COLOURS);
  const file = `${OUT}/molty-hero${suffix}.webp`;
  await sharp(pngs, { join: { animated: true }, limitInputPixels: false })
    .webp({ ...(COLOURS ? { lossless: true } : { quality: QUALITY }), effort: 4, loop: 0, delay: pngs.map(() => Math.round(1000 / FPS)) })
    .toFile(file);
  console.log(`wrote ${file} (${pngs.length} frames, ${((marks.end - marks.start) / 1000).toFixed(1)} s, ${(statSync(file).size / 1e6).toFixed(2)} MB)`);
  // the still: the whole hero at the question (the sectors up)
  await sharp(still).extract(band).png().toFile(`${OUT}/molty-hero${suffix}.png`);
  console.log(`wrote ${OUT}/molty-hero${suffix}.png`);
  if (process.env.README_DEBUG) {
    mkdirSync(`${OUT}/frames`, { recursive: true });
    for (let i = 0; i < pngs.length; i++) await sharp(pngs[i]).toFile(`${OUT}/frames/${theme}-${String(i).padStart(3, '0')}.png`);
  }
  await ctx.close();
}
await browser.close();
