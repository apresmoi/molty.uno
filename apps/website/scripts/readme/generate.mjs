// The GitHub README's hero, rendered by the site's REAL runtime (the approach of scripts/og:
// the built landing's own <head> and React islands, recomposed into a small page on the
// preview's origin and recorded as it plays). Nothing here is hand-drawn.
//   left   the landing's braille "molty.UNO" heading in the page fluid (FluidField), live: the
//          field moves, and a pointer passing over the wordmark stirs it
//   right  the hero's Molty slot in its round Device, and over it the talk demo's screen
//          (SpeedReader, the same Device) for the question; one at a time, swapped while
//          both screens are only fluid
// One scripted round, the whole banner recorded @2x:
//   the Molty (the duck) condenses out of the fluid and idles → melts → "Build passed. Open
//   the PR?" word by word → yes / no / idk sectors → a tap on yes → "Opening it now." → the
//   Molty condenses again, happy → melts (back to the first frame: a seamless loop)
// The page is GitHub's own colour (#0d1117 dark, #ffffff light) and the fluid fades out at the
// banner's edges, so the README shows no box; the README picks the variant per theme.
// → <outDir>/<name>.webp + <name>-light.webp (animated, lossless 256 colours, ~3–4 MB) and
//   <name>[-light].png (the question, still)
// Re-run when the look changes; the outputs are committed to docs/assets/.
//   pnpm build && pnpm preview          (another terminal)
//   pnpm readme [baseUrl=http://localhost:4330] [outDir=../../docs/assets]
//   README_MOLTY=cute-bunny README_NAME=molty-hero-bunny README_THEMES=dark README_FPS=12 …
import { chromium } from 'playwright';
import sharp from 'sharp';
import { mkdirSync, statSync } from 'node:fs';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const BASE = process.argv[2] ?? 'http://localhost:4330';
const OUT = resolve(process.argv[3] ?? fileURLToPath(new URL('../../../../docs/assets/', import.meta.url)));
const THEMES = (process.env.README_THEMES ?? 'dark,light').split(',');
const MOLTY = process.env.README_MOLTY ?? 'cute-duck';
const NAME = process.env.README_NAME ?? 'molty-hero'; // the output files' stem
const FPS = Number(process.env.README_FPS ?? 12);
const QUALITY = Number(process.env.README_Q ?? 80);
const COLOURS = Number(process.env.README_COLOURS ?? 256); // 0: lossy WebP at README_Q instead
const QUESTION = 'Build passed. Open the PR?';
/** GitHub's page colours, so the banner has no box; on paper the wordmark is handed a deeper
 *  green (the site's paper conversion keeps its hue; thin braille dots need more than 3:1). */
const LOOK = { dark: { page: '#0d1117' }, light: { page: '#ffffff', ink: '#0b6642' } };
const ANSWER = 'Opening it now.';
const W = 830, H = 440, PAD = 46, D = Number(process.env.README_SCREEN ?? 300), DPR = 2; // GitHub's README column, @2x; D: the screen
mkdirSync(OUT, { recursive: true });

const landing = await (await fetch(BASE + '/')).text();
const browser = await chromium.launch();

/** Runs in the browser: the landing's HTML → the hero, built from its own parts. */
function compose([html, question, answer, W, H, D, PAD, look]) {
  const doc = new DOMParser().parseFromString(html, 'text/html');
  const island = (name) => doc.querySelector(`astro-island[component-export="${name}"]`);
  // the island runtime Astro inlines in the body (its scripts and the islands' display: contents)
  const runtime = [...doc.body.querySelectorAll('script, style')].filter((s) => !s.closest('astro-island') && (!s.type || s.type === 'module'));
  const main = doc.createElement('main');
  main.className = 'rm';
  const mark = doc.querySelector('h1[data-fluid-word]').cloneNode(true);
  mark.dataset.fluidDelay = '0';
  if (look.ink) mark.dataset.fluidColor = look.ink;
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
    html, body { margin: 0; overflow: hidden; background: ${look.page}; }
    /* the page fluid on GitHub's own page colour, fading out at the banner's edges (no box) */
    [data-fluid-field] { background: transparent;
      mask-image: radial-gradient(circle ${D / 2 + 2}px at ${W - PAD - D / 2}px ${H / 2}px, #000 99%, transparent 100%),
        linear-gradient(to right, transparent 0, #000 ${look.fade}px, #000 calc(100% - ${look.fade}px), transparent 100%),
        linear-gradient(to bottom, transparent ${look.top}px, #000 ${look.top + look.fade}px, #000 ${look.bottom - look.fade}px, transparent ${look.bottom}px);
      mask-composite: add, intersect; }
    /* the Molty's screen: the site's page colour under it (the Device's backdrop inverts it on
       paper), so it is the same dark LCD as the talk screen in both themes */
    .rm::before { content: ''; position: absolute; z-index: 0; left: ${W - PAD - D}px; top: ${(H - D) / 2}px;
      width: ${D}px; height: ${D}px; border-radius: 50%; background: var(--m-bg); }
    .rm { position: relative; box-sizing: border-box; width: ${W}px; height: ${H}px;
      display: grid; grid-template-columns: 1fr ${D}px; align-items: center; column-gap: 20px; padding: 0 ${PAD}px 0 30px; }
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

/** The banner, captured @2x as fast as it paints, while `run` plays. */
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

/** The last `n` device px at the bottom (the tail of the device's soft shadow) eased into the
 *  page colour, so no frame ends on a line. */
async function settle(pngs, page, n = 48) {
  const bg = [1, 3, 5].map((i) => parseInt(page.slice(i, i + 2), 16));
  return Promise.all(
    pngs.map(async (png) => {
      const { data, info } = await sharp(png).removeAlpha().raw().toBuffer({ resolveWithObject: true });
      const { width, height } = info;
      for (let y = height - n; y < height; y++) {
        const k = (y - (height - n) + 1) / n;
        for (let x = 0; x < width; x++) {
          const i = (y * width + x) * 3;
          for (let c = 0; c < 3; c++) data[i + c] = Math.round(data[i + c] * (1 - k) + bg[c] * k);
        }
      }
      return sharp(data, { raw: { width, height, channels: 3 } }).png().toBuffer();
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
  // the banner: the device and a little air above and below it (the page is laid out taller,
  // so nothing of the bezel or its shadow is cut); the fluid fades out at its edges
  const top = (H - D) / 2 - 40, bottom = (H + D) / 2 + 60; // the device's shadow falls below it
  const look = { ...LOOK[theme], top, bottom, fade: 56 };
  const html = await page.evaluate(compose, [landing, QUESTION, ANSWER, W, H, D, PAD, look]);
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

  const clip = { x: 0, y: top, width: W, height: bottom - top };
  const mark = await page.locator('.rm h1').boundingBox();
  let still = null;
  const { frames, marks } = await record(page, clip, async () => {
    const at = {};
    await page.waitForTimeout(300);
    at.start = Date.now();
    await set('[data-fluid-molty]', 'fluidMolty', MOLTY); // condenses (~1 s), idles
    // a hand passes over the wordmark: the pointer stirs the page fluid, the letters re-form
    const y = mark.y + mark.height / 2;
    await page.mouse.move(mark.x - 20, y);
    await page.mouse.move(mark.x + mark.width + 20, y + 6, { steps: 24 });
    await page.mouse.move(W - 4, 4, { steps: 4 });
    await page.waitForTimeout(1000);
    await set('[data-fluid-molty]', 'fluidMolty', ''); // melts
    await page.waitForTimeout(800);
    // the talk screen takes over while both are only fluid, and asks
    await say(QUESTION);
    await page.waitForTimeout(150); // its last line gives way to the first word
    await set('.rm-stage', 'show', 'ask');
    await page.waitForSelector('[data-reader-glass][data-listening]', { timeout: 20000 });
    await page.waitForTimeout(1100); // the sectors read
    still = await page.screenshot({ clip });
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
  console.log(`  ${frames.length} captures in ${((marks.end - marks.start) / 1000).toFixed(1)} s`);
  const pngs = await quantize(await settle(await seam(resample(frames, marks.start, marks.end)), look.page), COLOURS);
  const file = `${OUT}/${NAME}${suffix}.webp`;
  await sharp(pngs, { join: { animated: true }, limitInputPixels: false })
    .webp({ ...(COLOURS ? { lossless: true } : { quality: QUALITY }), effort: 4, loop: 0, delay: pngs.map(() => Math.round(1000 / FPS)) })
    .toFile(file);
  console.log(`wrote ${file} (${pngs.length} frames, ${((marks.end - marks.start) / 1000).toFixed(1)} s, ${(statSync(file).size / 1e6).toFixed(2)} MB)`);
  // the still: the whole hero at the question (the sectors up)
  await sharp((await settle([still], look.page))[0]).png().toFile(`${OUT}/${NAME}${suffix}.png`);
  console.log(`wrote ${OUT}/${NAME}${suffix}.png`);
  if (process.env.README_DEBUG) {
    mkdirSync(`${OUT}/frames`, { recursive: true });
    for (let i = 0; i < pngs.length; i++) await sharp(pngs[i]).toFile(`${OUT}/frames/${theme}-${String(i).padStart(3, '0')}.png`);
  }
  await ctx.close();
}
await browser.close();
