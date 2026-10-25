// Render public/og.png (1200×630) from the REAL fluid: open the landing in a running
// preview, wait for the hero Molty to emerge, grab its canvas, and place it on a card
// with the wordmark. Re-run when the look changes; the PNG is committed.
//   pnpm build && pnpm preview   (another terminal)
//   pnpm og [baseUrl=http://localhost:4330]
import { chromium } from 'playwright';
import { fileURLToPath } from 'node:url';

const BASE = process.argv[2] ?? 'http://localhost:4330';
const OUT = fileURLToPath(new URL('../public/og.png', import.meta.url));

const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1440, height: 900 }, deviceScaleFactor: 2 });
await page.goto(BASE + '/', { waitUntil: 'networkidle' });
await page.waitForSelector('canvas.molty-screen', { timeout: 20000 });
await page.waitForTimeout(7500); // blob → "hello" → the first Molty, mid-dance
const screen = await page.evaluate(() => document.querySelector('canvas.molty-screen').toDataURL('image/png'));

const card = await browser.newPage({ viewport: { width: 1200, height: 630 }, deviceScaleFactor: 1 });
await card.setContent(`<!doctype html><meta charset="utf-8"><style>
  * { margin: 0; box-sizing: border-box; }
  body { width: 1200px; height: 630px; background: #000; color: #d8e2dc; overflow: hidden;
    font-family: -apple-system, BlinkMacSystemFont, "Inter", "Segoe UI", Roboto, sans-serif; position: relative; }
  .copy { position: absolute; left: 80px; top: 0; bottom: 0; width: 520px; display: flex; flex-direction: column; justify-content: center; gap: 26px; }
  .mark { font: 700 34px ui-monospace, Menlo, monospace; }
  .mark b { color: #9fe8c4; margin-right: 12px; }
  h1 { font-size: 48px; line-height: 1.08; letter-spacing: -0.02em; font-weight: 600; }
  h1 span { color: #9fe8c4; }
  p { font: 22px/1.45 inherit; color: #7d8a83; }
  .url { font: 18px ui-monospace, Menlo, monospace; color: #39423d; }
  .glass { position: absolute; right: 80px; top: 85px; width: 460px; height: 460px; border-radius: 50%; overflow: hidden;
    box-shadow: 0 0 0 10px #0b0d0c, 0 0 0 11px #1d2320, 0 0 120px rgba(159,232,196,.08); }
  .glass img { width: 100%; height: 100%; display: block; }
</style>
<div class="copy">
  <div class="mark"><b>⠶</b>molty</div>
  <h1>A calm creature of characters <span>for your desk.</span></h1>
  <p>Your AI agent is its brain. Open source.</p>
  <div class="url">molty.uno</div>
</div>
<div class="glass"><img src="${screen}"></div>`);
await card.waitForTimeout(300);
await card.screenshot({ path: OUT, clip: { x: 0, y: 0, width: 1200, height: 630 } });
await browser.close();
console.log(`wrote ${OUT}`);
