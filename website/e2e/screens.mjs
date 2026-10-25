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

/** Lit pixels of the hero canvas, plus a hash, so two samples can be compared. */
const sampleCanvas = (page, selector) =>
  page.evaluate((sel) => {
    const cv = document.querySelector(sel);
    if (!cv) return null;
    const c = document.createElement('canvas');
    c.width = 160;
    c.height = 160;
    const g = c.getContext('2d');
    g.drawImage(cv, 0, 0, 160, 160);
    const d = g.getImageData(0, 0, 160, 160).data;
    let lit = 0;
    let hash = 0;
    for (let i = 0; i < d.length; i += 4) {
      const v = d[i] + d[i + 1] + d[i + 2];
      if (v > 60) lit++;
      hash = (hash * 31 + v) | 0;
    }
    return { lit, hash };
  }, selector);

// Landing, desktop
{
  const page = await open(1440, 900, '/');
  const canvas = 'canvas.molty-screen';
  await page.waitForSelector(canvas, { timeout: 20000 });
  await page.waitForTimeout(6000); // past blob + "hello" → the first Molty has emerged
  await page.screenshot({ path: OUT + 'landing-1440-hero-a.png' });
  const a = await sampleCanvas(page, canvas);
  await page.waitForTimeout(1000);
  await page.screenshot({ path: OUT + 'landing-1440-hero-b.png' });
  const b = await sampleCanvas(page, canvas);
  if (!a || a.lit < 500) fail(`hero canvas not rendering (lit=${a?.lit})`);
  else if (a.hash === b.hash) fail('hero canvas did not change in 1 s');
  else console.log(`ok   hero renders (lit ${a.lit} → ${b.lit} px) and moves (hash ${a.hash} ≠ ${b.hash})`);
  await page.locator('#moltys').scrollIntoViewIfNeeded();
  await page.getByRole('button', { name: 'Axolotl' }).click();
  await page.waitForTimeout(4000);
  await page.locator('#moltys').evaluate((el) => el.closest('section').scrollIntoView());
  await page.screenshot({ path: OUT + 'landing-1440-gallery.png' });
  const g = await sampleCanvas(page, 'section:has(#moltys) canvas.molty-screen');
  if (!g || g.lit < 500) fail(`gallery preview not rendering (lit=${g?.lit})`);
  else console.log(`ok   gallery live preview renders (lit ${g.lit})`);
  await page.evaluate(() => window.scrollTo(0, 0));
  await page.waitForTimeout(300);
  await page.screenshot({ path: OUT + 'landing-1440-full.png', fullPage: true });
  await overflow(page, 'landing @1440');
  await page.close();
}

// Landing, phone
{
  const page = await open(390, 844, '/');
  await page.waitForSelector('canvas.molty-screen', { timeout: 20000 });
  await page.waitForTimeout(6000);
  await page.screenshot({ path: OUT + 'landing-390-hero.png' });
  await page.screenshot({ path: OUT + 'landing-390-full.png', fullPage: true });
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
