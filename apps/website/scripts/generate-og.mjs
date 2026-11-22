// Render public/og.png (1200×630) from the REAL page: open the landing in a running preview at
// the card's size, wait for "molty.UNO" to condense and the hero's screen to boot to the cat,
// hide the header, and capture the hero. Re-run when the look changes; the PNG is committed.
//   pnpm build && pnpm preview   (another terminal)
//   pnpm og [baseUrl=http://localhost:4330] [out=public/og.png]
import { chromium } from 'playwright';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const BASE = process.argv[2] ?? 'http://localhost:4330';
const OUT = process.argv[3] ? resolve(process.argv[3]) : fileURLToPath(new URL('../public/og.png', import.meta.url));

const browser = await chromium.launch();
const context = await browser.newContext({ viewport: { width: 1200, height: 630 }, deviceScaleFactor: 1, colorScheme: 'dark' });
const page = await context.newPage();
await page.goto(BASE + '/', { waitUntil: 'networkidle' });
await page.waitForSelector('html[data-fluid-moltys="live"]', { timeout: 20000 });
// card framing: no header, the hero pulled up so wordmark, lede and Molty all fit 630 px
await page.addStyleTag({
  content: `body > div:first-of-type, section[aria-label="molty.UNO"] > p:last-child { display: none !important; }
    section[aria-label="molty.UNO"] { min-height: 0 !important; padding-block: 40px 0 !important; justify-content: flex-start !important; }`,
});
await page.waitForTimeout(8000); // wordmark condensed, the device booted, the cat up
await page.screenshot({ path: OUT });
await browser.close();
console.log(`wrote ${OUT}`);
