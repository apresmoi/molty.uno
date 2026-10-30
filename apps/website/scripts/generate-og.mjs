// Render public/og.png (1200×630) from the REAL page fluid: open the landing in a running
// preview at the card's size, wait for "molty.UNO" to condense and the first Molty to
// emerge, hide the header, and capture the hero. Re-run when the look changes; the PNG is
// committed.
//   pnpm build && pnpm preview   (another terminal)
//   pnpm og [baseUrl=http://localhost:4330]
import { chromium } from 'playwright';
import { fileURLToPath } from 'node:url';

const BASE = process.argv[2] ?? 'http://localhost:4330';
const OUT = fileURLToPath(new URL('../public/og.png', import.meta.url));

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
await page.waitForTimeout(7000); // wordmark condensed, the first Molty mid-dance
await page.screenshot({ path: OUT });
await browser.close();
console.log(`wrote ${OUT}`);
