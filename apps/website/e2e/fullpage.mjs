// The whole landing, top to bottom, at 1280 and 390 px wide, dark and light, against a running
// preview: e2e-out/<label>-<width>-<theme>.png. Every section is scrolled through first, so its
// device has turned in and its screen is on.
//   node e2e/fullpage.mjs [baseUrl=http://localhost:4330] [label=landing-full]
import { chromium } from 'playwright';
import { mkdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

const BASE = process.argv[2] ?? 'http://localhost:4330';
const LABEL = process.argv[3] ?? 'landing-full';
const OUT = fileURLToPath(new URL('../e2e-out/', import.meta.url));
mkdirSync(OUT, { recursive: true });
const browser = await chromium.launch();
for (const theme of ['dark', 'light'])
  for (const [width, height] of [
    [1280, 800],
    [390, 844],
  ]) {
    const ctx = await browser.newContext({ viewport: { width, height }, deviceScaleFactor: 1, colorScheme: theme });
    const page = await ctx.newPage();
    await page.goto(BASE + '/', { waitUntil: 'networkidle' });
    await page.waitForTimeout(6000);
    const total = await page.evaluate(() => document.documentElement.scrollHeight);
    for (let y = 0; y < total; y += height * 0.6) {
      await page.evaluate((to) => window.scrollTo(0, to), y);
      await page.waitForTimeout(1800);
    }
    await page.evaluate(() => window.scrollTo(0, 0));
    await page.waitForTimeout(1500);
    await page.screenshot({ path: `${OUT}${LABEL}-${width}-${theme}.png`, fullPage: true });
    console.log(`wrote ${LABEL}-${width}-${theme}.png`);
    await ctx.close();
  }
await browser.close();
