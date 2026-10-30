// The device's wall (src/features/device/puck.ts) and its reveal (reveal.ts): every face winds
// outward (polycss culls back faces, so a flipped one silently vanishes) and hides behind the
// bezel face on; every reveal is ≤ 1.2 s, starts away from rest, moves without jumps, swings
// past rest once (a small overshoot, then nothing visible) and ends exactly at rest with the
// Molty released; the glass's glare rests up-left and slides against a turn.
// Run: pnpm test (node --test; Node 22 strips the types).
import { test } from "node:test";
import assert from "node:assert/strict";
import { PUCK, normalOf, puckPolygons } from "../../src/features/device/puck.ts";
import { REST, REVEAL_SECONDS, glareOf, revealFrame, spring, type RevealKind } from "../../src/features/device/reveal.ts";

const COLORS = { wall: "#333333", back: "#444444" };
const KINDS = Object.keys(REVEAL_SECONDS) as RevealKind[];

test("every face of the wall winds outward; the back faces away", () => {
  for (const p of puckPolygons(COLORS)) {
    const n = normalOf(p);
    if (p.color === COLORS.back) assert.ok(n[0] < -0.99, "the back faces away (-X)");
    else {
      const c = p.vertices.reduce((s, v) => [s[0] + v[0], s[1] + v[1], s[2] + v[2]], [0, 0, 0]).map((x) => x / 4);
      assert.ok((n[1] * c[1]! + n[2] * c[2]!) / Math.hypot(c[1]!, c[2]!) > 0.99, "the wall faces out");
    }
  }
});

test("the wall is rectangles, hidden behind the bezel face on, the back plate only when asked", () => {
  const lean = puckPolygons(COLORS, { back: false });
  assert.equal(lean.length, PUCK.segments);
  assert.equal(puckPolygons(COLORS).length, PUCK.segments * 2);
  for (const p of lean) {
    const [a, b, c, d] = p.vertices as number[][];
    const len = (u: number[], v: number[]) => Math.hypot(u[0]! - v[0]!, u[1]! - v[1]!, u[2]! - v[2]!);
    assert.ok(Math.abs(len(a!, b!) - len(c!, d!)) < 1e-9 && Math.abs(len(b!, c!) - len(d!, a!)) < 1e-9, "a rectangle");
    for (const v of p.vertices) {
      assert.ok(Math.hypot(v[1], v[2]) < PUCK.bezel - 0.01, "inside the bezel's outline");
      assert.ok(v[0] <= PUCK.front, "behind the face");
    }
  }
});

test("the spring is the closed form of a damped oscillator", () => {
  // x'' = -ω²x - 2ζωx', stepped finely, against the closed form
  const w = 8, z = 0.58;
  let x = 40,
    v = -100;
  const dt = 1e-5;
  for (let t = 0; t < 0.6; t += dt) {
    const a = -w * w * x - 2 * z * w * v;
    v += a * dt;
    x += v * dt;
  }
  assert.ok(Math.abs(x - spring(0.6, 40, -100, w, z)) < 0.01);
});

/** The main motion of a kind (the angle that carries it). */
const main = (k: RevealKind, u: number) => {
  const f = revealFrame(k, u);
  return k === "rise" ? f.pitch : f.yaw;
};

for (const kind of KINDS)
  test(`${kind}: ≤ 1.2 s, starts away, no jumps, one small overshoot, ends at rest released`, () => {
    assert.ok(REVEAL_SECONDS[kind] <= 1.2);
    const a = revealFrame(kind, 0);
    assert.ok(Math.abs(a.yaw) + Math.abs(a.pitch) + Math.abs(a.lid) > 10, "it starts turned away");
    assert.deepEqual(revealFrame(kind, 1), REST);
    const end = revealFrame(kind, 0.995);
    assert.ok(Math.abs(end.yaw) < 0.3 && Math.abs(end.pitch) < 0.3 && Math.abs(end.lid) < 0.3 && end.cover < 0.01);
    assert.equal(end.hold, false);
    const step = 1 / (120 * REVEAL_SECONDS[kind]); // one 120 Hz frame
    let prev = a;
    const start = Math.sign(main(kind, 0));
    let crossings = 0,
      sign = start,
      past = 0,
      second = 0;
    for (let u = step; u < 1; u += step) {
      const f = revealFrame(kind, u);
      const limit = kind === "coin" ? 10 : 5;
      assert.ok(Math.abs(f.yaw - prev.yaw) < limit && Math.abs(f.pitch - prev.pitch) < limit, `${kind} jumps at ${u}`);
      assert.ok(Math.abs(f.lid - prev.lid) < 8, `${kind} lid jumps at ${u}`);
      prev = f;
      const m = main(kind, u);
      if (Math.sign(m) !== sign && Math.abs(m) > 0.05) {
        crossings++;
        sign = Math.sign(m);
      }
      if (crossings === 1) past = Math.max(past, Math.abs(m));
      if (crossings >= 2) second = Math.max(second, Math.abs(m));
    }
    assert.ok(crossings >= 1, `${kind} swings past rest`);
    assert.ok(past < 0.16 * Math.abs(main(kind, 0)) + 2, `${kind} overshoot ${past}`);
    assert.ok(second < 1.2, `${kind} swings back visibly a second time (${second})`);
  });

test("the lid falls shut and bounces once", () => {
  let hits = 0,
    prev = revealFrame("lid", 0).lid;
  for (let u = 0; u < 1; u += 0.001) {
    const l = revealFrame("lid", u).lid;
    if (prev < 0 && l === 0) hits++;
    prev = l;
  }
  assert.ok(hits >= 1 && hits <= 2, `hits ${hits}`);
});

test("the glare rests up-left and slides against the turn", () => {
  const rest = glareOf(0, 0);
  assert.equal(rest.dx, 0);
  assert.equal(rest.dy, 0);
  assert.ok(glareOf(20, 0).dx > 0, "the face turning left slides it right");
  assert.ok(glareOf(-20, 0).dx < 0);
  assert.ok(glareOf(0, -20).dy < 0, "the face tipped down slides it up");
  assert.ok(glareOf(50, 0).fade < 0.5, "turned far, it has slid off the glass");
});
