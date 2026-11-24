// The landing's opening, in order (src/features/home/opening.ts, src/services/molty-screen/forming.ts):
// the title's window is rough dust first, then its word forms as calm spreads from the middle to
// its ends; the title waits for the hero's screen to be prepared (or the cap); the hero's device and
// its cat wait for the title to start forming, then come in step.
// Run: pnpm test (node --test; Node 22 strips the types).
import { mock, test } from "node:test";
import assert from "node:assert/strict";
import { FORMING_S, formingWater } from "../../src/services/molty-screen/forming.ts";
import {
  CAT_AFTER_S,
  OPENING_CAP_MS,
  afterTitle,
  catSetsOff,
  heroPrepared,
  heroReady,
  heroStandsIn,
  titleForming,
} from "../../src/features/home/opening.ts";

// a page with the hero's device
(globalThis as { document?: unknown }).document = { querySelector: () => ({}) };
const flush = () => new Promise((r) => setImmediate(r));

/** The roughest the water is over the word's band, `x` from `from` to `to` (glass units). */
const roughest = (from: number, to: number, t: number) => {
  let most = 0;
  for (let x = from; x <= to; x += 0.01)
    for (let y = -0.1; y <= 0.1; y += 0.01) most = Math.max(most, Math.abs(formingWater(x, y, t)));
  return most;
};

test("the title is dust first, then its word forms from the middle to its ends", () => {
  assert.ok(roughest(-0.9, 0.9, 0) > 1, "rough all over at first");
  assert.ok(roughest(-0.9, -0.7, 0) > 1 && roughest(0.7, 0.9, 0) > 1, "its ends rough at first");
  const mid = FORMING_S / 2;
  assert.ok(roughest(-0.1, 0.1, mid) < 1e-6, "calm at its middle halfway");
  assert.ok(roughest(0.75, 0.9, mid) > 1, "its ends still rough halfway");
  assert.ok(roughest(-0.9, 0.9, FORMING_S - 0.3) < 1e-6, "the word calm end to end before the window eases flat");
});

let formedAt = 0;

test("the title never starts before the hero's screen is prepared", async () => {
  let ready = false;
  void heroReady().then(() => (ready = true));
  await new Promise((r) => setTimeout(r, 200));
  assert.equal(ready, false, "it waits while the hero prepares");
  heroPrepared();
  await flush();
  assert.equal(ready, true, "it may start once the hero is prepared");
});

test("the hero's device and cat wait for the title, then come in step", async () => {
  const start = performance.now();
  const at = (p: Promise<unknown>) => p.then(() => performance.now() - start);
  const cat = at(afterTitle(0));
  const device = at(afterTitle(0.3));
  await new Promise((r) => setTimeout(r, 200));
  formedAt = performance.now();
  titleForming();
  const [c, d] = await Promise.all([cat, device]);
  assert.ok(c >= 190 && c < 400, `the cat waits for the title, then walks in at once (${c.toFixed(0)} ms)`);
  assert.ok(d - c >= 280 && d - c < 450, `the device turns in just after it (${(d - c).toFixed(0)} ms later)`);
});

test("the hero's cat sets off a second after the title, and touches the glass no sooner than its device stands", async () => {
  // its device's turn: it stands this long from now
  heroStandsIn(1150);
  const stands = performance.now() + 1150;
  // the least page seconds from setting off to the glass at the cat's own pace (Entrance.glassS):
  // none, a phone's window, 1440, 1920, 2560 and a very wide, short one (scripts/tests/
  // entrance.test.ts: the cat takes at least that long on every width)
  const offs = await Promise.all(
    [0, 0.24, 0.61, 1.37, 2.5, 5.96].map((glassS) => catSetsOff(glassS).then(() => [glassS, performance.now()] as const)),
  );
  for (const [glassS, at] of offs) {
    assert.ok(at >= formedAt + CAT_AFTER_S * 1000 - 2, `${glassS} s out: it sets off a second after the title`);
    assert.ok(at + glassS * 1000 >= stands - 2, `${glassS} s out: at the glass ${(stands - at - glassS * 1000).toFixed(0)} ms before it stands`);
  }
});

test("at the cap the opening starts whatever is ready, in its order", async () => {
  mock.timers.enable({ apis: ["setTimeout"] });
  try {
    // a page of its own: the hero is never prepared, the title never forms
    const fresh = "../../src/features/home/opening.ts?cap";
    const opening = (await import(fresh)) as typeof import("../../src/features/home/opening.ts");
    const done = { title: false, device: false };
    void opening.heroReady().then(() => (done.title = true));
    void opening.afterTitle(0.3).then(() => (done.device = true));
    mock.timers.tick(OPENING_CAP_MS - performance.now() - 50);
    await flush();
    assert.deepEqual(done, { title: false, device: false }, "nothing before the cap");
    mock.timers.tick(100);
    await flush();
    assert.deepEqual(done, { title: true, device: false }, "the title at the cap, the device not yet");
    mock.timers.tick(300);
    await flush();
    assert.equal(done.device, true, "the device 0.3 s after the cap");
  } finally {
    mock.timers.reset();
  }
});
