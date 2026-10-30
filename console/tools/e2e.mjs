// Headless Chromium walk-through against the mock device (tools/mock-device.mjs on
// :8170 — start it with `pnpm mock` after `pnpm build`), at phone width (390 px):
//   1. the 5-step setup flow (Wi-Fi → Molty → Agent → Tap → Done), screenshot per step
//   2. the same Molty step with the three.js CDN blocked → live device preview fallback
//   3. the settings pages (agent single-message design, molty picker, degraded firmware)
// Then an optional READ-ONLY pass against a real device via ?device= (GETs only, plus
// harmless fluid orders that it restores). It never scans, connects, forgets or uploads there.
//
//   node tools/e2e.mjs [outDir] [realDevice]
import { chromium } from 'playwright';
import { mkdirSync, readFileSync } from 'node:fs';

const OUT = process.argv[2] ?? './e2e-out';
const REAL = process.argv[3] ?? '';
const MOCK = 'http://localhost:8170';
const PHONE = { width: 390, height: 844 };
mkdirSync(OUT, { recursive: true });

const errors = [];
// Known-benign: headless-GPU readback notes from the viewer engine (three → canvas), our
// own canvas sampling, and the CDN request the offline pass blocks on purpose.
const BENIGN = [/GL Driver Message/, /willReadFrequently/];
const checks = [];
const ok = (cond, what) => { checks.push(`${cond ? 'PASS' : 'FAIL'} ${what}`); if (!cond) process.exitCode = 1; };
const mockLog = async () => (await fetch(`${MOCK}/__mock/log`)).json();
const mock = (path, c = {}) => fetch(`${MOCK}/__mock/${path}`, { method: 'POST', body: JSON.stringify(c) });
const wait = (ms) => new Promise((r) => setTimeout(r, ms));

const browser = await chromium.launch();
async function newPage(tag, { offline = false, viewport = PHONE } = {}) {
  const ctx = await browser.newContext({ viewport, deviceScaleFactor: 2 });
  await ctx.grantPermissions(['clipboard-read', 'clipboard-write'], { origin: MOCK });
  if (offline) await ctx.route('https://cdn.jsdelivr.net/**', (r) => r.abort('internetdisconnected'));
  // The status page asks the device's relay for the latest release: answer from the mock.
  await ctx.route('**/v1/releases/latest*', async (r) => r.fulfill({ response: await ctx.request.get(`${MOCK}/__mock/release?${new URL(r.request().url()).searchParams}`), headers: { 'access-control-allow-origin': '*' } }));
  const page = await ctx.newPage();
  page.on('console', (m) => {
    if (m.type() !== 'error' && m.type() !== 'warning') return;
    if (BENIGN.some((r) => r.test(m.text())) || (offline && /ERR_INTERNET_DISCONNECTED/.test(m.text()))) return;
    errors.push(`[${tag}] console.${m.type()}: ${m.text()}`);
  });
  page.on('pageerror', (e) => errors.push(`[${tag}] pageerror: ${e.message}`));
  page.on('requestfailed', (r) => { if (!(offline && r.url().includes('cdn.jsdelivr.net'))) errors.push(`[${tag}] requestfailed: ${r.url()} ${r.failure()?.errorText}`); });
  return page;
}
const shot = (page, name) => page.screenshot({ path: `${OUT}/${name}.png`, fullPage: true });
const bubble = (page) => page.locator('.setup .bubble').innerText();
const noOverflow = async (page, what) =>
  ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth && [...document.querySelectorAll('.setup *')].every((e) => e.getBoundingClientRect().right <= innerWidth + 0.5)),
    `${what}: no horizontal overflow at 390px`);
/** Count lit + orange-accent pixels on the fluid canvas (the code must not have an ORP-orange digit). */
const canvasStats = (page) => page.evaluate(() => {
  const c = document.querySelector('.fstage canvas'); const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
  let lit = 0, orange = 0;
  for (let i = 0; i < d.length; i += 4) { const [r, g, b] = [d[i], d[i + 1], d[i + 2]]; if (r + g + b > 90) lit++; if (r > 200 && g > 90 && g < 180 && b < 90) orange++; }
  return { lit, orange };
});

// ── 1. setup flow ─────────────────────────────────────────────────────────────
{
  await mock('config', { link: true, file: true, reset: true, wifi: 'none' });
  const page = await newPage('setup');
  await page.goto(`${MOCK}/`);
  await page.waitForURL(/#\/setup\/wifi/);
  ok(true, 'first run lands on #/setup/wifi');
  await page.locator('.setup .opt', { hasText: 'FRITZ!Box' }).waitFor();
  ok((await bubble(page)).startsWith('Hi! Which Wi-Fi should I use?'), 'wifi: bubble asks one question');
  ok(await page.locator('.setup .opt.net').count() === 3, 'wifi: scanned list deduped (3 networks)');
  ok(await page.locator('.prog').innerText() === '● Wi-Fi   ○ Molty   ○ Agent', 'wifi: progress line');
  await page.locator('.fstage canvas').first().waitFor({ timeout: 15000 });
  await wait(1200);
  await noOverflow(page, 'wifi');
  await shot(page, '01-wifi');

  await page.locator('.setup .opt', { hasText: 'FRITZ!Box' }).click();
  ok((await bubble(page)).includes('password for FRITZ!Box 7590 QX'), 'pass: asks for the password');
  await page.getByLabel('password').fill('wrong');
  await page.getByRole('button', { name: 'Connect' }).click();
  await page.locator('.setup .bubble', { hasText: 'that didn’t work' }).waitFor();
  ok(true, 'pass: wrong password → bubble says so, stays on password');
  await noOverflow(page, 'pass');
  await shot(page, '02-wifi-wrong-password');
  await page.getByLabel('password').fill('hunter2hunter2');
  await page.getByRole('button', { name: 'Connect' }).click();
  await page.locator('.setup .bubble', { hasText: 'Connecting' }).waitFor();
  await page.waitForURL(/#\/setup\/molty/);
  const sc = (await mockLog()).filter((l) => l.path === '/api/set_creds').map((l) => JSON.parse(l.body));
  ok(sc.at(-1)?.ssid === 'FRITZ!Box 7590 QX' && sc.at(-1)?.password === 'hunter2hunter2', 'set_creds body {ssid,password}');
  ok((await bubble(page)).startsWith('Connected! Now, who should I be?'), 'wifi → molty auto-advance on success');

  // Molty
  await page.locator('.setup .tile img').first().waitFor();
  const tiles = await page.locator('.setup .tile').count();
  ok(tiles === 29, `molty: grid has all 29 Moltys (${tiles})`);
  const thumbsOk = await page.evaluate(() => [...document.querySelectorAll('.setup .tile img')].every((i) => i.complete && i.naturalWidth === 160));
  ok(thumbsOk, 'molty: 29 bundled thumbnails decoded');
  ok(!/creature/i.test(await page.locator('.setup').innerText()), 'molty: no "creature" in the copy');
  ok(await page.getByRole('button', { name: 'Pick one' }).isDisabled(), 'molty: button disabled until a pick');
  await page.locator('.setup .tile', { hasText: 'Frog' }).click();
  await page.locator('.setup .bubble small', { hasText: 'On your Molty now' }).waitFor();
  const log = await mockLog();
  const up = log.filter((l) => l.path.startsWith('/api/creature/file'));
  const frog = readFileSync(new URL('../../creatures/molty/frog.molty', import.meta.url));
  ok(up.length === 1 && up[0].path === '/api/creature/file?name=cute-frog' && up[0].bytes === frog.length && up[0].head === 'MLTY', `molty: frog.molty uploaded raw (${up[0]?.bytes} B, magic ${up[0]?.head})`);
  ok(up[0]?.ctype?.startsWith('text/plain'), 'molty: upload is a CORS-simple request');
  ok(log.some((l) => l.path === '/api/creature/state' && JSON.parse(l.body).state === 'creature'), 'molty: device told to show the creature state');
  ok(await page.getByRole('button', { name: 'Choose Frog' }).isEnabled(), 'molty: "Choose Frog"');
  ok((await page.locator('.fstage').getAttribute('data-mode')) === 'fluid', 'molty: round screen runs the real fluid engine (three from CDN)');
  await wait(3500); // let it emerge and start dancing
  const st = await canvasStats(page);
  ok(st.lit > 2000, `molty: Frog drawn on the round screen (${st.lit} lit px)`);
  await noOverflow(page, 'molty');
  await shot(page, '03-molty');
  await page.getByRole('button', { name: 'Choose Frog' }).click();

  // Agent
  await page.waitForURL(/#\/setup\/agent/);
  await page.locator('.msg pre', { hasText: 'molty-k7m2w9c4ya' }).waitFor();
  const msg = await page.locator('.msg pre').innerText();
  const MSG = "Hi! Please connect yourself to my Molty.\n\nInstructions: https://molty-relay.example.workers.dev/skill.md\nDevice: molty-k7m2w9c4ya\nCode: K7QX2MPA\n\nI'll tap Molty to accept you.";
  ok(msg === MSG, 'agent: the one message has real device id, code, https relay /skill.md');
  ok((await bubble(page)).startsWith('Who will be my brain?'), 'agent: bubble');
  await page.getByRole('button', { name: 'Copy message' }).click();
  ok(await page.evaluate(() => navigator.clipboard.readText()) === MSG, 'agent: Copy message → clipboard has the message');
  ok(await page.getByRole('button', { name: 'Copied ✓' }).isVisible(), 'agent: button confirms the copy');
  await page.locator('.wait', { hasText: 'Waiting for your agent' }).waitFor();
  await wait(2500);
  ok(!/#\/setup\/done/.test(page.url()), 'agent: an older last_pair does not skip ahead');
  await noOverflow(page, 'agent');
  await shot(page, '04-agent');
  await page.locator('summary', { hasText: 'Advanced' }).click();
  const adv = await page.locator('.adv').innerText();
  ok(adv.includes('molty-k7m2w9c4ya') && adv.includes('K7QX2MPA') && adv.includes('molty pair --relay https://molty-relay.example.workers.dev') && adv.includes('https://molty-relay.example.workers.dev/mcp'),
    'agent: Advanced has device id, code, CLI pair command, MCP fallback');
  await page.getByLabel('relay url').fill('https://relay.example.org/');
  await page.getByRole('button', { name: 'Save' }).click();
  await page.locator('.adv-msg', { hasText: 'Saved' }).waitFor();
  ok((await mockLog()).some((l) => l.path === '/api/link' && JSON.parse(l.body).relay_url === 'wss://relay.example.org'), 'agent: relay POSTed as ws origin');
  ok((await page.locator('.msg pre').innerText()).includes('https://relay.example.org/skill.md'), 'agent: message follows the new relay');
  await noOverflow(page, 'agent advanced');
  await shot(page, '05-agent-advanced');

  // Tap
  await mock('pair', { agent_name: 'Grok', code: '4821' });
  await page.locator('.setup .bubble', { hasText: 'Tap me if you see this code' }).waitFor({ timeout: 5000 });
  ok((await bubble(page)).includes('Grok wants to connect'), 'tap: auto-advance on pair, names the agent');
  await wait(2500);
  const tap = await canvasStats(page);
  ok(tap.lit > 800 && tap.orange < 40, `tap: code condensed in the fluid, uniform (lit ${tap.lit}, orange ${tap.orange})`);
  await noOverflow(page, 'tap');
  await shot(page, '06-tap');

  // a rejected request → back to the agent step
  await mock('decide', { ok: false, reason: 'denied' });
  await page.locator('.setup .bubble', { hasText: 'That didn’t work (denied). Send the message again?' }).waitFor({ timeout: 5000 });
  ok(/#\/setup\/agent/.test(page.url()), 'failed pair → back to the agent step');
  await shot(page, '07-agent-failed');

  // Done
  await mock('pair', { agent_name: 'Grok', code: '1937' });
  await page.locator('.setup .bubble', { hasText: 'Tap me' }).waitFor({ timeout: 5000 });
  await mock('decide', { ok: true });
  await page.locator('.setup .bubble', { hasText: 'I’m Frog, and Grok is my brain.' }).waitFor({ timeout: 5000 });
  ok(true, 'done: "I’m Frog, and Grok is my brain."');
  ok(await page.locator('.prog').innerText() === '✓ Wi-Fi   ✓ Molty   ✓ Agent', 'done: progress all ticked');
  await wait(2500);
  ok((await canvasStats(page)).lit > 2000, 'done: Molty keeps dancing on the screen');
  await noOverflow(page, 'done');
  await shot(page, '08-done');
  await page.getByRole('button', { name: 'Open Molty' }).click();
  await page.waitForURL(/#\/status/);
  await page.locator('.facts dd.hi', { hasText: 'molty-k7m2w9c4ya' }).waitFor();
  await page.goto(`${MOCK}/`);
  await page.waitForURL(/#\/status/);
  ok(true, 'after setup, / opens the status page');
  await shot(page, '09-status');
  await page.context().close();
}

// ── 2. offline: no three.js → live device preview on the round screen ──────
{
  await mock('config', { reset: true });
  const page = await newPage('offline', { offline: true });
  await page.goto(`${MOCK}/#/setup/molty`);
  await page.locator('.fstage[data-mode="device"] .pv-status', { hasText: 'live' }).waitFor({ timeout: 20000 });
  ok(true, 'offline: round screen falls back to the live device preview');
  await page.locator('.setup .tile', { hasText: 'Duck' }).click();
  await page.locator('.setup .bubble small', { hasText: 'On your Molty now' }).waitFor();
  ok((await mockLog()).some((l) => l.path === '/api/creature/file?name=cute-duck' && l.head === 'MLTY'), 'offline: picking still uploads the bundled .molty');
  await wait(800);
  await noOverflow(page, 'offline molty');
  await shot(page, '10-molty-offline');
  await page.context().close();
}

// ── 3. settings pages + degraded firmware ────────────────────────────────────
{
  await mock('config', { reset: true });
  const page = await newPage('settings');
  await page.goto(`${MOCK}/#/agent`);
  await page.locator('.msg pre', { hasText: 'molty-k7m2w9c4ya' }).waitFor();
  ok(await page.getByRole('button', { name: 'Copy message' }).isVisible(), 'settings agent: same single-message design');
  await mock('pair', { agent_name: 'Claude', code: '5150' });
  await page.locator('.link-state', { hasText: 'tap molty to accept Claude' }).waitFor({ timeout: 5000 });
  ok(true, 'settings agent: shows a pending pair');
  ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'settings agent: no horizontal scroll at 390px');
  await shot(page, '11-settings-agent');
  await mock('decide', { ok: true });

  await page.goto(`${MOCK}/#/molty`);
  await page.locator('.creature img').first().waitFor();
  ok(await page.locator('.creature').count() === 29, 'settings molty: 29 Moltys with thumbnails');
  await page.locator('.creature', { hasText: 'Owl' }).click();
  await page.getByRole('button', { name: 'send Owl to molty' }).click();
  await page.locator('.note.ok', { hasText: 'moving in' }).waitFor();
  ok((await mockLog()).some((l) => l.path === '/api/creature/file?name=cute-owl' && l.head === 'MLTY'), 'settings molty: upload');
  const ringRow = page.getByRole('group', { name: 'listening ring colour' });
  ok(await ringRow.getByRole('button', { name: 'accent (default)' }).getAttribute('aria-pressed') === 'true', 'settings molty: listening ring defaults to the accent');
  await ringRow.getByRole('button', { name: 'violet' }).click();
  await ringRow.locator('button[aria-pressed="true"]', { hasText: 'violet' }).waitFor();
  ok((await mockLog()).some((l) => l.path === '/api/creature/state' && JSON.parse(l.body).ring_color === '#b197fc'), 'settings molty: ring colour sent to the device');
  ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'settings molty: no horizontal scroll at 390px');
  await shot(page, '12-settings-molty');

  await mock('config', { file: false });
  await page.getByRole('button', { name: 'send Owl to molty' }).click();
  await page.locator('.note.warn', { hasText: "can't take new moltys yet" }).waitFor();
  ok(true, 'settings molty: creature-file 404 → graceful notice');

  // pre-contract firmware: link without pair/last_pair still waits; no link at all → skip offered
  await mock('legacy', { on: true });
  await page.goto(`${MOCK}/#/setup/agent`);
  await page.locator('.wait', { hasText: 'Waiting for your agent' }).waitFor();
  await wait(1500);
  ok(/#\/setup\/agent/.test(page.url()), 'legacy link (no pair fields): agent step waits, no crash');
  await mock('config', { link: false });
  await page.reload();
  await page.locator('.setup .bubble', { hasText: 'firmware update' }).waitFor({ timeout: 5000 });
  await page.getByRole('button', { name: 'Skip for now' }).click();
  await page.locator('.setup .bubble', { hasText: '!' }).waitFor();
  ok((await bubble(page)).includes('Give me a brain any time'), 'no /api/link → "Skip for now" → done');
  await shot(page, '13-link-unsupported-done');
  await mock('config', { reset: true, link: true, file: true });
  await page.context().close();
}

// ── firmware update on the status page ──────────────────────────────────────
{
  await mock('config', { reset: true });
  const page = await newPage('update');
  await page.goto(`${MOCK}/#/status`);
  const card = page.locator('.update .card');
  await card.locator('b', { hasText: /^update available: \d+\.\d+\.\d+$/ }).waitFor({ timeout: 10000 });
  ok(await card.locator('a', { hasText: /^molty-.*-ota\.bin$/ }).count() === 1, 'update: older firmware gets the manual path (download link + install)');
  await card.locator('summary', { hasText: 'what’s new' }).click();
  ok((await card.locator('pre').innerText()).includes('tap an answer'), 'update: what’s new shows the release notes');
  await page.setInputFiles('input[type=file]', { name: 'molty-ota.bin', mimeType: 'application/octet-stream', buffer: Buffer.alloc(4096, 0xe9) });
  await card.getByRole('button', { name: 'Install' }).click();
  await card.getByRole('button', { name: 'Sure? Molty restarts' }).click();
  await card.locator('.note', { hasText: 'restarting' }).waitFor();
  ok((await mockLog()).some((l) => l.path === '/api/ota' && l.bytes === 4096), 'update: the chosen image is uploaded to /api/ota');
  await noOverflow(page, 'update manual');
  await shot(page, '14-update-manual');
  await mock('config', { reset: true, update: true });
  await page.reload();
  await card.getByRole('button', { name: 'Update' }).click();
  await card.getByRole('button', { name: 'Sure? Update now' }).click();
  await page.locator('.update .note', { hasText: /updated to \d+\.\d+\.\d+ ✓/ }).waitFor({ timeout: 15000 });
  const post = (await mockLog()).find((l) => l.path === '/api/update');
  ok(!!post && /releases\/download\/v.*-ota\.bin/.test(JSON.parse(post.body).url) && JSON.parse(post.body).sha256 === 'd'.repeat(64), 'update: self-updating firmware gets Update, which posts the relay’s official image + sha256');
  ok((await page.locator('.facts dd').nth(1).innerText()).startsWith((await (await fetch(`${MOCK}/__mock/release`)).json()).latest.version), 'update: status shows the new firmware afterwards');
  await shot(page, '15-update-self');
  // the new image does not stick: Molty comes back on the old firmware -> rolled back, Update offered again
  await mock('config', { reset: true, update: true, rollback: true });
  await page.reload();
  await card.getByRole('button', { name: 'Update' }).click();
  await card.getByRole('button', { name: 'Sure? Update now' }).click();
  await card.locator('.note', { hasText: /did not stick/ }).waitFor({ timeout: 15000 });
  ok(await card.getByRole('button', { name: 'Update' }).count() === 1, 'update: back on the old firmware ends the install as rolled back and offers Update again');
  await shot(page, '16-update-rolled-back');
  // the download fails (a transient TLS error): the failure shows and Update is offered again
  await mock('config', { reset: true, update: true, fail: true });
  await page.reload();
  await card.getByRole('button', { name: 'Update' }).click();
  await card.getByRole('button', { name: 'Sure? Update now' }).click();
  await card.locator('.note', { hasText: /update failed: tls/ }).waitFor({ timeout: 15000 });
  ok(await card.getByRole('button', { name: 'Update' }).count() === 1, 'update: a failed download shows why and offers Update again');
  await mock('config', { reset: true });
  await page.context().close();
}

// ── real device: read-only (+ harmless, restored fluid orders) ───────────────
if (REAL) {
  const page = await newPage('real', { viewport: { width: 1100, height: 900 } });
  await page.goto(`${MOCK}/?device=${encodeURIComponent(REAL)}#/status`);
  await page.locator('.facts dd.hi', { hasText: /^molty-/ }).waitFor({ timeout: 15000 });
  await page.locator('.pv-status', { hasText: 'live' }).waitFor({ timeout: 15000 });
  await shot(page, '20-status-real');
  await page.goto(`${MOCK}/?device=${encodeURIComponent(REAL)}#/agent`);
  await page.locator('.msg pre', { hasText: 'molty-' }).waitFor({ timeout: 10000 });
  await shot(page, '21-agent-real');
  await page.goto(`${MOCK}/?device=#/status`); // clear the remembered override
  await page.context().close();
}

await browser.close();
console.log(checks.join('\n'));
console.log(errors.length ? `CONSOLE ERRORS (${errors.length}):\n` + errors.join('\n') : 'console errors: 0');
if (errors.length) process.exitCode = 1;
