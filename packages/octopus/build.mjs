#!/usr/bin/env node
// The octopus as a creature package: soft and boneless, under water. The mantle breathes and swells, eight arms act on their own,
// colour comes and goes (a flush when pleased, pale when startled, passing clouds while it thinks), and the water round it is alive:
// bubbles rise, motes drift, a slow current stirs the fluid, ink when it is startled.
// Its own channels (0 at rest): reach, limp, curl, ring (a wave round the skirt), swirl (one arm circles), chin (an arm to its face),
// drum (tips tap in turn), talk (front arms gesture), point (+/- one side's arm lifts), drag (arms trail a turn), ex/exI (one arm feels
// about), twitch, flex (arms push down), swell (the mantle fills), far (shot back), flush (+ rosier, - pale), cloud, puff/gush/jet/ink (bubbles).
import { ch, sin, cos, nz, env, bump, ss, ssd, ex, lin, lt, gt, clamp, E, B, P, CHAIN, eyes, mouth, merge, LR, labHash, alive, living, blinkBurst, breathe, writePackage } from '../../tools/creatures/kit.mjs';
import { ACTIONS } from '../../device/creatures/js/actions.js';
const ID = 'octopus', a = 'a', T = '@t', TAU = Math.PI * 2, D = (n) => ACTIONS[n];

const ARMS = Array.from({ length: 8 }, (_, i) => { const an = (i + 0.5) / 8 * TAU; return { id: `arm${i}`, x: Math.cos(an), z: Math.sin(an) }; });
const SEG = ['', 's1', 's2', 's3'];
const body = merge({ bones: [{ id: 'torso', pos: [0, 0.2, 0], d: 0.5 }, { id: 'head', parent: 'torso', pos: [0, 0.22, 0] }],
  parts: [E('head', [0.31, 0.32, 0.28]), E('torso', [0.25, 0.14, 0.23]), ...LR((d) => [E('head', [0.075, 0.06, 0.03], [d * 0.14, 0.2, 0.22], 'accent'), P(E('head', [0.05, 0.03, 0.01], [d * 0.2, -0.1, 0.235], 'cheek'))]), E('head', [0.05, 0.05, 0.03], [0, 0.26, 0.235], 'accent'), P(B('head', [0.1, 0.012, 0.01], [0, -0.12, 0.26], 'mouth'))] },
  eyes('head', 'glossy', 0.12, -0.02, 0.245, 0.9), mouth(-0.12, 0.275),
  ...ARMS.map((q, i) => CHAIN(SEG.map((s) => q.id + s), 'torso', [+(q.x * 0.16).toFixed(3), -0.04, +(q.z * 0.15).toFixed(3)], [[0, 0, 0], [q.x * 0.14, -0.12, q.z * 0.14], [q.x * 0.3, -0.15, q.z * 0.3], [q.x * 0.42, -0.06, q.z * 0.42], [q.x * 0.47, 0.03, q.z * 0.47]].map((p) => p.map((v) => +v.toFixed(3))), 0.046, 'body', (j) => ({ wave: [+(-q.z).toFixed(3), +q.x.toFixed(3), j, i], d: 1 + j }))));
const palette = { eye: '#0d0a12', shine: '#ffffff', maw: '#160810', body: '#ff6f86', accent: '#d93a62', belly: '#ffd3cc', cheek: '#ffb0c0', mouth: '#7a1f3a' };

// the jet: arms flare, snap together and trail as it shoots off mantle first, then it glides and opens again. It only covers ground
// while surge(q) says so, and always finishes the cycle it started
const q = '@walk.q', K = '@walk.k';
const walk = { hz: 0.8, burst: 1, radial: 1, surge: [[0.2, lt(q, 0.2)], [3.3, gt(q, 0.2), ex(q, 3.4, 0.2)]], ch: [
  ch('_jg', [[-0.5 / 0.15, clamp(q, 0, 0.15)], [1.5 / 0.15, clamp(q, 0.15, 0.3)], [-1.5], [-1 / 0.7, clamp(q, 0.3, 1)], [0.3 / 0.7]]),
  ch('gather', [[1, K, '@_jg']]), ch('squash', [[0.16, K, ['pos:v', '@_jg']], [-0.06, K, ['neg:v', '@_jg']]]),
  ch('rot.z', [[-0.75, '@walk.side', '@walk.go']]), ch('rot.x', [[0.45, '@walk.fz']]), ch('pos.y', [[0.08, '@walk.go']])] };

// gestures, done with eight arms and a soft body
const glance = (s) => { const d = D('glance_left'), e = env(a, d, 0.12, 0.35), late = [ss(a, 0.15, 0.25), ssd(a, d - 0.35, 0.35)];
  return { ch: [ch('look.x', [[0.9 * s, e]]), ch('head.y', [[0.16 * s, ...late]]), ch('head.z', [[-0.05 * s, ...late]]), ch('rot.y', [[0.12 * s, ...late]]), ch('point', [[s, ...late]])] }; };
const gestures = {
  // a shimmy: the skirt ripples round and the mantle twists on it
  wiggle: { ch: (() => { const e = env(a, D('wiggle'), 0.15, 0.4); return [ch('rot.y', [[0.22, e, sin(a, 1.7)]]), ch('rot.z', [[0.05, e, sin(a, 1.7, -1)]]), ch('squash', [[0.05, e, sin(a, 3.4)]]), ch('ring', [[1.4, e]])]; })() },
  // no neck: the whole mantle dips twice, the second smaller and later, and the arms press down to push it back up
  nod: { ch: (() => { const e = env(a, D('nod'), 0.08, 0.3); return [ch('_d', [[0.24, bump(a, 0.05, 0.5), e], [0.13, bump(a, 0.55, 0.95), e]]), ch('rot.x', [[1, '@_d']]), ch('head.x', [[0.25, '@_d']]), ch('pos.y', [[-0.12, '@_d']]), ch('squash', [[-0.25, '@_d']]), ch('look.y', [[-0.6, '@_d']]), ch('flex', [[3, '@_d']])]; })() },
  // it swivels on its arms, and they trail the turn
  shake_head: { ch: (() => { const e = env(a, D('shake_head'), 0.1, 0.35); return [ch('rot.y', [[0.32, e, sin(a, 2)]]), ch('head.y', [[0.1, e, sin(a, 2, -0.6)]]), ch('look.x', [[0.35, e, sin(a, 2)]]), ch('drag', [[-0.9, e, cos(a, 2)]])]; })() },
  // a jet straight up: arms flare, snap shut as it shoots, then open like a parachute as it sinks back
  bounce: { ch: (() => { const d = D('bounce'), flare = bump(a, 0, 0.3), go = [ss(a, 0.22, 0.14), ssd(a, 0.45, d)], sink = ss(a, 0.5, 0.7);
    return [ch('pos.y', [[-0.04, flare], [0.24, ...go], [-0.204, ...go, sink]]), ch('squash', [[-0.1, flare], [0.16, bump(a, 0.22, 0.6)], [-0.05, bump(a, 0.7, d)]]), ch('gather', [[-0.6, flare], [1.1, bump(a, 0.22, 0.62)], [-0.5, bump(a, 0.6, d)]]), ch('jet', [[1, bump(a, 0.2, 0.32)]])]; })() },
  // it flattens itself down to hide, then the eyes rise first and look round
  peek: { ch: (() => { const e = env(a, D('peek'), 0.25, 0.5), dip = [ss(a, 0, 0.25), ssd(a, 0.75, 0.4)], up = [ss(a, 0.8, 0.35), e], s = sin(a, 0.9, -TAU * 0.9 * 0.9);
    return [ch('pos.y', [[-0.12, ...dip]]), ch('squash', [[-0.3, ...dip], [0.04, ...up]]), ch('limp', [[0.9, ...dip]]), ch('look.x', [[0.8, s, ...up]]), ch('look.y', [[0.25, ...up], [-0.3, ...dip]]), ch('head.y', [[0.12, s, ...up]]), ch('head.x', [[-0.1, ...up]]), ch('lid', [[0.3, ...dip]], { op: 'max' })]; })() },
  glance_left: glance(-1), glance_right: glance(1),
  // a blink squeezes the mantle a touch, and lets out one small bubble
  blink: { ch: [ch('lid', [[1 / 0.07, clamp(a, 0, 0.07)], [-1 / 0.16, clamp(a, 0.14, 0.3)], [0.14 / 0.16]], { op: 'max' }), ch('squash', [[-0.04, bump(a, 0, 0.3)]]), ch('swell', [[-0.05, bump(a, 0, 0.3)]]), ch('puff', [[1, lt(a, 0.2)]])] },
  // jelly: the soft body wobbles on and on, each wave smaller, the arms a beat behind
  wobble: { ch: (() => { const k = [ex(a, 2), ss(a, 0, 0.06), ssd(a, D('wobble') - 0.3, 0.3)]; return [ch('squash', [[0.14, ...k, sin(a, 3.1)]]), ch('rot.z', [[0.07, ...k, sin(a, 1.6)]]), ch('swell', [[0.08, ...k, sin(a, 3.1, 1.2)]]), ch('ring', [[0.8, ...k]])]; })() },
  // every arm reaches out as far as it goes, the mantle drawn up long, then it lets go with a shiver
  stretch: { ch: (() => { const d = D('stretch'), e = env(a, d, 0.55, 0.45), sh = [bump(a, d - 0.5, d), sin(a, 9)];
    return [ch('reach', [[1, e]]), ch('squash', [[0.15, e], [0.03, ...sh]]), ch('head.x', [[-0.1, e]]), ch('lid', [[0.4, e]], { op: 'max' }), ch('pos.y', [[0.04, e]]), ch('drag', [[0.3, ...sh]])]; })() },
  // it tips its mantle back and the front arms lift; a few bubbles go up for it to watch
  look_up: { ch: (() => { const e = env(a, D('look_up'), 0.3, 0.45); return [ch('rot.x', [[-0.2, e]]), ch('head.x', [[-0.2, e]]), ch('look.y', [[0.9, e]]), ch('look.x', [[0.15, e, sin(a, 0.6)]]), ch('curl', [[0.6, e]]), ch('pos.y', [[0.03, e]]), ch('puff', [[1, gt(a, 0.15), lt(a, 0.3)]])]; })() },
  // the mantle fills right up, holds, then lets it all out in a gush of bubbles
  yawn: { ch: (() => { const fill = [ss(a, 0, 1.1), ssd(a, 1.75, 0.35)]; return [ch('swell', [[0.3, ...fill]]), ch('squash', [[0.06, ...fill], [-0.08, bump(a, 1.75, 2.5)]]), ch('head.x', [[-0.18, ...fill]]), ch('lid', [[0.7, env(a, D('yawn'), 0.5, 0.6)]], { op: 'max' }),
    ch('mouth', [[0.9, env(a, 2.1, 0.6, 0.4), lt(a, 2.1)]], { op: 'max' }), ch('reach', [[0.35, ...fill]]), ch('gush', [[1, gt(a, 1.8), lt(a, 1.95)]])]; })() },
  // it swirls one arm round and round in the water, the body rocking against it
  paddle: { ch: (() => { const e = env(a, D('paddle'), 0.35, 0.5); return [ch('swirl', [[1, e]]), ch('rot.z', [[-0.05, e, sin(a, 1.1)]]), ch('rot.y', [[0.1, e]]), ch('look.x', [[0.35, e]]), ch('look.y', [[-0.25, e]]), ch('head.x', [[0.06, e]])]; })() },
};

// moods: what holds while it feels that way (the sharp moments are in post)
const moods = {
  listening: { ch: [ch('curl', [[0.8], [0.2, nz(T, 0.8, 3)]]), ch('gather', [[0.15]]), ch('rot.x', [[0.06]]), ch('pos.y', [[0.015]])] },
  thinking: { ch: [ch('cloud', [[1]]), ch('chin', [[1, ss(a, 0, 0.9)]]), ch('drum', [[1, ss(a, 0.8, 0.6)]]), ch('flush', [[-0.1]])] },
  speaking: { ch: [ch('ring', [[0.15]])] },
  happy: { ch: [ch('flush', [[1]]), ch('ring', [[0.9]]), ch('pos.y', [[0.07], [0.025, sin(T, 0.45)]]), ch('rot.y', [[0.18, sin(T, 0.21)]])] },
  sleepy: { ch: [ch('limp', [[1]]), ch('flush', [[-0.5]]), ch('pos.y', [[-0.08]]), ch('squash', [[-0.09]]), ch('rot.x', [[0.05]]), ch('swell', [[0.04, sin(T, 0.16)]])] },
  surprised: { ch: [ch('rot.x', [[-0.08]])] },
};

// an arm wanders off by itself to feel about, then comes back
const events = { explore: { first: 2, gap: [1.5, 5.5], len: [1.6, 3.4, 7], ch: [ch('_exv', [[1, ss(a, 0, [0.48, 1.02, 7]), ['ssd', a, [1.04, 2.21, 7], [0.56, 1.19, 7]]]]), ch('exI', [[1, ['fl', '~1', 8]]])] } };

// the last word on the pose
const S = '@m.surprised', sk = '@now.surprised', Z = '@m.sleepy';
const pose = [
  ch('talk', [[1, '@mouth', '@now.speaking']]), ch('_still', [[1], [-1.5, K]], { cl: [0, 1] }),
  // breathing: the mantle fills and empties, unevenly; now and then a breath goes out as a bubble (ambient)
  ch('_br', [[0.32, lin('@w.sleepy', 1, -0.45)], [0.1, nz(T, 0.13, 7), lin('@w.sleepy', 1, -0.45)]], { int: 1 }), ch('swell', [[0.05, sin('@_br', 1)], [0.025, nz(T, 0.2, 9), sin('@_br', 1)]]),
  ch('ex', [[1, '@_exv', '@_still', lin('@w.sleepy', 1, -0.8), lin('@swirl', 1, -0.7)]]),
  // startled: it blanches white, bunches its arms, shoots back and squirts ink, then flushes dark as it recovers
  ch('flush', [[-1, sk, ss(S, 0, 0.06), ssd(S, 0.5, 0.7)], [0.7, sk, ss(S, 0.7, 0.5)]]), ch('gather', [[1.1, sk, ss(S, 0, 0.05), ssd(S, 0.45, 0.5)], [-0.35, sk, bump(S, 0.8, 2.6)]]),
  ch('far', [[1, sk, ss(S, 0, 0.1)], [-0.55, sk, ss(S, 0, 0.1), ss(S, 0.6, 1.6)]]), ch('pos.y', [[0.1, sk, bump(S, 0, 0.7)]]), ch('squash', [[0.1, sk, bump(S, 0, 0.35)]]), ch('ink', [[1, sk, lt(S, 0.4)]]),
  // asleep, it dreams: every few seconds a wave of colour and a twitch runs through its arms
  ch('_dream', [[1, '@is.sleepy', ['chance', Z, 2.4, 4.2, 0.7, 1], gt(Z, 1)]]), ch('flush', [[0.9, '@_dream', ['bumpr', Z, 1 / 2.4, -1 / 2.4, 0, 1.3 / 2.4]]]), ch('twitch', [[1, '@_dream', ['bumpr', Z, 1 / 2.4, -1 / 2.4, 0.1 / 2.4, 0.7 / 2.4]]]),
  // turning, its arms trail behind
  ch('_ospin', [[1, '@rot.y']], { d: [-6, 6], lp: 6 }), ch('drag', [[-0.12, '@_ospin']]),
  // in water nothing drops: up and down is slow, and happy turns hops into floating
  ch('pos.y', [[1, '@pos.y']], { lp: { _: 16, happy: -13 } }), ch('squash', [[1, '@squash']], { lp: { _: 30, happy: -24 } }),
  // the current wanders across the glass
  ch('_cx', [[0.6, sin(T, 0.13 / TAU)]]), ch('_cy', [[0.4, cos(T, 0.17 / TAU)]]), ch('_cvx', [[0.08, sin(T, 0.31 / TAU)]]), ch('_cvy', [[0.05, cos(T, 0.23 / TAU)]]),
];

// the arms: per joint coefficients, laid out once per arm and segment (attributes), so one list of channels moves all 32 joints
const at = (f) => ARMS.flatMap((A, i) => SEG.map((_, j) => +f(A, i, j).toFixed(4)));
const front = (A) => Math.max(0, A.z), pick = (j, v) => v[j];
const pointy = (A, s) => (A.x * s > 0.3 && A.z > -0.2 ? 1 : 0); // the arms on one side that lift when it points that way
const arms = { bones: ARMS.flatMap((A) => SEG.map((s) => A.id + s)), attr: {
  ax: at((A) => -A.z), az: at((A) => A.x), lag: at((A, i) => 0.25 * labHash(i * 5.3)),
  kr: at((A, i, j) => pick(j, [0.5, -0.1, -0.4, -0.55])), kL: at((A, i, j) => pick(j, [-0.08, 0.06, -0.14, -0.2])), kF: at((A, i, j) => (j ? 0 : -0.35)), kC: at((A, i, j) => pick(j, [0.18, 0, 0.75, 1]) * front(A)),
  k6: at((A, i, j) => (j === 3 ? 0.08 : 0)), ph6: at((A, i) => i), kG: at((A, i, j) => pick(j, [0.35, 0.25, 0.3, 0.3])), gph: at((A, i, j) => -i * TAU / 8 - 0.9 * j),
  kD: at((A, i, j) => (j === 3 ? 0.5 * front(A) : 0)), dph: at((A, i) => -0.17 * i), kT1: at((A, i, j) => (j === 1 ? 0.35 * labHash(i + 0.5) : 0)), tph1: at((A, i) => 2 * i), kT2: at((A, i, j) => (j === 2 ? 0.3 : 0)),
  kPp: at((A, i, j) => pointy(A, 1) * pick(j, [0.6 * Math.abs(A.x), 0, 0, 0.4])), kPn: at((A, i, j) => pointy(A, -1) * pick(j, [0.6 * Math.abs(A.x), 0, 0, 0.4])),
  kE: at((A, i, j) => pick(j, [0.55, 0, -0.35, 0])), kE3: at((A, i, j) => (j === 3 ? 0.5 : 0)), kEy: at((A, i, j) => pick(j, [0.25, 0, 0, 0])), kEy2: at((A, i, j) => (j === 2 ? 0.3 : 0)),
  kDr: at((A, i, j) => 0.35 + 0.25 * j), root: at((A, i, j) => (j ? 0 : 1)) } };
const armRig = { each: 'arms', ch: [
  ch('%r', [[1, ['ss', '@reach', '$lag', 0.75]]]),
  ch('%c', [['$kr', '@%r'], ['$kL', '@limp'], ['$kF', '@flex'], ['$kC', '@curl'], ['$k6', '@%r', ['sin', T, 6, '$ph6']], ['$kG', '@ring', ['sin', T, 1.3, '$gph']], ['$kD', '@drum', ['bumpr', T, 2.3, '$dph', 0, 0.22]],
    ['$kT1', '@twitch', ['sin', T, 31 / TAU, '$tph1']], ['$kT2', '@twitch', ['sin', T, 23 / TAU, '$ph6']], ['$kPp', ['pos:v', '@point']], ['$kPn', ['neg:v', '@point']],
    ['$kE', '@ex', ['is', '@exI', '$ai']], ['$kE3', '@ex', ['is', '@exI', '$ai'], sin(T, 1.6)]]),
  ch(':rx', [['$ax', '@%c']]), ch(':rz', [['$az', '@%c']]),
  ch(':ry', [['$kEy', '@ex', ['is', '@exI', '$ai'], sin(T, 0.5)], ['$kEy2', '@ex', ['is', '@exI', '$ai'], sin(T, 1.1)], ['$kDr', '@drag']]),
  ch(':s', [[0.22, '$root', '@%r'], [0.08, '$root', '@limp']])] };
arms.attr.ai = at((A, i) => i);
// the special arms: arm 0 circles (paddle), arm 2 comes up to its face (thinking), arms 1 and 2 talk with their hands
const seg = (i, j, p, terms) => { const A = ARMS[i], id = A.id + SEG[j]; return p === 'c' ? [ch(id + ':rx', terms.map(([g, ...f]) => [g * -A.z, ...f])), ch(id + ':rz', terms.map(([g, ...f]) => [g * A.x, ...f]))] : [ch(id + ':' + p, terms)]; };
const W = '@swirl', H = '@chin', TK = '@talk', wp = (k) => sin(T, 1.05, k);
const special = [
  ...seg(0, 0, 'c', [[1.1, W], [0.4, W, wp(0)]]), ...seg(0, 0, 'ry', [[0.8, W, cos(T, 1.05)]]), ...seg(0, 1, 'c', [[0.45, W, wp(-1)]]), ...seg(0, 2, 'c', [[0.55, W, wp(-2)]]), ...seg(0, 3, 'c', [[0.7, W, wp(-3)]]),
  ...seg(2, 0, 'c', [[1, H]]), ...seg(2, 0, 'ry', [[-0.5, H]]), ...seg(2, 1, 'c', [[0.7, H]]), ...seg(2, 2, 'c', [[0.7, H], [0.25, H, ['bumpr', T, 0.9, 0, 0, 0.3]]]), ...seg(2, 3, 'c', [[0.5, H]]),
  ...[1, 2].flatMap((i) => [...seg(i, 0, 'c', [[0.35, TK], [0.3 * (i === 1 ? 1 : -1), TK, sin(T, 0.55)]]), ...seg(i, 3, 'c', [[0.5, TK, sin(T, 1.7, i)]])]),
];
const rig = [armRig, ...special,
  // the mantle fills and empties; shot back, it is smaller
  ch('head:sx', [[1, '@swell']]), ch('head:sz', [[1, '@swell']]), ch('head:sy', [[1.2, '@swell']]), ch('head:py', [[0.12, '@swell']]), ch('_far', [[1, '@far']], { cl: [0, 1] }), ch('root:s', [[-0.3, '@_far']])];

// colour: rosier, paler, or dark clouds rippling from the mantle down the arms. Only its skin changes, never the eyes
const tints = [{ ch: 'flush', to: { body: '#ff4a78', accent: '#c2245a' }, neg: { body: '#ffd2da', accent: '#f0a0b4' } }, { ch: 'cloud', to: { body: '#8c1f45', accent: '#5a0f2c' }, wave: [0.55, 0.22, 0.9] }];
// under water: bubbles always rising and popping at the top, a few from the octopus itself, motes drifting, ink, a slow current
const ambient = {
  kinds: { bubble: { type: 'bubble', rise: [0.14, 1.1, 0.05], acc: 0.05, f: [1.5, 3.5], wob: [0.015, 0.045], glass: 0.82, pop: [0.15, '#8fc6e0', 0.05, 0.25, 0.05], big: 0.05, color: ['#6fa9c8', '#9ad4f0'], shine: [0.06, '#e8f8ff'] },
    ink: { type: 'blot', vx: [-0.1, 0.1], pts: 9, life: 2.6, sink: 0.06, spin: 0.3, color: '#4b3a72' } },
  emit: { water: { p: 'bubble', every: [0.35, 1.25], at: 'floor', r: [0.03, 0.08], rp: 2, twin: 0.25 },
    breath: { p: 'bubble', rate: ['_still', 0.144, 0.01], at: 'side', n: [1, 2], dx: [0, 0.02], dy: [-0.06, 0], r: [0.035, 0.06] },
    puff: { p: 'bubble', on: 'puff', at: 'top', r: 0.045 }, gush: { p: 'bubble', on: 'gush', at: 'side', n: 7, dx: [-0.04, 0.04], dy: [-0.05, 0.05], r: [0.03, 0.1] },
    jet: { p: 'bubble', on: 'jet', at: 'bottom', n: 6, dx: [-0.125, 0.125], dy: [0, 0.08], r: [0.03, 0.08], impulse: [0.5, 0.1] },
    trail: { p: 'bubble', rate: ['walk.k', 2.5, 0.3], at: 'bottom', dx: [-0.1, 0.1], dy: 0.05, r: [0.025, 0.055] },
    ink: { p: 'ink', on: 'ink', at: 'center', n: 4, dx: [-0.2, 0.2], stack: 0.07, stagger: 0.06, r: [0.1, 0.16], impulse: [0.8, 0.12] } },
  motes: { n: 16, fall: [0.012, 0.02], drift: 0.02, size: [0.035, 0.02], color: ['#22404f', '#2f5468'] },
  current: { ch: ['_cx', '_cy', '_cvx', '_cvy'], r: 0.4 } };

const fid = alive(ID), live = living(body, { gait: { type: 'float', height: 0.03, amp: 1.2, rate: 0.9 }, motions: [breathe('head', 'sy', 0.025, 0.5), blinkBurst(4)] });
const pkg = { v: 'molty-pkg/1', id: ID, name: 'Octopus', version: '1.0.0', kind: 'creature',
  creature: { body: { palette, ...body }, walk, groups: { ...live.groups, arms }, events: { ...fid.events, ...live.events, ...events }, base: fid.base, gestures, moods, post: [...pose, ...live.post, ...rig], tints, ambient } };
const out = writePackage(new URL(`./${ID}.json`, import.meta.url).pathname, pkg);
console.log(ID, out.size, 'bytes', out.sha256);
