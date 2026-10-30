// OG card OPTIONS (1200×630 + a 600×315 preview of each), rendered by the site's REAL runtime:
// the built landing's own <head> (tokens, CSS modules, the inline before-paint scripts) and its
// own React islands (FluidField: the page fluid that draws headings and Moltys; Device: the
// round screen's bezel and glass; SpeedReader: the talk demo with its yes / no / idk sectors)
// are recomposed into a card-sized page in the browser, served on the preview's origin and
// screenshotted once the fluid has drawn. Nothing here is hand-drawn. Not wired into the
// build; public/og.png stays what `pnpm og` makes until one of these is picked.
//   pnpm build && pnpm preview          (another terminal)
//   node scripts/og/generate-options.mjs [baseUrl=http://localhost:4330] [outDir=e2e-out/og-options]
//   OG_ONLY=device,conversation OG_THEMES=dark,light OG_MOLTY=cute-fox OG_BURST=1 …
//   OG_ONLY=device OG_THEMES=dark OG_MOLTYS=all OG_JOBS=3 …   (every Molty + a contact sheet)
//   (OG_BURST: frames every 0.5 s to <outDir>/burst/, to re-pick a moment)
import { chromium } from 'playwright';
import sharp from 'sharp';
import { mkdirSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { OPTIONS } from './options.mjs';

const BASE = process.argv[2] ?? 'http://localhost:4330';
const OUT = resolve(process.argv[3] ?? fileURLToPath(new URL('../../e2e-out/og-options/', import.meta.url)));
const ONLY = process.env.OG_ONLY?.split(',');
const THEMES = (process.env.OG_THEMES ?? 'dark,light').split(',');
const BURST = !!process.env.OG_BURST;
mkdirSync(OUT, { recursive: true });

const landing = await (await fetch(BASE + '/')).text();
const browser = await chromium.launch();

/** Runs in the browser: the landing's HTML → a card page built from its own parts. */
function compose([html, opt]) {
  const doc = new DOMParser().parseFromString(html, 'text/html');
  const island = (name) => doc.querySelector(`astro-island[component-export="${name}"]`);
  // the island runtime Astro inlines in the body (its scripts and the islands' display: contents)
  const runtime = [...doc.body.querySelectorAll('script, style')].filter((s) => !s.closest('astro-island') && (!s.type || s.type === 'module'));
  const fluid = island('FluidField');
  const mark = doc.querySelector('h1[data-fluid-word]');
  const slot = doc.querySelector('[data-fluid-molty]');
  const still = slot.querySelector('img');
  const heroDevice = slot.querySelector('astro-island[component-export="Device"]');
  const brand = doc.querySelector('footer > span'); // MoltyMark + "molty.UNO"
  const lede = doc.querySelector('p[data-fluid-quiet]');
  const card = doc.createElement('main');
  card.className = `og og-${opt.name}`;
  const el = (tag, cls, text) => {
    const e = doc.createElement(tag);
    if (cls) e.className = cls;
    if (text) e.textContent = text;
    return e;
  };
  /** The landing's own heading: the fluid condenses it (its class keeps the site's type). */
  const word = (text, cls) => {
    const h = mark.cloneNode(false);
    h.textContent = text;
    h.classList.add(cls);
    if (opt.wordDelay !== undefined) h.dataset.fluidDelay = String(opt.wordDelay);
    return h;
  };
  /** A Molty slot (the hero's, one Molty, no cycling), with or without its device. */
  const moltySlot = (cls, device) => {
    const s = slot.cloneNode(false);
    s.dataset.fluidMolty = opt.molty;
    s.dataset.fluidDelay = String(opt.moltyDelay ?? 0.3);
    delete s.dataset.fluidEvery;
    delete s.dataset.fluidRunIn;
    s.classList.add(cls);
    const img = still.cloneNode(false);
    s.append(img);
    if (device) s.append(heroDevice.cloneNode(true));
    return s;
  };
  const line = (text) => {
    const p = lede.cloneNode(false);
    p.textContent = text;
    p.classList.add('og-line');
    return p;
  };

  if (opt.name.startsWith('device')) {
    card.append(moltySlot('og-screen', true), word('molty.UNO', 'og-word'), line(opt.line));
  } else if (opt.name === 'conversation') {
    const reader = island('SpeedReader');
    const props = JSON.parse(reader.getAttribute('props'));
    const said = props.text[1];
    props.text = [0, opt.question];
    reader.setAttribute('props', JSON.stringify(props));
    // its server-rendered lines too, or hydration would not match
    const texts = doc.createTreeWalker(reader, NodeFilter.SHOW_TEXT);
    while (texts.nextNode()) if (texts.currentNode.data === said) texts.currentNode.data = opt.question;
    const side = el('div', 'og-side');
    const foot = el('div', 'og-foot');
    foot.append(brand.cloneNode(true), line(opt.line));
    side.append(foot);
    card.append(reader, side);
  } else {
    card.append(word('molty.UNO', 'og-word'), line(opt.line), moltySlot('og-molty', false));
  }
  card.prepend(fluid);
  const style = el('style', null, opt.css);
  doc.head.append(style);
  doc.body.replaceChildren(...runtime, card);
  return '<!doctype html>\n' + doc.documentElement.outerHTML;
}

/** Inked pixels in a box of a screenshot: far from the card's background (its top-left
 *  pixel), past the field's dim braille, in either theme. */
async function ink(png, box, bright = false) {
  const clip = { left: Math.max(0, Math.round(box.x)), top: Math.max(0, Math.round(box.y)) };
  clip.width = Math.min(1200 - clip.left, Math.round(box.width));
  clip.height = Math.min(630 - clip.top, Math.round(box.height));
  const { data, info } = await sharp(png).extract(clip).removeAlpha().raw().toBuffer({ resolveWithObject: true });
  const { data: bg } = await sharp(png).extract({ left: 0, top: 0, width: 1, height: 1 }).removeAlpha().raw().toBuffer({ resolveWithObject: true });
  let n = 0;
  for (let i = 0; i < data.length; i += info.channels)
    if (bright ? Math.min(data[i], data[i + 1], data[i + 2]) > 200 : Math.abs(data[i] - bg[0]) + Math.abs(data[i + 1] - bg[1]) + Math.abs(data[i + 2] - bg[2]) > 240) n++;
  return n;
}

/** The moment, from frames every ~20 ms across a window (ms after the card is ready): the one
 *  whose Molty's face reads best (the most near-white pixels in its slot: its eyes). With a
 *  `band`, only frames that catch the wordmark MID-condense count (its ink within that share
 *  of its final ink: half fluid, half letters); the condense takes ~0.4 s, so a fixed delay
 *  would land on either side of it. */
async function pickFrame(page, { from, to, band }) {
  const slot = await page.locator('[data-fluid-molty]').boundingBox();
  const word = band && (await page.locator('.og-word').boundingBox());
  await page.waitForTimeout(from);
  const frames = [];
  for (const end = Date.now() + (to - from); Date.now() < end; ) frames.push(await page.screenshot());
  let pool = await Promise.all(frames.map(async (png) => ({ png, m: await ink(png, slot, true), w: 1 })));
  if (band) {
    await page.waitForTimeout(2000);
    const full = await ink(await page.screenshot(), word);
    for (const f of pool) f.w = (await ink(f.png, word)) / full;
    pool = pool.filter((f) => f.w >= band[0] && f.w <= band[1]);
    if (!pool.length) throw new Error('no frame caught the wordmark mid-condense');
  }
  const best = pool.sort((a, b) => b.m - a.m)[0];
  console.log(`     ${frames.length} frames${band ? `, wordmark at ${(best.w * 100).toFixed(0)} % of its ink` : ''}`);
  return best.png;
}

/** One card: compose, serve, wait for the fluid (and its Molty), capture → <OUT>/<stem>.png. */
async function render(opt, theme, stem) {
  const ctx = await browser.newContext({ viewport: { width: 1200, height: 630 }, deviceScaleFactor: 1, colorScheme: theme });
  const page = await ctx.newPage();
  page.on('pageerror', (e) => console.error(`${stem}: pageerror ${e.message}`));
  await page.goto(BASE + '/robots.txt');
  const card = await page.evaluate(compose, [landing, opt]);
  const url = `${BASE}/__og/${stem.replace(/\W/g, '-')}`;
  await page.route(url, (r) => r.fulfill({ body: card, contentType: 'text/html; charset=utf-8' }));
  await page.goto(url, { waitUntil: 'networkidle' });
  await page.waitForFunction(() => document.documentElement.dataset.fluid === 'live', null, { timeout: 20000 });
  if (opt.molty) await page.waitForFunction(() => document.documentElement.dataset.fluidMoltys === 'live', null, { timeout: 20000 });
  if (opt.waitFor) await page.waitForSelector(opt.waitFor, { timeout: 30000 });
  if (BURST) {
    mkdirSync(`${OUT}/burst`, { recursive: true });
    for (let i = 0; i < 24; i++) {
      await page.screenshot({ path: `${OUT}/burst/${stem}-${String(i * 500).padStart(5, '0')}.png` });
      await page.waitForTimeout(500);
    }
  } else {
    const png = opt.pick ? await pickFrame(page, opt.pick) : (await page.waitForTimeout(opt.settle), await page.screenshot());
    await sharp(png).toFile(`${OUT}/${stem}.png`);
    await sharp(png).resize(600, 315).toFile(`${OUT}/${stem}-600.png`);
    console.log(`wrote ${OUT}/${stem}.png (+ -600)`);
  }
  await ctx.close();
}

/** OG_MOLTYS=all (or a comma list of ids): every card with a Molty once per Molty, as
 *  <outDir>/<moltyDir>/<name>[-light].png (moltyDir: the option's, else its name), plus a
 *  labelled contact-sheet[-light].png there. */
const catalog = JSON.parse(readFileSync(new URL('../../../../device/console/src/catalog/catalog.json', import.meta.url), 'utf8')).creatures;
const moltys = process.env.OG_MOLTYS === 'all' ? catalog.map((c) => c.id) : process.env.OG_MOLTYS?.split(',');
const short = (id) => id.replace(/^cute-/, '');
const jobs = [];
const sheets = [];
for (const opt of OPTIONS) {
  if (ONLY && !ONLY.includes(opt.name)) continue;
  if (process.env.OG_MOLTY && opt.molty) opt.molty = process.env.OG_MOLTY;
  for (const theme of THEMES.filter((t) => (opt.themes ?? THEMES).includes(t))) {
    const suffix = theme === 'dark' ? '' : '-light';
    if (moltys && opt.molty) {
      const dir = opt.moltyDir ?? opt.name;
      mkdirSync(`${OUT}/${dir}`, { recursive: true });
      for (const id of moltys) jobs.push(() => render({ ...opt, molty: id }, theme, `${dir}/${short(id)}${suffix}`));
      sheets.push({ stem: `${dir}/contact-sheet${suffix}`, cards: moltys.map((id) => [`${dir}/${short(id)}${suffix}`, catalog.find((c) => c.id === id)?.name ?? id]) });
    } else jobs.push(() => render(opt, theme, `${opt.file}${suffix}`));
  }
}
// OG_JOBS cards at once (each runs its own fluid; a few in parallel keep 30 fps on a laptop)
const queue = [...jobs];
await Promise.all(Array.from({ length: Number(process.env.OG_JOBS ?? 1) }, async () => { while (queue.length) await queue.shift()(); }));

/** All cards of one option tiled at 400×210 with their names, to compare at a glance. */
for (const { stem, cards } of sheets) {
  const W = 400, H = 210, L = 30, C = 5;
  const tiles = await Promise.all(
    cards.map(async ([file, name], i) => {
      const label = Buffer.from(`<svg width="${W}" height="${L}"><rect width="100%" height="100%" fill="#111"/><text x="10" y="21" font-family="Menlo, monospace" font-size="16" fill="#d8e2dc">${name}</text></svg>`);
      const img = await sharp(`${OUT}/${file}.png`).resize(W, H).toBuffer();
      const x = (i % C) * W, y = Math.floor(i / C) * (H + L);
      return [{ input: img, left: x, top: y }, { input: label, left: x, top: y + H }];
    }),
  );
  const out = `${OUT}/${stem}.png`;
  await sharp({ create: { width: W * C, height: (H + L) * Math.ceil(cards.length / C), channels: 3, background: '#222' } }).composite(tiles.flat()).png().toFile(out);
  console.log(`wrote ${out}`);
}
await browser.close();
