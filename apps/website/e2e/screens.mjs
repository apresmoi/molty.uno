// Screenshot + smoke check against a running preview (`pnpm build && pnpm preview`).
// Writes apps/website/e2e-out/*.png and fails on: page errors, horizontal overflow at 390px,
// a page with no dust, a title the renderer does not draw, a tap that sends no wave, a Molty
// screen that does not draw or move, a stage
// whose buttons do not reach the cat, or a talk screen that does not ask and take an answer.
//   node e2e/screens.mjs [baseUrl=http://localhost:4330]
import { chromium } from 'playwright';
import sharp from 'sharp';
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

/** A Molty screen's canvas (the firmware's renderer draws into it): its lit pixels and a hash
 *  of them, so two samples can be compared. */
const sampleScreen = (page, selector) =>
  page.evaluate((sel) => {
    const c = document.querySelector(sel);
    if (!c) return null;
    const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
    let lit = 0;
    let hash = 0;
    for (let i = 0; i < d.length; i += 4) {
      const v = d[i] + d[i + 1] + d[i + 2];
      if (v > 120) lit++;
      hash = (hash * 31 + v) | 0;
    }
    return { lit, hash };
  }, selector);
/** `selector`'s screen draws (`min` lit pixels) and moves within `ms`. */
async function screenLives(page, selector, what, min = 1500, ms = 1500) {
  const a = await sampleScreen(page, selector);
  await page.waitForTimeout(ms);
  const b = await sampleScreen(page, selector);
  if (!a || a.lit < min) fail(`${what} not drawing (lit=${a?.lit})`);
  else if (a.hash === b.hash) fail(`${what} did not move in ${ms} ms`);
  else console.log(`ok   ${what} draws (${a.lit} px lit) and moves`);
}

const scrollToSection = (page, id) =>
  page.evaluate((id) => {
    const el = document.getElementById(id);
    window.scrollTo(0, el.getBoundingClientRect().top + scrollY - innerHeight * 0.22);
  }, id);

// Landing, desktop
{
  const page = await open(1440, 900, '/');
  await page.waitForSelector('html[data-fluid="live"]', { timeout: 20000 });
  await page.waitForTimeout(8000); // the wordmark is drawn, the cat has walked in and sat
  await page.screenshot({ path: OUT + 'landing-1440-hero-a.png' });
  // the page's water: its dust is the page's own background, the title a word the renderer draws,
  // and a tap sends a wave through the page
  const dust = await page.evaluate(() => getComputedStyle(document.querySelector('[data-water]')).backgroundImage);
  if (!/dust|\.png/.test(dust)) fail(`the page has no dust (background-image: ${dust})`);
  else console.log('ok   the page wears its dust');
  await screenLives(page, 'section[aria-label="molty.UNO"] h1 canvas', "the title's word", 1200, 1500);
  const lit = () =>
    page.evaluate(() => {
      const c = document.querySelector('[data-page-water]');
      const d = c.width && c.height ? c.getContext('2d').getImageData(0, 0, c.width, c.height).data : [];
      let n = 0;
      for (let i = 3; i < d.length; i += 4) if (d[i]) n++;
      return n;
    });
  const calm = await lit();
  await page.mouse.click(300, 620);
  await page.waitForTimeout(700);
  const wave = await lit();
  if (calm !== 0 || wave < 300) fail(`a tap sends no wave through the page (${calm} px lit before, ${wave} after)`);
  else console.log(`ok   a tap sends a wave through the page (${wave} px lit)`);
  if (!(await page.locator('html[data-fluid-moltys="live"]').count())) fail("the Moltys never came up (the firmware's renderer: molty.wasm, the cat's package)");
  await screenLives(page, '[data-molty-screen]:has([data-device="arrive"]) canvas:not([data-outside])', "the hero's screen");
  await scrollToSection(page, 'moltys');
  await page.waitForTimeout(3000); // the device has turned in, its screen is on
  await screenLives(page, '[data-stage="gallery"] canvas', "the stage's screen");
  const eat = page.locator('#moltys ~ * button, section:has(#moltys) button').filter({ hasText: /^eat$/ }).first();
  await eat.click();
  await page.waitForTimeout(2500);
  if ((await eat.getAttribute('aria-pressed')) !== 'true') fail('the stage did not take "eat" (the button is not pressed)');
  else console.log('ok   the stage plays what is asked (eat)');
  await page.screenshot({ path: OUT + 'landing-1440-gallery.png' });
  {
    const reader = page.locator('[data-speed-reader]');
    await reader.locator('canvas').scrollIntoViewIfNeeded();
    await page.waitForTimeout(3500);
    await screenLives(page, '[data-speed-reader] canvas', "the talk screen", 300, 900);
    await page.getByRole('button', { name: '600' }).click();
    await page.getByRole('button', { name: '[ type your own ]' }).click();
    await page.getByLabel('Text for Molty to read').fill('hello from the test. ok?');
    await page.getByRole('button', { name: '[ read ]' }).click();
    if (!(await page.getByText('“hello from the test. ok?”').count())) fail('the talk screen did not take typed text');
    else console.log('ok   the talk screen takes wpm + typed text');
    // the device reads it, then asks on the rim; an answer comes back through the renderer
    await reader.and(page.locator('[data-phase="listen"]')).waitFor({ timeout: 15000 }).catch(() => fail('the talk screen never asked'));
    await page.waitForTimeout(2000); // the question has formed
    await page.screenshot({ path: OUT + 'landing-1440-ask.png' });
    await page.getByRole('group', { name: 'Answer Molty' }).getByRole('button', { name: 'yes' }).click();
    await page.getByText('you tapped yes').waitFor({ timeout: 5000 }).then(
      () => console.log('ok   the talk screen asks and takes an answer'),
      () => fail('the answer did not come back from the screen'),
    );
  }
  for (const id of ['talk', 'bridge', 'boards']) {
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

// Showcase
{
  const page = await open(1440, 900, '/showcase/');
  await page.waitForSelector('html[data-fluid-moltys="live"]', { timeout: 20000 });
  await page.waitForTimeout(3000);
  await screenLives(page, '[data-stage="showcase"] canvas', "the showcase's screen");
  await page.getByRole('button', { name: 'chart' }).click();
  await page.waitForTimeout(5000);
  await page.screenshot({ path: OUT + 'showcase-1440-chart.png' });
  await overflow(page, 'showcase @1440');
  await page.close();
}
{
  const page = await open(390, 844, '/showcase/');
  await page.waitForSelector('html[data-fluid-moltys="live"]', { timeout: 20000 });
  await page.waitForTimeout(3000);
  await page.screenshot({ path: OUT + 'showcase-390.png' });
  await overflow(page, 'showcase @390');
  await page.close();
}

// Docs
for (const [path, name, width] of [
  ['/getting-started/introduction/', 'docs-introduction-1440', 1440],
  ['/agents/protocol/', 'docs-protocol-1440', 1440],
  ['/agents/connect/', 'docs-connect-390', 390],
  ['/build/', 'build-1440', 1440],
  ['/build/', 'build-390', 390],
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
