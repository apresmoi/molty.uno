#!/usr/bin/env node
// The duck as a creature package: a round body on two big paddles. It rolls its weight from foot to foot, bobs and cocks its head
// to bring one eye round, flicks, flaps and folds its wings, wags its tail a beat late. Every gesture is reseeded each play.
// Its own channels: duck.wl/wr wing spread, wx wings swept back, bill, tuck (head under the wing), puff, neck (head up or thrust),
// fl/fr foot lift, kl/kr foot kick, zl/zr foot reach, feet (paddles spread).
import { ch, sin, cos, nz, env, bump, ss, ssd, ex, lin, lt, gt, clamp, pos, neg, snap, sgn, rnd, E, P, eyes, mouth, merge, LR, alive, living, blinkBurst, writePackage } from '../../tools/creatures/kit.mjs';
import { ACTIONS } from '../../device/creatures/js/actions.js';
const ID = 'duck', a = 'a', T = '@t';
const mul = (terms, g, ...f) => terms.map(([k, ...x]) => [k * g, ...x, ...f]); // scale a term list and multiply in more factors
const sides = (s, l, r) => [[l, lt(s, 0.5)], [r, gt(s, 0.5)]]; // per-play side: l when the draw says left, r when right

const body = merge({ bones: [{ id: 'torso', pos: [0, 0.27, -0.05] }, { id: 'head', parent: 'torso', pos: [0, 0.36, 0.11] }, { id: 'jaw', parent: 'head', pos: [0, -0.115, 0.2] }, { id: 'tuft', parent: 'head', pos: [0.01, 0.29, 0] }, { id: 'wingL', parent: 'torso', pos: [0.27, 0.14, 0] }, { id: 'wingR', parent: 'torso', pos: [-0.27, 0.14, 0] }, { id: 'tail', parent: 'torso', pos: [0, 0.06, -0.3] }, { id: 'tail2', parent: 'tail', pos: [0, 0.08, -0.07] }, { id: 'footL', pos: [0.13, 0.03, 0.02] }, { id: 'footR', pos: [-0.13, 0.03, 0.02] }],
  parts: [E('torso', [0.32, 0.25, 0.36]), E('head', [0.32, 0.3, 0.3]), E('head', [0.13, 0.04, 0.11], [0, -0.085, 0.28], 'accent'), E('jaw', [0.095, 0.026, 0.08], [0, -0.015, 0.05], 'low'), P(E('head', [0.045, 0.03, 0.01], [0.2, -0.08, 0.235], 'cheek')), P(E('head', [0.045, 0.03, 0.01], [-0.2, -0.08, 0.235], 'cheek')), E('tuft', [0.03, 0.07, 0.03], [0, 0.04, 0], 'body', [0, 0, -0.5]), ...LR((d) => [E(`wing${d > 0 ? 'L' : 'R'}`, [0.05, 0.14, 0.12], [d * 0.04, -0.09, 0], 'wing')]), E('tail', [0.06, 0.11, 0.05], [0, 0.02, -0.04], 'body', [-0.9, 0, 0]), E('tail2', [0.03, 0.06, 0.03], [0, 0.02, 0], 'body', [-0.3, 0, 0]), E('footL', [0.08, 0.026, 0.11], [0, 0, 0.03], 'accent'), E('footR', [0.08, 0.026, 0.11], [0, 0, 0.03], 'accent')] },
  eyes('head', 'glossy', 0.13, 0.03, 0.26, 0.9), mouth(-0.105, 0.3));
const palette = { eye: '#0d0a12', shine: '#ffffff', maw: '#160810', body: '#ffdf5a', accent: '#ff9a3c', wing: '#ffc63d', low: '#f0832a', cheek: '#ff9fb4' };

// the waddle: each foot plants ahead and is pushed back under it; the body rolls over whichever foot holds it, the head bobs
const Q = '@walk.q', K = '@walk.k', s = sin(Q, 1, -0.35), side = lin('@walk.along', 1, -0.5), roll = 0.2;
const foot = (o) => ({ sw: [[-0.064, K, ['step', Q, 1, -o, 0.58]]], up: [[0.07, K, ['bumpr', Q, 1, -o, 0.58, 1]]] });
const L = foot(0), R = foot(0.5);
const walk = { hz: 2, stride: 0.22, pace: 0.45, ch: [
  ch('rot.z', [[-roll, K, s, side]]), ch('pos.x', [[0.025, K, s, side]]), ch('rot.y', [[0.1, K, sin(Q, 1, 0.4), lin('@walk.along', 1, -1)]]), ch('pos.y', [[0.024, K, ['abs:sin', Q, 1, -0.25]], [-0.0144, K]]),
  ch('head.z', [[0.75 * roll, K, s, side]]), ch('head.x', [[0.07, K, sin(Q, 2, 0.5)]]), ch('tail.x', [[0.15, K]]), ch('tail.y', [[0.45, K, sin(Q, 1, -1.6)]]),
  ch('stride.0', L.up), ch('stride.1', R.up), ch('swing.0', L.sw), ch('swing.1', R.sw),
  ch('duck.neck', [[0.3, K, sin(Q, 2, -0.9)]]), ch('duck.wl', [[0.08, K], [0.1, K, ['neg:sin', Q, 1, -0.35]]]), ch('duck.wr', [[0.08, K], [0.1, K, ['pos:sin', Q, 1, -0.35]]]),
  ch('duck.kl', mul(L.up, -2)), ch('duck.kr', mul(R.up, -2))] };

// gestures: t runs 0..dur; ranges [lo, hi, slot] are drawn once per play
const D = (n) => ACTIONS[n];
const nodJab = (n) => [['cyc', a, n / 0.9, -0.08 * n / 0.9, 0.3], gt(a, 0.08), lt(a, 0.98), n === 3 ? lt('~1', 0.5) : gt('~1', 0.5)];
const nodBill = (n) => [['bumpr', a, n / 0.9, -0.08 * n / 0.9, 0.2, 0.45], gt(a, 0.08), lt(a, 0.98), n === 3 ? lt('~1', 0.5) : gt('~1', 0.5)];
const nod = (g, ...f) => [2, 3].map((n) => [g, ...nodJab(n), ...f]);
const shk = [ss(a, 0.15, 0.05), ex(a, 2.4, 0.15), ssd(a, D('shake_head') - 0.25, 0.25)], shF = [6.5, 8, 1], shS = sin(a, shF, [-2 * Math.PI * 0.15 * 6.5, -2 * Math.PI * 0.15 * 8, 1]);
const bF = ['bump', a, 0.22, [0.58, 0.64, 2]], bC = bump(a, 0, 0.26), bL = ['bump', a, [0.54, 0.6, 2], [0.88, 0.94, 2]], bW = ['bump', a, 0.18, [0.68, 0.74, 2]];
const pLow = [ss(a, 0, 0.35), ssd(a, 0.7, 0.25)], pTall = [ss(a, 0.75, 0.25), ssd(a, D('peek') - 0.45, 0.45)];
const pA = [[1, snap(a, 1.0)], [-2, ['ss', a, [1.45, 1.65, 1], 0.09], lt('~2', 0.7)], [-1, ['ss', a, [1.45, 1.65, 1], 0.09], gt('~2', 0.7)]];
const glance = (s, d) => { const k = [[0.55, snap(a, 0.08)], [0.45, ['ss', a, [0.32, 0.44, 1], 0.09]]], back = ['ssd', a, [d - 0.42, d - 0.52, 2], 0.14];
  return { ch: [ch('head.y', mul(k, 0.55 * s, back)), ch('head.z', mul(k, 0.22 * s, back)), ch('look.x', mul(k, 0.7 * s, back)), ch('duck.neck', mul(k, 0.1, back))] }; };
const wK = [ex(a, 2), ss(a, 0, 0.1), ssd(a, D('wobble') - 0.3, 0.3)], wS = sin(a, [1.9, 2.5, 1]), cu = bump(a, 0.12, 0.5);
const one = env(a, 1.05, 0.3, 0.25), both = [ss(a, 0.95, 0.3), ssd(a, D('stretch') - 0.35, 0.35)], sh = [bump(a, D('stretch') - 0.3, D('stretch')), sin(a, 12)], shA = [bump(a, D('stretch') - 0.3, D('stretch')), ['abs:sin', a, 12]];
const ye = env(a, D('yawn'), 0.6, 0.6), gape = [ss(a, 0.45, 0.45), ssd(a, 1.75, 0.4)], pre = bump(a, 0, 0.5), ysh = [bump(a, 2.05, 2.5), sin(a, 13)], yshA = [bump(a, 2.05, 2.5), ['abs:sin', a, 13]], yA = rnd(1, 0.85, 1), quiver = (g) => [[g, ...gape, yA], [0.06 * g, ...gape, yA, sin(a, 7)]];
const pe = env(a, D('paddle'), 0.3, 0.5), stroke = (o) => ['cyc', a, 1 / 1.25, -o / 1.25, 0.25], pR = stroke(0.47), pL = stroke(1.095);
const gestures = {
  // a tail shimmy: the rear end waggles fast while the body rocks slower and the head stays level
  wiggle: { ch: (() => { const e = env(a, D('wiggle'), 0.15, 0.35), f = [2.4, 3.2, 1]; return [ch('rot.z', [[0.09, e, sin(a, f)]]), ch('rot.y', [[0.06, e, sin(a, f, 1)]]), ch('head.z', [[-0.063, e, sin(a, f)]]), ch('pos.x', [[0.02, e, sin(a, f)]]), ch('tail.y', [[0.65, e, sin(a, [7, 9, 2])]]), ch('squash', [[0.03, e, sin(a, [4.8, 6.4, 1])]]), ch('duck.wl', [[0.18, e]]), ch('duck.wr', [[0.18, e]]), ch('duck.puff', [[0.25, e]])]; })() },
  // a duck nods by bobbing: quick jabs forward and down, drawn back slower; the bill clicks at the bottom (two or three jabs)
  nod: { ch: [ch('head.x', nod(0.3, rnd(2, 0.8, 1.1))), ch('spine.x', nod(0.05)), ch('look.y', nod(-0.15)), ch('duck.neck', nod(-0.35, rnd(2, 0.8, 1.1))), ch('duck.bill', [2, 3].map((n) => [0.25, ...nodBill(n)])), ch('tail.y', nod(0.2, sin(a, 1.5)))] },
  // shakes water off its head: draws it in, a fast decaying shake, the bill rattles, a ruffle at the end
  shake_head: { ch: [ch('head.y', [[0.5, ...shk, shS]]), ch('head.z', [[0.18, ...shk, sin(a, shF, [-2 * Math.PI * 0.15 * 6.5 + 1.2, -2 * Math.PI * 0.15 * 8 + 1.2, 1])]]), ch('head.x', [[0.1, bump(a, 0, 0.22)]]), ch('look.x', [[0.3, ...shk, shS]]), ch('rot.z', [[0.04, ...shk, shS]]),
    ch('duck.neck', [[-0.3, bump(a, 0, 0.22)]]), ch('duck.bill', [[0.35, ...shk, ['abs:sin', a, shF, [-2 * Math.PI * 0.15 * 6.5, -2 * Math.PI * 0.15 * 8, 1]]]]), ch('duck.wl', [[0.25, bump(a, 0.85, 1.2)]]), ch('duck.wr', [[0.25, bump(a, 0.85, 1.2)]]), ch('duck.puff', [[0.4, bump(a, 0.85, 1.2)]]), ch('squash', [[-0.04, bump(a, 0, 0.22)]])] },
  // a little hop: crouch with the wings drawn back, spring up with them thrown open, feet dangling, land soft
  bounce: { ch: [ch('pos.y', [[[0.12, 0.18, 1], bF], [-0.03, bC], [-0.02, bL]]), ch('squash', [[-0.15, bC], [0.08, bF], [-0.13, bL]]), ch('rot.x', [[0.08, bC], [-0.06, bF]]), ch('head.x', [[0.12, bC], [-0.1, bF], [0.12, bL]]), ch('tail.y', [[0.4, sin(a, 6), bL]]), ch('tail.x', [[0.3, bF]]),
    ch('duck.wl', [[0.91, bW], [0.39, bW, cos(a, 5)]]), ch('duck.wr', [[0.91, bW], [0.39, bW, sin(a, 5, Math.PI / 2 + 0.4)]]), ch('duck.wx', [[0.4, bC]]), ch('duck.kl', [[0.5, bF]]), ch('duck.kr', [[0.45, bF]]), ch('duck.neck', [[-0.3, bC], [0.2, bF]])] },
  // ducks down low behind its own chest, then stretches up tall and cranes about, one eye then the other
  peek: { ch: [ch('pos.y', [[-0.08, ...pLow], [0.03, ...pTall]]), ch('squash', [[-0.16, ...pLow], [0.06, ...pTall]]), ch('head.y', mul(pA, 0.45, sgn(9), ...pTall)), ch('head.z', mul(pA, 0.3, sgn(9), ...pTall)), ch('head.x', [[0.1, ...pLow]]),
    ch('look.x', mul(pA, 0.6, sgn(9), ...pTall)), ch('look.y', [[0.2, ...pTall]]), ch('duck.neck', [[-0.5, ...pLow], [0.7, ...pTall]]), ch('duck.puff', [[0.2, ...pLow]])] },
  // looks with one eye: the head snaps round in two jerks and tips so that eye faces you, holds dead still, snaps back
  glance_left: glance(-1, D('glance_left')), glance_right: glance(1, D('glance_right')),
  blink: { ch: [ch('lid', [[1 / 0.07, clamp(a, 0, 0.07)], [-1 / 0.17, clamp(a, 0.13, 0.3)], [0.13 / 0.17]], { op: 'max' }), ch('head.x', [[-0.03, bump(a, 0, 0.25)]])] },
  // loses its footing: tips over, the wings shoot out, the other foot steps to catch it, and it rocks still
  wobble: { ch: [ch('rot.z', [[0.2, sgn(9), ...wK, wS]]), ch('head.z', [[-0.16, sgn(9), ...wK, wS]]), ch('pos.x', [[-0.04, sgn(9), ...wK, wS]]), ch('tail.y', [[-0.3, sgn(9), ...wK, wS]]),
    ch('duck.wl', mul(sides('~9', 1.3, 0.6), 0.9, ...wK)), ch('duck.wr', mul(sides('~9', 0.6, 1.3), 0.9, ...wK)), ch('duck.fl', [[0.06, cu, lt('~9', 0.5)]]), ch('duck.fr', [[0.06, cu, gt('~9', 0.5)]]), ch('duck.zl', [[0.05, cu, lt('~9', 0.5)]]), ch('duck.zr', [[0.05, cu, gt('~9', 0.5)]])] },
  // the wing-and-leg stretch: one wing and the same leg swept out behind together, then both wings arched up over the back
  stretch: { ch: [ch('rot.z', [[-0.08, sgn(9), one]]), ch('rot.x', [[-0.05, ...both]]), ch('head.x', [[-0.15, ...both]]), ch('head.z', [[0.1, sgn(9), one]]), ch('squash', [[0.07, ...both], [-0.03, one]]), ch('lid', [[0.4, one], [0.4, ...both]], { op: 'max' }),
    ch('duck.wl', [...mul(sides('~9', 0.05, 0.7), 1, one), [1.9, ...both], [0.15, ...sh]]), ch('duck.wr', [...mul(sides('~9', 0.7, 0.05), 1, one), [1.9, ...both], [0.15, ...sh]]), ch('duck.wx', [[0.9, one], [0.4, ...both]]),
    ch('duck.fl', [[0.07, one, gt('~9', 0.5)]]), ch('duck.fr', [[0.07, one, lt('~9', 0.5)]]), ch('duck.kl', [[-0.9, one, gt('~9', 0.5)]]), ch('duck.kr', [[-0.9, one, lt('~9', 0.5)]]), ch('duck.zl', [[-0.09, one, gt('~9', 0.5)]]), ch('duck.zr', [[-0.09, one, lt('~9', 0.5)]]), ch('duck.neck', [[0.4, ...both]]), ch('duck.puff', [[0.3, ...shA]])] },
  // looks up the way birds do: tips the head right over so one eye faces the sky, and scans in little snaps
  look_up: { ch: (() => { const e = env(a, D('look_up'), 0.2, 0.35); return [ch('head.z', [[0.55, sgn(9), e]]), ch('head.x', [[-0.22, e], [0.12, ['steps', a, [0.35, 0.5, 1], [0, 50, 2]]], [-0.06]]), ch('head.y', [[0.15, sgn(9), e]]), ch('look.y', [[0.7, e]]), ch('look.x', [[0.3, sgn(9), e]]), ch('spine.x', [[-0.05, e]]), ch('duck.neck', [[0.45, e]])]; })() },
  // a big yawn: the head sinks, then tips back, the bill gapes wide and quivers, the wings ease out; a little shiver after
  yawn: { ch: [ch('head.x', [[-0.32, ye, ...gape], [0.12, pre]]), ch('head.z', [[0.08, ...gape, sgn(9)]]), ch('spine.x', [[-0.06, ye]]), ch('squash', [[0.08, ...gape], [-0.04, pre]]), ch('lid', [[0.85, ...gape]], { op: 'max' }), ch('mouth', quiver(0.9), { op: 'max' }), ch('rot.z', [[0.03, ...ysh]]),
    ch('duck.bill', quiver(1)), ch('duck.neck', [[0.4, ...gape], [-0.2, pre]]), ch('duck.wl', [[0.35, ...gape], [0.12, ...yshA]]), ch('duck.wr', [[0.35, ...gape], [0.12, ...yshA]]), ch('duck.wx', [[0.3, ...gape]]), ch('duck.puff', [[0.35, ...yshA]])] },
  // paddles: sits low as if afloat and kicks its feet in turn, a power stroke back and a slow recovery; it yaws with each stroke
  paddle: { ch: [ch('pos.y', [[-0.05, pe], [0.012, sin(a, 0.7)]]), ch('rot.y', [[0.07, pe, pL], [-0.07, pe, pR]]), ch('rot.z', [[0.04, pe, pR], [-0.04, pe, pL], [0.02, nz(a, 0.8, 3)]]), ch('rot.x', [[0.04, pe]]), ch('head.x', [[0.08, pe]]), ch('head.z', [[-0.03, pe, pR], [0.03, pe, pL]]), ch('look.y', [[-0.3, pe]]), ch('tail.y', [[0.3, pe, pL], [-0.3, pe, pR]]),
    ch('duck.fl', [[0.09, pe, pL]]), ch('duck.fr', [[0.09, pe, pR]]), ch('duck.kl', [[-1.6, pe, pL]]), ch('duck.kr', [[-1.6, pe, pR]]), ch('duck.zl', [[-0.08, pe, pL]]), ch('duck.zr', [[-0.08, pe, pR]]), ch('duck.feet', [[0.5, pe]]), ch('duck.puff', [[0.15, pe]])] },
};

// moods: a is the time since the mood came, and draws are made once each time it comes
const ls = [[1, sgn(1)], [-2, sgn(1), lt('~2', 0.5), snap(a, 1.7, 0.14)]], lj = ['steps', a, 0.9, [0, 500, 3]];
const side2 = [[2, gt('@_side', 0.5)], [-1]], pop = ss(a, 0, 0.08), calm = ss(a, 0.5, 1.2), sk = [ss(a, 0.35, 1.2)], tuck = [[1, ...sk], [-0.3, ...sk, ['bump', a, [2, 2.4, 1], [2.7, 3.1, 1]]]], leg = ss(a, 1, 0.6), br = sin(T, 0.28);
const moods = {
  // listening: cocks its head so one eye faces you and goes still, re-aims in tiny snaps, now and then swaps eyes
  listening: { ch: [ch('head.z', [...mul(ls, 0.3), [0.1, lj], [-0.05]]), ch('head.y', mul(ls, 0.25)), ch('head.x', [[-0.04], [0.05, lj], [-0.025]]), ch('look.x', mul(ls, 0.3)), ch('duck.neck', [[0.3]]), ch('duck.wl', [[-0.04]]), ch('duck.wr', [[-0.04]])] },
  // thinking: one eye up to the sky, then the other, in snaps; the bill nibbles at nothing; a foot shifts
  thinking: { ch: [ch('_side', [[1, ['steps', a, [1.1, 1.5, 1], [0, 350, 2]]]]), ch('head.z', mul(side2, 0.3)), ch('head.x', [[-0.12]]), ch('look.x', mul(side2, 0.3)), ch('duck.bill', [[0.25, ['pos:nz', a, 1.5, [0, 300, 4]], ['abs:sin', a, 5]]]),
    ch('duck.fl', [[0.03, ['bumpr', a, 1 / 2.3, [0, 2 / 2.3, 5], 0, 0.35 / 2.3]]]), ch('duck.kl', [[0.3, ['bumpr', a, 1 / 2.3, [0, 2 / 2.3, 5], 0, 0.35 / 2.3]]]), ch('duck.neck', [[0.15]])] },
  // speaking: the wings lift a little on what it means (the bill and head bobs ride the voice, in post)
  speaking: { ch: [ch('duck.wl', [[0.25, ['pos:nz', T, 1.3, 5]]]), ch('duck.wr', [[0.25, ['pos:nz', T, 1.2, 9]]]), ch('duck.neck', [[0.1]])] },
  // happy: wingbeats in bursts (down fast, up slower, never on a beat), a fast tail waggle, the head bobbing, quacks
  happy: { ch: [ch('_g', [[2, nz(T, 1.4, 21)], [0.7]], { cl: [0, 1] }), ch('_fp', [[5.5], [1.375, nz(T, 0.9, 41)]], { int: 1 }), ch('_f', [[1, ['flap', '@_fp']]]), ch('_wp', [[6.5], [1.5, nz(T, 1, 33)]], { int: 1 }), ch('_q', [[3, nz(T, 2.2, 17)], [-0.8]], { cl: [0, 1] }),
    ch('duck.wl', [[0.35], [1.4, '@_g', '@_f']]), ch('duck.wr', [[0.35], [1.4, '@_g', ['pow', '@_f', 1.15]]]), ch('duck.wx', [[-0.15, '@_g']]), ch('duck.bill', [[0.55, '@_q', ['abs:sin', T, 4]]]), ch('duck.neck', [[0.12, sin(T, 2.3)]]),
    ch('tail.y', [[0.35, sin('@_wp', 1)], [0.2, nz(T, 1, 8), sin('@_wp', 1)]]), ch('tail.x', [[0.25]]), ch('head.z', [[0.08, nz(T, 1.7, 4)]])] },
  // sleepy: settles down, turns its head round and tucks the bill into its back, draws one foot up, fluffs out; slow breaths
  sleepy: { ch: [ch('pos.y', [[-0.03, ss(a, 0, 0.6)]]), ch('rot.z', [[-0.05, leg]]), ch('squash', [[-0.04, ss(a, 0, 0.6)], [0.025, br]]), ch('duck.tuck', tuck), ch('duck.puff', [...mul(tuck, 0.35), [0.1, br]]), ch('duck.fr', [[0.1, leg]]), ch('duck.kr', [[-0.7, leg]]), ch('duck.wl', [[-0.05]]), ch('duck.wr', [[-0.05]])] },
  // surprised: puffs right up, throws the wings out, stretches tall and quacks, a startled step back; it settles slowly
  surprised: { ch: [ch('pos.z', [[-0.04, ss(a, 0, 0.3)]]), ch('duck.puff', [[1.1, pop], [-0.5, pop, calm]]), ch('duck.wl', [[1.5, pop], [-0.9, pop, calm], [0.3, sin(a, 9), ex(a, 3)]]), ch('duck.wr', [[1.4, pop], [-0.85, pop, calm], [-0.3, sin(a, 9), ex(a, 3)]]), ch('duck.neck', [[0.6, pop], [-0.25, pop, calm]]),
    ch('duck.bill', [[0.6, bump(a, 0.05, 0.6)]]), ch('duck.fl', [[0.04, bump(a, 0.05, 0.3)]]), ch('duck.fr', [[0.04, bump(a, 0.1, 0.35)]]), ch('tail.x', [[0.5, pop], [-0.3, pop, calm]]), ch('tail.y', [[0.4, sin(a, 8), ex(a, 4)]])] },
};

// idle: it is always up to something small. One at a time, while it is quiet: a ruffle, a wag, a shuffle, a preen, a cocked head, a nibble
const s9 = sgn(0), rs = [bump(a, 0.05, 0.6), sin(a, 13)], rsA = [bump(a, 0.05, 0.6), ['abs:sin', a, 13]], sa = bump(a, 0.05, 0.45), sb = bump(a, 0.5, 0.85), pre2 = env(a, 1.9, 0.3, 0.35), ck = [snap(a, 0.05), ['ssd', a, 1.15, 0.12]];
const R0 = lt('~0', 0.5), R1 = gt('~0', 0.5); // the side drawn: R0 left (-1), R1 right (+1)
const quiet = [lt('@act', 0.5), lt('@walk.k', 0.1), lt('@rot.x', 0.3), gt('@rot.x', -0.3), lt('@head.x', 0.35), gt('@head.x', -0.35)];
const events = {
  idle: { first: 2, gap: [0.6, 3.2], gate: [[1, ...quiet, gt('@w.idle', 0.6)]], w: [[1, '@w.idle', '@_qt']], stop: [[1, gt('@walk.k', 0.3)]], pick: {
    ruffle: { len: 0.75, ch: [ch('duck.wl', [[0.35, bump(a, 0, 0.7)], [0.1, ...rs]]), ch('duck.wr', [[0.35, bump(a, 0, 0.7)], [0.1, ...rs]]), ch('duck.puff', [[0.6, bump(a, 0, 0.75)]]), ch('rot.z', [[0.03, ...rs]])] },
    wag: { len: 0.7, ch: [ch('tail.y', [[0.6, sin(a, 8), bump(a, 0, 0.7)]])] },
    shuffle: { len: 1.1, ch: [ch('rot.z', [[0.05, s9, sa], [-0.03, s9, sb]]), ch('duck.fr', [[0.05, sa, R1], [0.03, sb, R0]]), ch('duck.fl', [[0.03, sb, R1], [0.05, sa, R0]]), ch('duck.zr', [[0.03, sa, R1]]), ch('duck.zl', [[0.03, sa, R0]])] },
    preen: { len: 1.9, ch: [ch('head.y', [[1.05, s9, pre2]]), ch('head.x', [[0.35, pre2]]), ch('head.z', [[0.25, s9, pre2]]), ch('lid', [[0.5, pre2]], { op: 'max' }), ch('duck.bill', [[0.3, ['abs:sin', a, 6], bump(a, 0.45, 1.5)]]), ch('duck.wl', [[0.3, pre2, R1]]), ch('duck.wr', [[0.3, pre2, R0]]), ch('duck.neck', [[-0.2, pre2]])] },
    cock: { len: 1.4, ch: [ch('head.z', [[0.38, s9, ...ck]]), ch('head.y', [[0.2, s9, ...ck]]), ch('look.x', [[0.3, s9, ...ck]])] },
    nibble: { len: 0.6, ch: [ch('duck.bill', [[0.3, ['abs:sin', a, 4.5], bump(a, 0, 0.6)]]), ch('head.x', [[0.08, bump(a, 0, 0.6)]])] },
  } },
  // happy: the shared hop is a metronome; it is damped (in post) and the duck hops with its wingbeats instead, now and then
  hop: { rate: 0.54, gate: [[1, '@w.happy']], len: 0.62, ch: [ch('_hopfly', [[1, bump(a, 0, 0.45)]]), ch('_hopland', [[1, bump(a, 0.42, 0.62)]])] },
};

// the last word on the pose, then the duck's own parts on its bones
const W = (m) => '@w.' + m;
const pose = [
  ch('_qt', [[1, ...quiet]], { lp: 10, keep: 1 }),
  ch('pos.y', [[-0.75, W('happy')]], { op: '*' }), ch('squash', [[-0.6, W('happy')]], { op: '*' }), ch('pos.y', [[0.08, W('happy'), '@_hopfly']]), ch('squash', [[0.05, W('happy'), '@_hopfly'], [-0.1, W('happy'), '@_hopland']]),
  ch('duck.neck', [[0.3, '@mouth', W('speaking')]]), ch('head.x', [[-0.06, '@mouth', W('speaking')]]), ch('duck.bill', [[1, '@mouth']], { op: 'max' }),
  ch('_wake', [[4, W('surprised')], [2, W('happy')]], { cl: [0, 1] }), ch('duck.tuck', [[-1, '@_wake']], { op: '*' }),
  // follow-through: the tuft lags the head, the wings overshoot and settle
  ch('_tz', [[1, '@head.z'], [1, '@rot.z'], [0.6, '@head.y']]), ch('_tx2', [[1, '@head.x'], [4, '@pos.y'], [-0.5, '@duck.neck']]),
  ch('_tuft', [[1, '@_tz']], { sp: [170, 7], lag: 1, cl: [-0.6, 0.6] }), ch('_tuftx', [[1, '@_tx2']], { sp: [140, 6], lag: 1, cl: [-0.6, 0.6] }),
  ch('duck.wl', [[1, '@duck.wl']], { sp: [900, 24] }), ch('duck.wr', [[1, '@duck.wr']], { sp: [900, 24] }),
];
const rig = [
  // wings from the shoulder (the duck's own wings replace the shared spread), the lower bill on its hinge, the head tucked or thrust
  ch('wingL:rx', [[0.8, '@duck.wx'], [0.06, sin(T, 9 / (2 * Math.PI)), clamp('@duck.wl', -9, 1)]]), ch('wingL:rz', [[1, clamp('@duck.wl', -0.1, 9)]]), ch('wingL:sy', [[0.15, clamp('@duck.wl', 0, 1)]]),
  ch('wingR:rx', [[0.8, '@duck.wx'], [0.06, sin(T, 9 / (2 * Math.PI), 2), clamp('@duck.wr', -9, 1)]]), ch('wingR:rz', [[-1, clamp('@duck.wr', -0.1, 9)]]), ch('wingR:sy', [[0.15, clamp('@duck.wr', 0, 1)]]),
  ch('jaw:rx', [[0.75, '@duck.bill']]),
  ch('head:py', [[0.07, '@duck.neck'], [-0.13, '@duck.tuck']]), ch('head:pz', [[0.04, neg('@duck.neck')], [-0.2, '@duck.tuck']]), ch('head:ry', [[2.6, '@duck.tuck']]), ch('head:rx', [[0.4, '@duck.tuck']]), ch('head:s', [[0.05, '@duck.puff']]), ch('torso:s', [[0.1, '@duck.puff']]),
  ch('tuft:rz', [[1.6, '@_tuft']]), ch('tuft:rx', [[1.6, '@_tuftx']]),
  ch('footL:py', [[1, '@duck.fl']]), ch('footL:pz', [[1, '@duck.zl']]), ch('footL:rx', [[1, '@duck.kl']]), ch('footL:s', [[1, '@duck.feet']]),
  ch('footR:py', [[1, '@duck.fr']]), ch('footR:pz', [[1, '@duck.zr']]), ch('footR:rx', [[1, '@duck.kr']]), ch('footR:s', [[1, '@duck.feet']]),
];

const fid = alive(ID, { fidget: 1.1, shake: 1 }), live = living(body, { sides: false, motions: [blinkBurst(5)] });
// about: what a person sees on the round screen, for the agent that speaks as this Molty (docs/CREATURES.md section 1)
const about = {
  look: 'A round yellow duckling, mostly head, with a small orange bill, pink cheeks and a little tuft on top. Stubby wings at its sides, a short tail behind and two big orange paddle feet.',
  gestures: {
    wiggle: 'waggles its tail end fast while its body rocks and its head stays level',
    nod: 'bobs its head forward two or three times, the bill clicking at the bottom',
    shake_head: 'shakes its head fast as if shaking off water, the bill rattling, then ruffles',
    bounce: 'crouches, then hops up with its wings thrown open and lands soft',
    peek: 'ducks down low, then stretches up tall and cranes about, one eye then the other',
    glance_left: 'snaps its head round to its left so one eye looks, holds still, snaps back',
    glance_right: 'snaps its head round to its right so one eye looks, holds still, snaps back',
    blink: 'a quick blink with a tiny dip of the head',
    wobble: 'loses its footing and tips over, wings shooting out, then steps to catch itself',
    stretch: 'sweeps one wing and leg out behind, then arches both wings up over its back',
    look_up: 'tips its head right over so one eye faces the sky and scans in little snaps',
    yawn: 'a big yawn, the bill gaping wide and quivering, the wings easing out, then a shiver',
    paddle: 'sits low as if afloat and kicks its feet in turn, turning a little with each stroke',
  },
  moods: {
    idle: 'stands and keeps busy with small things: a ruffle, a tail wag, a preen, a nibble',
    listening: 'cocks its head so one eye faces you and goes still',
    thinking: 'looks up with one eye, then the other, the bill nibbling at nothing',
    speaking: 'the bill moves with the words and the wings lift a little',
    happy: 'flaps its wings in bursts, wags its tail fast, bobs its head and hops',
    sleepy: 'settles down, tucks its bill into its back, draws one foot up and fluffs out',
    surprised: 'puffs right up, throws its wings out, stretches tall and steps back',
  },
};
const pkg = { v: 'molty-pkg/1', id: ID, name: 'Duck', version: '1.0.0', kind: 'creature',
  creature: { about, body: { palette, ...body }, walk, groups: live.groups, events: { ...fid.events, ...live.events, ...events }, base: fid.base, gestures, moods, post: [...pose, ...live.post, ...rig] } };
const out = writePackage(new URL(`./${ID}.json`, import.meta.url).pathname, pkg);
console.log(ID, out.size, 'bytes', out.sha256);
