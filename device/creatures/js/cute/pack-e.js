// Cute pack E — pets & wild: Puppy, Fox, Turtle.
// Each one has a different body plan (sitting floppy-eared pup / slender sitter with a
// plume tail / low domed shell with a head that tucks in) and a signature behaviour
// written in its update() hook. Their device stand-ins are in tools/molty-behaviours.mjs.

import { defineCreature } from './kit.js';

const TAU = Math.PI * 2;
const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);
const bump = (q, a, b) => (q < a || q > b ? 0 : Math.sin(Math.PI * (q - a) / (b - a)));
const ease = (cur, target, dt, rate) => cur + (target - cur) * (dt > 0 ? 1 - Math.exp(-dt * rate) : 1);

// Muzzles and masks stop just above the mouth, so the kit's own mouth placement (the head
// ellipsoid) stays visible with no hook — the device runs no JS (see MOLTY-FORMAT.md).

// Recurring timer: returns the 0…1 progress of a `len`-second event every `lo…hi` s, else 0.
function pulse(s, key, t, lo, hi, len) {
  const e = (s[key] ||= { next: t + lo + Math.random() * (hi - lo), t0: -1 });
  if (e.t0 < 0 && t > e.next) e.t0 = t;
  if (e.t0 < 0) return 0;
  const q = (t - e.t0) / len;
  if (q >= 1) { e.t0 = -1; e.next = t + lo + Math.random() * (hi - lo); return 0; }
  return q;
}

// ------------------------------------------------------------------ Puppy
// Sitting pup: a big round head, long floppy brown ears hanging at the sides, a cream
// muzzle with a button nose, a red bandana, a stubby tail.
// Signature: the tail never stops wagging (a blur when happy), the tongue lolls out and
// pants when happy, and it holds a big curious head tilt while it listens.
const PUP_TONGUE = { in: [0, -0.13, 0.2], out: [0, -0.215, 0.27], s0: 0.3 };
defineCreature({
  id: 'cute-puppy', name: 'Puppy', archetype: 'Pet',
  roster: 'Floppy-eared pup — tail never stops wagging, tongue out when happy, big head tilt when it listens',
  palette: { body: '#f2d3a8', accent: '#93603d', belly: '#fff6e8', muzzle: '#fff3e2', nose: '#3b2a26', scarf: '#e8574f', eye: '#241a14', cheek: '#ffa3a3', mouth: '#5a2626', tongue: '#ff7f95' },
  body: { size: [0.22, 0.22, 0.22], at: [0, 0.22, -0.03], seg: [8, 5] },
  head: { size: [0.35, 0.3, 0.3], at: [0, 0.6, 0.04], seg: [10, 7], pitch: 0.45 },
  eyes: { x: 0.38, y: 0.04, r: 0.08 },
  mouth: { type: 'smile', y: -0.6, w: 0.032 },
  cheeks: { x: 0.62, y: -0.26, r: 0.045 },
  motion: { type: 'hop', rate: 1.1, amp: 0.55 },
  limbs: { tail: 1.8, ear: 0.8 },
  sockets: { ear: { pos: [0.3, 0.77, 0.0], rot: [0, 0, 0.3] }, tail: { rot: [-0.75, 0, 0] } },
  parts: [
    // floppy ears: narrow at the root, hanging down the sides of the head
    { name: 'ear', type: 'teardrop', at: 'ear', size: [0.1, 0.2, 0.06], pos: [0.035, -0.15, 0], color: 'accent', seg: [6, 4], pinch: 0.5 },
    { name: 'muzzle', type: 'ellipsoid', at: 'head', size: [0.14, 0.075, 0.08], pos: [0, -0.07, 0.24], color: 'muzzle', seg: [8, 4] },
    { name: 'nose', type: 'ellipsoid', at: 'head', size: [0.05, 0.034, 0.03], pos: [0, -0.03, 0.325], color: 'nose', seg: [6, 3] },
    { name: 'brow', type: 'ellipsoid', at: 'head', size: [0.045, 0.02, 0.02], pos: [0.12, 0.155, 0.255], rot: [0, 0, -0.25], mirror: true, color: 'accent', seg: [4, 3] },
    { name: 'tongue', type: 'ellipsoid', at: 'head', size: [0.04, 0.055, 0.016], pos: PUP_TONGUE.in, rot: [-0.25, 0, 0], scale: PUP_TONGUE.s0, color: 'tongue', seg: [6, 3] },
    { name: 'belly', type: 'ellipsoid', at: 'body', size: [0.14, 0.14, 0.08], pos: [0, 0.2, 0.14], color: 'belly', seg: [6, 4] },
    { name: 'bandana', type: 'teardrop', at: 'body', size: [0.13, 0.1, 0.03], pos: [0, 0.31, 0.165], rot: [-0.2, 0, Math.PI], color: 'scarf', seg: [6, 3], pinch: 0.9 },
    { name: 'spot', type: 'ellipsoid', at: 'body', size: [0.12, 0.08, 0.1], pos: [0.06, 0.33, -0.12], rot: [0.4, 0, 0.3], color: 'accent', seg: [5, 3] },
    { name: 'haunch', type: 'ellipsoid', at: 'body', size: [0.08, 0.07, 0.11], pos: [0.16, 0.08, -0.02], mirror: true, seg: [5, 3] },
    { name: 'paw', type: 'ellipsoid', at: 'foot', size: [0.055, 0.045, 0.07], pos: [0, 0.045, 0.06], color: 'belly', seg: [5, 3] },
    { name: 'tail', type: 'teardrop', at: 'tail', size: [0.04, 0.11, 0.04], pos: [0, 0.09, 0], color: 'accent', seg: [5, 3], pinch: 0.7 },
  ],
  build(ctx) { ctx.s = { tongue: 0 }; },
  update(ctx) {
    const { w, t, dt, s, creature } = ctx;
    // the big puppy head tilt while listening (on top of the kit's little curious tilts)
    creature.headPivot.rotation.z += 0.32 * w.listening;
    // happy: the whole rear wiggles with the wag
    creature.root.rotation.z += 0.045 * w.happy * Math.sin(TAU * 5 * t);
    // tongue: tucked in the muzzle at rest, lolls out and pants when happy
    s.tongue = ease(s.tongue, w.happy, dt, 8);
    const k = s.tongue, pant = 0.008 * w.happy * Math.sin(TAU * 3 * t);
    for (const m of ctx.part('tongue')) {
      m.position.set(0, PUP_TONGUE.in[1] + (PUP_TONGUE.out[1] - PUP_TONGUE.in[1]) * k + pant, PUP_TONGUE.in[2] + (PUP_TONGUE.out[2] - PUP_TONGUE.in[2]) * k);
      m.scale.setScalar(PUP_TONGUE.s0 * (1 + (1 / PUP_TONGUE.s0 - 1) * k));
    }
  },
});

// ------------------------------------------------------------------ Fox
// Slender sitter: tall triangular ears, orange cheek fluff flaring out, a pointed orange
// snout over small cream cheeks, a little chest tuft, and a huge bushy tail plumed up
// beside it with a white tip. Built for the braille fluid: the silhouette carries it, cream
// stays small (big flat cream areas read as noise under auto-exposure) and nothing but the
// eyes and nose is dark (dark cells render as solid feature blocks).
// Signature: the tail swishes in slow sweeps; every few seconds it drops into a
// pounce-ready crouch (head down, ears forward, a bum wiggle) and springs a little hop.
defineCreature({
  id: 'cute-fox', name: 'Fox', archetype: 'Woodland',
  roster: 'Bushy-tailed kit — slow tail swish, drops into a pounce-ready crouch and springs',
  palette: { body: '#ff9e4f', accent: '#fb8c40', belly: '#ffe9d2', sock: '#c2531f', tipDark: '#9c3f17', earIn: '#c95a24', tip: '#fff0e0', nose: '#2e1d19', eye: '#24160f', cheek: '#ff9f8a', mouth: '#4a1f1a', tongue: '#ff7f8f' },
  body: { size: [0.17, 0.22, 0.17], at: [0, 0.22, -0.04], seg: [8, 5] },
  head: { size: [0.33, 0.26, 0.27], at: [0, 0.6, 0.04], seg: [10, 7], pitch: 0.45 },
  eyes: { x: 0.36, y: 0.08, r: 0.072, tilt: 0.14 },
  mouth: { type: 'cat', y: -0.66, w: 0.03 },
  cheeks: { x: 0.62, y: -0.22, r: 0.036 },
  motion: { type: 'waddle', rate: 0.8, amp: 0.4 },
  limbs: { tail: 0.4 },
  sockets: { ear: { pos: [0.17, 0.78, 0.0], rot: [-0.08, 0, -0.32] }, tail: { pos: [0.08, 0.1, -0.18], rot: [-0.3, 0, -1.05] } },
  parts: [
    // big triangular ears with small dark tips
    { name: 'ear', type: 'cone', at: 'ear', r: 0.1, h: 0.28, pos: [0, 0.11, 0], seg: 5 },
    { name: 'earIn', type: 'cone', at: 'ear', r: 0.058, h: 0.17, pos: [0, 0.08, 0.04], color: 'earIn', seg: 5 },
    { name: 'earTip', type: 'cone', at: 'ear', r: 0.036, h: 0.08, pos: [0, 0.226, 0.002], color: 'tipDark', seg: 5 },
    // the fox face: orange cheek fluff flaring out sideways (silhouette, same colour as the head)
    { name: 'ruff', type: 'teardrop', at: 'head', size: [0.09, 0.13, 0.08], pos: [0.27, -0.12, 0.06], rot: [0, 0, -2.0], mirror: true, seg: [5, 3], pinch: 0.85 },
    // the fox mask: small cream cheeks on the lower face, either side of the snout
    { name: 'mask', type: 'ellipsoid', at: 'head', size: [0.1, 0.065, 0.06], pos: [0.12, -0.12, 0.215], rot: [0, 0.5, 0.35], mirror: true, color: 'belly', seg: [6, 3] },
    // a pointed orange snout with a small cream chin under it and a dark nose on the tip
    { name: 'snout', type: 'cone', at: 'head', r: 0.075, h: 0.15, pos: [0, -0.07, 0.29], rot: [Math.PI / 2, 0, 0], seg: 6 },
    { name: 'chin', type: 'ellipsoid', at: 'head', size: [0.07, 0.035, 0.07], pos: [0, -0.125, 0.25], color: 'belly', seg: [6, 3] },
    { name: 'nose', type: 'sphere', at: 'head', size: 0.03, pos: [0, -0.065, 0.36], color: 'nose', seg: [5, 3] },
    // a small cream chest tuft
    { name: 'chest', type: 'teardrop', at: 'body', size: [0.075, 0.1, 0.05], pos: [0, 0.27, 0.13], rot: [0.2, 0, Math.PI], color: 'belly', seg: [6, 3], pinch: 0.7 },
    { name: 'haunch', type: 'ellipsoid', at: 'body', size: [0.085, 0.09, 0.12], pos: [0.12, 0.09, -0.04], mirror: true, seg: [5, 3] },
    { name: 'sock', type: 'ellipsoid', at: 'foot', size: [0.04, 0.07, 0.045], pos: [0, 0.065, 0.04], color: 'sock', seg: [5, 3] },
    // the plume: a huge bushy tail swung up beside the body, with a white tip
    { name: 'tail', type: 'teardrop', at: 'tail', size: [0.16, 0.32, 0.15], pos: [0, 0.28, 0], rot: [0, 0, 0], color: 'accent', seg: [7, 5], pinch: 0.35 },
    { name: 'tailTip', type: 'teardrop', at: 'tail', size: [0.11, 0.14, 0.11], pos: [0, 0.57, 0], color: 'tip', seg: [6, 3], pinch: 0.8 },
  ],
  build(ctx) { ctx.s = { pounce: {} }; },
  update(ctx) {
    const { w, t, creature } = ctx;
    // tail: slow sweeping swish, livelier when happy, curled in when asleep
    const tr = creature.sock.tail[0];
    tr.g.rotation.z = tr.base.z + (0.3 * (1 - 0.8 * w.sleepy) + 0.15 * w.happy) * Math.sin(TAU * 0.55 * t) + 0.3 * w.sleepy;
    // pounce-ready crouch, every 5–9 s while idle: crouch low, wiggle, spring
    const q = pulse(ctx.s.pounce, 'p', t, 5, 9, 1.8) * (w.idle > 0.6 ? 1 : 0);
    const crouch = q <= 0 ? 0 : q < 0.75 ? clamp(q / 0.15, 0, 1) : 1 - clamp((q - 0.75) / 0.08, 0, 1);
    const wiggle = q > 0.3 && q < 0.72 ? 1 : 0, spring = bump(q, 0.74, 1);
    creature.root.position.y += -0.045 * crouch + 0.09 * spring;
    creature.root.position.z += 0.05 * spring;
    creature.root.rotation.x += 0.18 * crouch - 0.1 * spring;
    creature.root.rotation.z += 0.07 * wiggle * Math.sin(TAU * 7 * t);
    creature.headPivot.rotation.x += 0.15 * crouch;
    for (const r of creature.sock.ear) r.g.rotation.x += -0.35 * crouch;
  },
});

// ------------------------------------------------------------------ Turtle
// Low and wide: a domed green shell with hexagonal scutes on a tan rim, four stubby legs,
// and a round head out in front that can tuck back into the shell.
// Signature: the head peeks out of the shell — it pulls in when startled (and half in when
// sleepy), and every so often it ducks in and slowly peeks back out.
const SHELL = { c: [0, 0.27, -0.06], r: [0.38, 0.27, 0.34] };
const scute = (name, elev, azim, r, color) => {
  // a flat hexagon (6-sided cylinder) sitting on the shell dome at (elevation, azimuth)
  const ce = Math.cos(elev), se = Math.sin(elev);
  const p = [SHELL.r[0] * ce * Math.sin(azim), SHELL.r[1] * se, SHELL.r[2] * ce * Math.cos(azim)];
  const n = [p[0] / SHELL.r[0] ** 2, p[1] / SHELL.r[1] ** 2, p[2] / SHELL.r[2] ** 2], nl = Math.hypot(...n);
  const nx = n[0] / nl, ny = n[1] / nl, nz = n[2] / nl;
  return { name, type: 'cylinder', at: 'body', r, h: 0.02, seg: 6, color,
    pos: [SHELL.c[0] + p[0] * 0.985, SHELL.c[1] + p[1] * 0.985, SHELL.c[2] + p[2] * 0.985],
    rot: [Math.atan2(nz, ny), 0, -Math.asin(nx)] };
};
const TURTLE_TUCK = { z: -0.2, y: -0.04, s: -0.6 };
defineCreature({
  id: 'cute-turtle', name: 'Turtle', archetype: 'Pond',
  roster: 'Domed little turtle — ducks into its shell when startled, then slowly peeks back out',
  palette: { body: '#bfe39a', accent: '#4f9a74', scute: '#86c99a', rim: '#ecd39c', belly: '#f6edc4', eye: '#1f2a22', cheek: '#ffa3a8', mouth: '#4a2a2a', tongue: '#ff8a98' },
  body: { size: SHELL.r, at: SHELL.c, color: 'accent', seg: [10, 6] },
  head: { size: [0.21, 0.19, 0.19], at: [0, 0.3, 0.33], seg: [10, 6], pivot: [0, 0.26, 0.16], pitch: 0.4, squash: 0.3 },
  eyes: { x: 0.4, y: 0.06, r: 0.068 },
  mouth: { type: 'smile', y: -0.42, w: 0.034 },
  cheeks: { x: 0.64, y: -0.28, r: 0.034 },
  motion: { type: 'waddle', rate: 0.45, amp: 0.55 },
  limbs: { side: 0 },
  sockets: { foot: [0.27, 0, 0.13], tail: { rot: [-0.9, 0, 0] } },
  parts: [
    scute('scute', Math.PI / 2, 0, 0.12, 'scute'),
    ...[0.6, 1.85, -0.6, -1.85, Math.PI].map((a) => scute('scute', 0.62, a, 0.095, 'scute')),
    { name: 'rim', type: 'ellipsoid', at: 'body', size: [0.41, 0.06, 0.37], pos: [0, 0.07, -0.06], color: 'rim', seg: [10, 3] },
    { name: 'neck', type: 'ellipsoid', at: 'head', size: [0.1, 0.09, 0.14], pos: [0, -0.07, -0.15], seg: [6, 3] },
    { name: 'leg', type: 'ellipsoid', at: 'foot', size: [0.08, 0.065, 0.085], pos: [0, 0.06, 0], seg: [5, 3] },
    { name: 'rearLeg', type: 'ellipsoid', at: 'root', size: [0.075, 0.06, 0.08], pos: [0.26, 0.055, -0.27], mirror: true, seg: [5, 3] },
    { name: 'tail', type: 'teardrop', at: 'tail', size: [0.035, 0.07, 0.03], pos: [0, -0.05, 0], seg: [5, 3], pinch: 0.8 },
  ],
  build(ctx) { ctx.s = { tuck: 0, peek: {} }; },
  update(ctx) {
    const { w, t, dt, s, creature, L } = ctx;
    // the shell is hard: it squashes much less than a soft body
    const bg = creature.bodyG; bg.scale.set(1 + (bg.scale.x - 1) * 0.35, 1 + (bg.scale.y - 1) * 0.35, 1 + (bg.scale.z - 1) * 0.35);
    // peek: every 7–12 s while calm it ducks in fast, then slowly peeks back out
    const q = pulse(s.peek, 'p', t, 7, 12, 2.6) * (w.idle + w.listening > 0.6 ? 1 : 0);
    const peek = q > 0 ? (q < 0.12 ? q / 0.12 : q < 0.4 ? 1 : 1 - (q - 0.4) / 0.6) : 0;
    s.tuck = ease(s.tuck, clamp(Math.max(w.surprised, 0.5 * w.sleepy), 0, 1), dt, 7);
    const k = Math.max(s.tuck, peek);
    const hp = creature.headPivot;
    hp.position.z += TURTLE_TUCK.z * k; hp.position.y += TURTLE_TUCK.y * k;
    hp.scale.multiplyScalar(1 + TURTLE_TUCK.s * k);
    // peeking: the head sways a little, looking round
    hp.rotation.y += 0.35 * Math.sin(TAU * 0.6 * t) * bump(q, 0.45, 0.95);
    // rear legs step with the front ones (diagonal trot, slowly)
    for (const m of ctx.part('rearLeg')) {
      const lift = m.userData.side > 0 ? L.foot[1].lift : L.foot[0].lift;
      m.position.y = m.userData.basePos.y + lift; m.rotation.x = -lift * 5;
    }
  },
});
