// The unseen queue (src/services/molty-screen/slices.ts) serves what the visitor looks at first:
// a canvas in view before one out of view, the nearer of two out of view first, and work for a
// canvas far out of view waits until it comes back. Page frames, the view and the canvases are
// stand-ins here. Run: pnpm test (node --test; Node 22 strips the types).
import { test } from "node:test";
import assert from "node:assert/strict";

const VIEW = 1000;
let frames: Array<() => void> = [];
let wakeNear: (() => void) | null = null;
Object.assign(globalThis, {
  innerHeight: VIEW,
  requestAnimationFrame: (cb: () => void) => frames.push(cb),
  IntersectionObserver: class {
    constructor(cb: () => void) {
      wakeNear = cb;
    }
    observe() {}
    unobserve() {}
  },
});
const { rank, sliced } = await import("../../src/services/molty-screen/slices.ts");

/** Run the page's next frame. */
const frame = () => {
  const now = frames;
  frames = [];
  for (const cb of now) cb();
};
const canvas = (top: number, height = 400) => {
  const box = { top, bottom: top + height };
  return { box, getBoundingClientRect: () => box } as unknown as Element & { box: typeof box };
};
/** A job of `n` steps, each `ms` long, that logs its name each step. */
function* work(name: string, n: number, log: string[], ms = 5) {
  for (let i = 0; i < n; i++) {
    const until = performance.now() + ms;
    while (performance.now() < until);
    log.push(name);
    yield;
  }
}

test("rank: in view first (nearest the middle first), then out of view by distance, far waits", () => {
  const middle = rank({ top: 300, bottom: 700 }, VIEW);
  const edge = rank({ top: 700, bottom: 1100 }, VIEW);
  const below = rank({ top: 1100, bottom: 1500 }, VIEW);
  const further = rank({ top: 1600, bottom: 2000 }, VIEW);
  const above = rank({ top: -700, bottom: -100 }, VIEW);
  assert.ok(middle < edge && edge < 1, "both in view, the middle one first");
  assert.ok(1 < below && below < further, "out of view after, nearer first");
  assert.ok(above < further, "above the view counts as below does");
  assert.equal(rank({ top: 2100, bottom: 2500 }, VIEW), Infinity);
  assert.equal(rank({ top: -1500, bottom: -1100 }, VIEW), Infinity);
});

test("the queue serves the canvas in view before one out of view, and follows the view", async () => {
  const log: string[] = [];
  const out = canvas(1300);
  const seen = canvas(300);
  const far = canvas(3000);
  const ends: string[] = [];
  const jobs = [
    sliced(work("far", 2, log), far).then(() => ends.push("far")),
    sliced(work("out", 3, log), out).then(() => ends.push("out")),
    sliced(work("seen", 3, log), seen).then(() => ends.push("seen")),
  ];
  frame();
  assert.deepEqual(log, ["seen"], "the one in view goes first, though queued last");
  // the visitor scrolls: the other comes into view, the first goes out
  out.box.top = 300;
  out.box.bottom = 700;
  seen.box.top = -600;
  seen.box.bottom = -200;
  frame();
  assert.deepEqual(log, ["seen", "out"]);
  while (frames.length) frame(); // to where only far work is left: the queue sleeps
  assert.deepEqual(log, ["seen", "out", "out", "out", "seen", "seen"]);
  await Promise.resolve();
  assert.deepEqual(ends, ["out", "seen"]);
  // it comes near: the observer wakes the queue and it finishes
  far.box.top = 500;
  far.box.bottom = 900;
  wakeNear!();
  while (frames.length) frame();
  await Promise.all(jobs);
  assert.deepEqual(ends, ["out", "seen", "far"]);
});
