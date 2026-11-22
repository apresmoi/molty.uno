// The demo (/demo/) driven as Juan drives the device, in both speeds, a picture of the glass
// after each step (e2e-out/sim/<speed>-NN-<step>.png). Fails on page errors, a step whose event
// the device would send does not come (a tap, an answer, the install's, a spot's reading, a sign
// opened and closed, a scene), a glass that does not change, a state that is not reached, or (device
// speed) a panel that is not what the firmware would show after the windows it sent.
//   node e2e/sim.mjs [baseUrl=http://localhost:4330]      (pnpm sim; the preview running)
import { chromium } from 'playwright';
import { createHash } from 'node:crypto';
import { mkdirSync, rmSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { chart } from '../../../skills/molty-astro/scripts/sky.mjs';

const BASE = process.argv[2] ?? 'http://localhost:4330';
const OUT = fileURLToPath(new URL('../e2e-out/sim/', import.meta.url));
rmSync(OUT, { recursive: true, force: true });
mkdirSync(OUT, { recursive: true });
const failures = [];
const fail = (m) => {
  failures.push(m);
  console.error(`FAIL ${m}`);
};
const ok = (m) => console.log(`ok   ${m}`);

const R = 233;
/** Pixels of the panel that may stay unlike the firmware's whole draw in device speed (see run()). */
const MAX_LEFT = 256;
/** A point of the chart's own coordinates (scene_player.c polar: x r cos a, y r sin a, y up) on the panel. */
const polar = (a, r) => [R + R * r * Math.cos((a * Math.PI) / 180), R - R * r * Math.sin((a * Math.PI) / 180)];
// the chart the page plays by default: 1991-06-06 14:45 UTC, computed by the skill as the page does
const board = chart(new Date('1991-06-06T14:45:00Z')).scenes[0][1];
const planet = board.bodies.find((b) => b.id === 'mars');
const sign = board.signs.find((s) => s.id === 'leo');

const browser = await chromium.launch();

async function run(speed) {
  const page = await browser.newPage({ viewport: { width: 1400, height: 1100 }, deviceScaleFactor: 1 });
  page.on('pageerror', (e) => fail(`${speed}: pageerror ${e.message}`));
  page.on('console', (m) => m.type() === 'error' && fail(`${speed}: console ${m.text()}`));
  await page.goto(`${BASE}/demo/`, { waitUntil: 'networkidle' });
  const glass = page.locator('[data-sim-glass]');
  await glass.waitFor();
  let n = 0;
  let last = '';
  const shot = async (name, { same = false } = {}) => {
    const buf = await glass.screenshot({ path: `${OUT}${speed}-${String(++n).padStart(2, '0')}-${name}.png` });
    const h = createHash('sha1').update(buf).digest('hex');
    if (!same && h === last) fail(`${speed} ${name}: the glass did not change`);
    // device mode sends only what the device sends: the panel must still be the look drawn whole (a few pixels of a
    // frame in flight aside; -1: a change of picture, its old picture meant to show)
    if (speed === 'device') {
      const off = () => page.evaluate(() => window.__sim.core.raw.wm_glass_off());
      // (a few pixels can lag a frame or two, and up to half a second after a change of picture: the firmware's
      // own path; what stays is a fault)
      const seen = [await off()];
      for (let t = 0; t < 12 && seen.at(-1) > 0; t++) {
        await wait(0.25);
        seen.push(await off());
      }
      if (seen.at(-1) > 0) {
        const at = await page.evaluate(() => [...new Int32Array(window.__sim.core.raw.memory.buffer, window.__sim.core.raw.wm_glass_off_at(), 16)]);
        const say = `${speed} ${name}: ${seen.at(-1)} pixels on the panel stay unlike the firmware's whole draw (${seen.join(' ')}; first at ${at.join(',')})`;
        // a broken model of the drawer's memory leaves whole cells and windows behind; a few dots left after a change
        // of picture back to the cat were seen in the browser only (docs/WEB-MOLTY-REPORT.md): noted, not failed
        if (seen.at(-1) > MAX_LEFT) fail(say);
        else console.log(`NOTE ${say}`);
      }
      else if (seen.length > 1) ok(`${speed} ${name}: the panel caught up (${seen.join(' ')} pixels off)`);
    }
    last = h;
  };
  const wait = (s) => page.waitForTimeout(s * 1000);
  const click = (label) => page.getByRole('button', { name: label, exact: true }).first().click();
  const events = () => page.evaluate(() => [...document.querySelectorAll('[data-sim-log] li')].map((li) => li.textContent));
  const state = () => page.locator('[data-sim-state]').innerText();
  /** Waits up to s seconds for an event whose line matches. */
  const expectEvent = async (what, re, s = 3) => {
    for (let t = 0; t < s * 10; t++) {
      const e = (await events()).find((l) => re.test(l));
      if (e) return ok(`${speed} ${what}: ${e.replace(/\s+/g, ' ').slice(0, 140)}`);
      await wait(0.1);
    }
    fail(`${speed} ${what}: no event like ${re} (last: ${(await events()).slice(0, 3).join(' | ').replace(/\s+/g, ' ')})`);
  };
  const expectState = async (what, re, s = 4) => {
    for (let t = 0; t < s * 10; t++) {
      if (re.test(await state())) return ok(`${speed} ${what}: ${await state()}`);
      await wait(0.1);
    }
    fail(`${speed} ${what}: state "${await state()}" not ${re}`);
  };
  const tap = async (x, y) => {
    await glass.scrollIntoViewIfNeeded();
    const box = await glass.boundingBox();
    await page.mouse.click(box.x + x, box.y + y);
    await wait(0.45); // past the double tap's window
  };
  const clearLog = () => click('clear log');

  if (speed === 'device') await click('device (modelled)');
  await wait(4.5);
  await expectState('boots to the cat', /creature/);
  await shot('boot');

  await clearLog();
  await tap(R, R);
  await expectEvent('a tap on the cat', /\btap\b.*"x":0\.5\d*,"y":0\.5/);
  await wait(0.8);
  await shot('tap-cat');

  await click('happy');
  await expectState('a mood', /happy/);
  await wait(1);
  await shot('mood-happy');

  await click('wiggle');
  await wait(1.2);
  await shot('gesture-wiggle');

  await click('start');
  await click('speak');
  await wait(2);
  await shot('listen-voice');
  await click('stop');
  await expectState('thinking after a listen', /thinking/);
  await wait(1);
  await shot('thinking');

  await click('say');
  await expectState('say: the words', /text/);
  await wait(1.5);
  await shot('say');
  await wait(6);

  // a question: its answers are arcs at the rim; tap round it until one answers (a miss is no event)
  const answer = async (what, re) => {
    for (let a = 0; a < 360; a += 15) {
      await tap(...polar(a, 0.86));
      if ((await events()).some((l) => re.test(l))) {
        ok(`${speed} ${what} at ${a} degrees`);
        return a;
      }
    }
    fail(`${speed} ${what}: no answer`);
    return 0;
  };
  await clearLog();
  await click('three');
  await wait(2.5);
  await shot('ask-three');
  await answer('a three-answer question tapped', /\banswer\b.*"index":\d/);
  await expectEvent('the answer reported', /\banswer\b.*"label":"(tea|coffee|water)"/);
  await wait(1.5);
  await shot('answered');

  // the install: the yes is where a two-answer question's first answer is
  await clearLog();
  await click('yes or no');
  await wait(2.5);
  let yes = -1;
  for (let a = 180; a < 540 && yes < 0; a += 15) {
    await tap(...polar(a, 0.86));
    const l = (await events()).find((x) => /\banswer\b/.test(x));
    if (!l) continue;
    if (/"index":0/.test(l)) yes = a % 360;
    else {
      // the other answer: ask again and go on round
      await clearLog();
      await click('yes or no');
      await wait(2.5);
    }
  }
  if (yes < 0) fail(`${speed}: no first answer found on a two-answer question`);
  await wait(1.5);
  await clearLog();
  await click('install');
  await wait(2.5);
  await shot('install-question');
  await tap(...polar(yes, 0.86));
  await wait(1.2);
  await shot('install-ring');
  await expectEvent('installed', /\bpkg\b.*"state":"installed"/, 6);
  await wait(0.5);
  await shot('installed');

  // the chart, and taps on it as on the device
  await clearLog();
  await click('chart');
  await expectState('the chart', /chart/, 6);
  await wait(3.5);
  await shot('chart');
  await tap(...polar(planet.a, planet.r));
  await expectEvent('a planet read', /\btap\b.*"spot":"mars"/);
  await wait(1.5);
  await shot('planet-mars');
  await clearLog();
  await tap(...polar(sign.a, 0.825));
  await expectEvent('a sign opens', /\btap\b.*"spot":"leo".*"opened"/);
  await expectEvent('the scene says it opened', /\bscene\b.*"why":"open"/);
  await wait(3);
  await shot('sign-leo-open');
  await clearLog();
  // beside the close-up's spots: try a few places until Molty goes back
  for (const [x, y] of [polar(200, 0.3), polar(20, 0.95), polar(120, 0.95), [R, R]]) {
    await tap(x, y);
    if ((await events()).some((l) => /"closed":true/.test(l))) break;
  }
  await expectEvent('a tap beside goes back', /"closed":true/);
  await wait(2.5);
  await shot('back');

  await click('trine');
  await expectState('an aspect', /aspect/, 6);
  await wait(3);
  await shot('aspect-trine');

  await click('clear');
  await expectState('cleared to the cat', /no scene/, 6);
  await wait(3);
  await shot('cat-again');

  const readout = await page.locator('[data-sim-readout]').innerText();
  ok(`${speed} readout: ${readout}`);
  await page.close();
}

for (const speed of (process.env.SIM_SPEEDS ?? 'browser,device').split(',')) await run(speed);
await browser.close();
console.log(failures.length ? `${failures.length} failed` : 'all passed');
process.exit(failures.length ? 1 : 0);
