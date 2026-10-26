// Screenshot + smoke check against a running preview (`pnpm build && pnpm preview`).
// Writes website/e2e-out/*.png and fails on: page errors, horizontal overflow at 390px,
// or a hero screen that does not render / does not move.
//   node e2e/screens.mjs [baseUrl=http://localhost:4330]
import { chromium } from 'playwright';
import { mkdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

const BASE = process.argv[2] ?? 'http://localhost:4330';
const OUT = fileURLToPath(new URL('../e2e-out/', import.meta.url));
mkdirSync(OUT, { recursive: true });
const failures = [];
const fail = (msg) => {
  failures.push(msg);
  console.error(`FAIL ${msg}`);
};

const browser = await chromium.launch();

async function open(width, height, path) {
  const page = await browser.newPage({ viewport: { width, height }, deviceScaleFactor: 1 });
  page.on('pageerror', (e) => fail(`${path} @${width}: pageerror ${e.message}`));
  page.on('console', (m) => m.type() === 'error' && fail(`${path} @${width}: console ${m.text()}`));
  await page.goto(BASE + path, { waitUntil: 'networkidle' });
  return page;
}

async function overflow(page, label) {
  const { sw, cw } = await page.evaluate(() => ({ sw: document.documentElement.scrollWidth, cw: document.documentElement.clientWidth }));
  if (sw > cw) fail(`${label}: horizontal overflow (scrollWidth ${sw} > ${cw})`);
  else console.log(`ok   ${label}: no horizontal overflow (${sw} ≤ ${cw})`);
}

/** Lit cells of a fluid (@glyphcss/fluid draws one <pre> of glyphs; U+2800 and spaces are
 *  blank), plus a hash of its text, so two samples can be compared. */
const sampleFluid = (page, selector) =>
  page.evaluate((sel) => {
    const pre = document.querySelector(sel);
    if (!pre) return null;
    let lit = 0;
    let hash = 0;
    for (const ch of pre.textContent ?? '') {
      const c = ch.codePointAt(0);
      if (c !== 0x20 && c !== 0x2800 && c !== 0x0a) lit++;
      hash = (hash * 31 + c) | 0;
    }
    return { lit, hash };
  }, selector);

const FIELD = '[data-fluid-field] pre';
const scrollToSection = (page, id) =>
  page.evaluate((id) => {
    const el = document.getElementById(id);
    window.scrollTo(0, el.getBoundingClientRect().top + scrollY - innerHeight * 0.22);
  }, id);

// Landing, desktop
{
  const page = await open(1440, 900, '/');
  await page.waitForSelector('html[data-fluid="live"]', { timeout: 20000 });
  await page.waitForTimeout(5000); // the wordmark has condensed, the first Molty has emerged
  await page.screenshot({ path: OUT + 'landing-1440-hero-a.png' });
  const a = await sampleFluid(page, FIELD);
  await page.waitForTimeout(1000);
  const b = await sampleFluid(page, FIELD);
  if (!a || a.lit < 500) fail(`page fluid not rendering (lit=${a?.lit})`);
  else if (a.hash === b.hash) fail('page fluid did not change in 1 s');
  else console.log(`ok   page fluid renders (lit ${a.lit} → ${b.lit} cells) and moves (hash ${a.hash} ≠ ${b.hash})`);
  if (!(await page.locator('html[data-fluid-moltys="live"]').count())) fail('Moltys never became ready (three.js rig / mesh target)');
  await scrollToSection(page, 'moltys');
  await page.getByRole('button', { name: 'frog' }).click();
  const slot = await page.locator('[data-fluid-slot="gallery"]').getAttribute('data-fluid-molty');
  if (slot !== 'cute-frog') fail(`gallery slot did not switch (data-fluid-molty=${slot})`);
  else console.log('ok   gallery picker drives the fluid slot');
  await page.waitForTimeout(4000);
  await page.screenshot({ path: OUT + 'landing-1440-gallery.png' });
  {
    await page.locator('[data-speed-reader]').scrollIntoViewIfNeeded();
    await page.waitForTimeout(2500);
    const r1 = await sampleFluid(page, '[data-speed-reader] pre');
    await page.waitForTimeout(700);
    const r2 = await sampleFluid(page, '[data-speed-reader] pre');
    if (!r1 || r1.lit < 200) fail(`speed reader not rendering (lit=${r1?.lit})`);
    else if (r1.hash === r2.hash) fail('speed reader did not move');
    else console.log(`ok   speed reader renders (lit ${r1.lit}) and moves`);
    await page.getByRole('button', { name: '600' }).click();
    await page.getByRole('button', { name: '[ type your own ]' }).click();
    await page.getByLabel('Text for Molty to read').fill('hello from the test');
    await page.getByRole('button', { name: '[ read ]' }).click();
    if (!(await page.getByText('“hello from the test”').count())) fail('speed reader did not take typed text');
    else console.log('ok   speed reader takes wpm + typed text');
  }
  for (const id of ['fluid', 'bridge', 'boards']) {
    await scrollToSection(page, id);
    await page.waitForTimeout(3500);
    await page.screenshot({ path: OUT + `landing-1440-${id}.png` });
  }
  await overflow(page, 'landing @1440');
  await page.close();
}

// Landing, phone
{
  const page = await open(390, 844, '/');
  await page.waitForSelector('html[data-fluid="live"]', { timeout: 20000 });
  await page.waitForTimeout(5000);
  await page.screenshot({ path: OUT + 'landing-390-hero.png' });
  await overflow(page, 'landing @390');
  await page.close();
}

// Docs
for (const [path, name, width] of [
  ['/getting-started/introduction/', 'docs-introduction-1440', 1440],
  ['/agents/protocol/', 'docs-protocol-1440', 1440],
  ['/agents/connect/', 'docs-connect-390', 390],
]) {
  const page = await open(width, 900, path);
  await page.screenshot({ path: OUT + `${name}.png` });
  await overflow(page, `${path} @${width}`);
  await page.close();
}

await browser.close();
if (failures.length) {
  console.error(`\n${failures.length} failure(s)`);
  process.exitCode = 1;
} else console.log(`\nall checks passed; screenshots in ${OUT}`);
