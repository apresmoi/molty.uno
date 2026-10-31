// Theme-label check against a running preview (`pnpm build && pnpm preview`): the theme button
// must name the right theme on its FIRST painted frame and never change before a click (no
// "[ theme ]" → "[ dark ]" flash while the island hydrates). Samples the button's visible label
// on every frame from document start, on a slow network and CPU, for the landing and a docs page
// in both themes, then clicks it and checks the label and <html data-theme> flip together.
//   node e2e/theme-label.mjs [baseUrl=http://localhost:4330]
import { chromium } from 'playwright';

const BASE = process.argv[2] ?? 'http://localhost:4330';
const failures = [];
const check = (ok, msg) => {
  if (ok) console.log(`ok   ${msg}`);
  else {
    failures.push(msg);
    console.error(`FAIL ${msg}`);
  }
};

/** Runs from document start: each frame records the theme button's visible label. */
function sampler() {
  const frames = (window.__labels = []);
  const tick = () => {
    const button = [...document.querySelectorAll('button')].find((b) => /theme/i.test(b.getAttribute('aria-label') ?? ''));
    if (button) frames.push({ label: button.innerText.replace(/\s+/g, ' ').trim(), theme: document.documentElement.dataset.theme ?? null });
    requestAnimationFrame(tick);
  };
  requestAnimationFrame(tick);
}

const browser = await chromium.launch();
const PAGES = ['/', '/getting-started/introduction/'];
// [label, colour scheme, stored choice, the label the button must show (the theme it switches to)]
const CASES = [
  ['dark (system)', 'dark', null, '[ light ]'],
  ['light (system)', 'light', null, '[ dark ]'],
  ['light (stored)', 'dark', 'light', '[ dark ]'],
];

for (const path of PAGES)
  for (const [label, colorScheme, stored, want] of CASES) {
    const ctx = await browser.newContext({ viewport: { width: 1280, height: 800 }, colorScheme });
    const page = await ctx.newPage();
    const cdp = await ctx.newCDPSession(page);
    await cdp.send('Network.enable');
    await cdp.send('Network.emulateNetworkConditions', { offline: false, latency: 150, downloadThroughput: 1.6e6 / 8, uploadThroughput: 750e3 / 8 });
    await cdp.send('Emulation.setCPUThrottlingRate', { rate: 4 });
    if (stored) await page.addInitScript((v) => localStorage.setItem('starlight-theme', v), stored);
    await page.addInitScript(sampler);
    await page.goto(BASE + path, { waitUntil: 'load' });
    // hydrated: the island has mounted and set its aria-label from the theme
    await page.waitForFunction(() => [...document.querySelectorAll('button')].some((b) => /^Switch to (dark|light) theme$/.test(b.getAttribute('aria-label') ?? '')), null, { timeout: 30000 });
    await page.waitForTimeout(300);
    const frames = await page.evaluate(() => window.__labels);
    const labels = [...new Set(frames.map((f) => f.label))];
    const tag = `${path} ${label}`;
    check(frames.length > 3, `${tag}: sampled the button from early frames (${frames.length})`);
    check(labels.length === 1 && labels[0] === want, `${tag}: the label is ${want} on every frame (saw ${labels.map((l) => JSON.stringify(l)).join(' → ')})`);
    const flip = want === '[ light ]' ? '[ dark ]' : '[ light ]';
    const button = page.getByRole('button', { name: /theme$/ });
    await button.click();
    const after = await page.evaluate(() => document.documentElement.dataset.theme);
    check((await button.innerText()).replace(/\s+/g, ' ').trim() === flip && after === want.slice(2, -2), `${tag}: a click switches to ${after} and the label to ${flip}`);
    check((await button.getAttribute('aria-label')) === `Switch to ${flip.slice(2, -2)} theme`, `${tag}: the aria-label follows the click`);
    await ctx.close();
  }

await browser.close();
if (failures.length) {
  console.error(`\n${failures.length} failure(s)`);
  process.exitCode = 1;
} else console.log('\nall theme-label checks passed');
