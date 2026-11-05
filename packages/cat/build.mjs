#!/usr/bin/env node
// The cat as a creature package: slow and fluid, all in the spine, the ears and the tail. It never hops; it sits on planted hips and
// lets the chest sway, loafs to sleep, arches when startled, kneads when happy, swats with a paw, and its ears say what it listens to.
// Its own channels: chest.x/z sway over the hips; earF.l/r flatten (+) or prick (-); earT.l/r swivel; paw.l/r a front paw raised;
// swat.l/r a raised paw swung sideways; tailUp, tailTip (quiver), tailWrap; fluff; stand, crouch, loaf, arch, dog (a stretch); tongue.
import { ch, sin, cos, nz, env, bump, ss, ssd, ex, lin, lt, gt, clamp, pos, neg, E, B, C, CY, P, TAIL, eyes, mouth, merge, LR, alive, living, ponder, sgn, blinkBurst, breathe, aimChain, writePackage } from '../../tools/creatures/kit.mjs';
import { ACTIONS } from '../../device/creatures/js/actions.js';
const ID = 'cat', EAT = 4.6, a = 'a', T = '@t', TAU = Math.PI * 2, D = (n) => ACTIONS[n];
const both = (k, terms, o) => [ch(k + '.l', terms, o), ch(k + '.r', terms, o)];

// four legs, each with a knee; the back ones on the hips so the spine can bend. Seated and standing positions on each
const Q = { pitch: 1.25, rise: -0.06, neck: 0.12, r: 0.035, fx: 0.06, fsit: [-0.05, 0.12], fwalk: [0.18, 0.05], flen: 0.25, bx: 0.1, bsit: [-0.2, 0], bwalk: [-0.2, -0.02], blen: 0.2, fold: 0.5, thigh: [0.08, 0.11, 0.12], paw: 'belly' }, HIPS = [-0.12, -0.02];
const leg = (n, x, sit, walk, len, back) => { const at = (p) => (back ? [p[0] - HIPS[0], p[1] - HIPS[1]] : p), sh = 'shin' + n.slice(3);
  return { bones: [{ id: n, parent: back ? 'hips' : 'torso', pos: [x, ...at(walk)], sit: [x, ...at(sit)], fold: back ? Q.fold : 1 }, { id: sh, parent: n, pos: [0, -len / 2, 0] }],
    parts: [CY(n, Q.r, len / 2, [0, -len / 4, 0]), E(sh, [Q.r, Q.r, Q.r]), CY(sh, Q.r * 0.9, len / 2, [0, -len / 4, 0]), E(sh, [Q.r * 1.3, 0.025, Q.r * 1.8], [0, -len / 2, Q.r * 0.6], Q.paw), ...(back ? [E(n, Q.thigh, [0, -0.02, 0])] : [])] }; };
const legs = merge(leg('legFL', Q.fx, Q.fsit, Q.fwalk, Q.flen), leg('legFR', -Q.fx, Q.fsit, Q.fwalk, Q.flen), leg('legBL', Q.bx, Q.bsit, Q.bwalk, Q.blen, true), leg('legBR', -Q.bx, Q.bsit, Q.bwalk, Q.blen, true));
const body = merge({ bones: [{ id: 'torso', pos: [0, 0.3, 0] }, { id: 'hips', parent: 'torso', pos: [0, -0.12, -0.02] }, { id: 'head', parent: 'torso', pos: [0, 0.34, 0.06] }, ...LR((d, n) => [{ id: 'ear' + n, parent: 'head', pos: [d * 0.115, 0.12, 0], rot: [0, 0, -d * 0.2] }]), { id: 'tongue', parent: 'head', pos: [0, -0.1, 0.17] }, { id: 'bowl', pos: [0, 0.04, 0.36] }],
  parts: [E('torso', [0.15, 0.25, 0.16], [0, 0.03, 0]), E('hips', [0.17, 0.15, 0.17], [0, -0.04, -0.01]), E('torso', [0.08, 0.15, 0.05], [0, 0.02, 0.13], 'belly'), E('head', [0.21, 0.18, 0.18]), ...LR((d, n) => [C('ear' + n, 0.075, 0.17, [0, 0.08, 0]), C('ear' + n, 0.04, 0.11, [0, 0.07, 0.035], 'ear'), P(B('head', [0.13, 0.008, 0.008], [d * 0.17, -0.05, 0.14], 'whisker', [0, 0, d * 0.1]))]), E('head', [0.02, 0.014, 0.012], [0, -0.04, 0.178], 'nose'), P(E('tongue', [0.026, 0.012, 0.024], [0, 0, 0], 'nose')),
    // the food bowl, there only while it eats (bowl:s): red, full of milk
    CY('bowl', 0.17, 0.1, [0, 0, 0], 'bowl', undefined, 0.12), P(E('bowl', [0.15, 0.014, 0.15], [0, 0.045, 0], 'belly'))] },
  TAIL('hips', [0.05, -0.08, -0.1], [[0, 0, 0], [0.18, 0.02, -0.06], [0.28, 0.2, -0.05], [0.22, 0.38, -0.03], [0.3, 0.46, -0.02]], 0.03, 'accent'), eyes('head', 'slit', 0.085, 0.01, 0.155, 0.7), mouth(-0.085, 0.18, 0.05), legs);
const palette = { eye: '#0d0a12', shine: '#ffffff', maw: '#160810', body: '#a99be8', accent: '#7566c4', belly: '#f4f0ff', ear: '#ffb7c8', nose: '#ff8fab', whisker: '#6e7590', iris: '#9be564', bowl: '#e8434f' };

// the stalk: slow, low and smooth. A paw stays planted while the body passes over it (the leg reaches to keep it on the floor);
// the spine snakes, the head stays aimed, the tail swishes low
const q = '@walk.q', K = '@walk.k', s = sin(q, 1), DS = 0.3 * 0.31;
const stalkLeg = (k, o, L) => [ch('_u' + k, [[1, ['frac', q, 1, -o]]]), ch('_x' + k, [[DS / L, '@_g', ['step', q, 1, -o, 0.62]]]), ch(`legs.${k}.sw`, [[1, ['asin', '@_x' + k]]]),
  ch(`legs.${k}.up`, [[0.2, '@_g', ['bumpr', q, 1, -o, 0.62, 1]], [-0.5, ['sq:v', '@_x' + k], lt('@_u' + k, 0.62)], [-0.375, ['sq:v', '@_x' + k], ['sq:v', '@_x' + k], lt('@_u' + k, 0.62)]])];
const walk = { hz: 0.9, stride: 0.3, pace: 0.3, quad: { pitch: Q.pitch, rise: Q.rise, neck: Q.neck }, ch: [ch('_g', [[3, K]], { cl: [0, 1] }),
  ...stalkLeg('FL', 0.25, 0.25), ...stalkLeg('FR', 0.75, 0.25), ...stalkLeg('BL', 0, 0.2), ...stalkLeg('BR', 0.5, 0.2),
  ch('pos.y', [[0.005, K, cos(q, 2)], [-0.02, K]]), ch('rot.z', [[0.03, K, s]]), ch('rot.y', [[0.04, K, s]]), ch('hips.bend', [[-0.18, K, sin(q, 1, -0.5)]]), ch('hips.roll', [[-0.09, K, sin(q, 1, 1.2)]]),
  ch('head.z', [[-0.03, K, s]]), ch('head.y', [[-0.06, K, s]]), ch('head.x', [[0.04, K, sin(q, 2, 2)]]), ch('tail.x', [[-0.3, K]]), ch('tail.y', [[0.25, K, sin(q, 1, -1.4)]])] };

// gestures
const dz = (hz, k, src = T) => ['dz', src, hz, k];
const glance = (side) => { const d = D('glance_left'), e = env(a, d, 0.12, 0.35), ear = [ss(a, 0, 0.08), ssd(a, d - 0.3, 0.3)], eye = [ss(a, 0.1, 0.08), e], hd = [ss(a, 0.2, 0.3), e];
  // the ears get there first, then the eyes, then the head; the tail answers late
  return { ch: [...both('earT', [[0.8 * side, ...ear]]), ...both('earF', [[-0.3, ...ear]]), ch('look.x', [[0.85 * side, ...eye]]), ch('head.y', [[0.26 * side, ...hd], [0.06 * side, bump(a, 0.35, 0.7)]]), ch('head.z', [[-0.06 * side, ...hd]]), ch('tail.y', [[0.35 * side, bump(a, 0.45, 1.05)]])] }; };
const nb = bump(a, 0.1, D('nod') - 0.05), bc = [ss(a, 0, 0.15), ssd(a, 0.45, 0.1)], bj = bump(a, 0.48, 0.85), bl = bump(a, 0.78, D('bounce'));
const atL = (lag) => [ex(a, 3, lag), ss(a, lag, 0.06), sin(a, 7, -TAU * 7 * lag), ssd(a, D('wobble') - 0.3, 0.3)];
const pr = (x, y) => bump(a, x, y);
const gestures = {
  // a hunting wiggle: down low, the hips shimmy side to side, the tail tip twitches, eyes locked ahead
  wiggle: { ch: (() => { const e = env(a, D('wiggle'), 0.3, 0.35); return [ch('_ph', [[3.4, a], [0.12, nz(a, 2, 3)]]), ch('crouch', [[0.9, e]]), ch('hips.bend', [[0.34, e, sin('@_ph', 1)]]), ch('hips.roll', [[0.12, e, sin(a, 3.4, -1)]]), ch('tailTip', [[1, e]]), ch('tail.y', [[0.3, e, sin(a, 1.7)]]), ...both('earF', [[-0.5, e]]), ch('look.y', [[-0.2, e]]), ch('head.x', [[0.12, e]])]; })() },
  // a slow cat nod: the eyes close first, the head sinks and comes back, the ears relax outward
  nod: { ch: [ch('_n', [[3, nb, nb], [-2, nb, nb, nb]]), ch('head.x', [[0.26, '@_n']]), ch('chest.x', [[0.04, '@_n']]), ch('lid', [[1.5, bump(a, 0.05, D('nod') - 0.2)]], { op: 'max', cl: [0, 1] }), ...both('earF', [[0.2, '@_n']])] },
  // shaking something off its head: a quick decaying rattle, ears flapping a beat behind, then one last ear flick
  shake_head: { ch: (() => { const A = [bump(a, 0.08, 0.8), bump(a, 0.08, 0.8)], sh = (o, m = '') => [m ? m + ':sin' : 'sin', a, 6.5, -TAU * 6.5 * 0.08 - o];
    return [ch('head.y', [[0.24, ...A, sh(0)]]), ch('head.z', [[0.16, ...A, sh(0.8)]]), ch('earF.l', [[0.7, ...A, sh(1.6, 'pos')], [0.8, bump(a, 0.9, 1.15)]]), ch('earF.r', [[0.7, ...A, sh(1.6, 'neg')]]), ch('lid', [[0.7, ...A]], { op: 'max' })]; })() },
  // a cat does not bounce: it crouches, wiggles its bum, and pounces, front paws out, then lands low
  bounce: { ch: [ch('stand', [[1, ss(a, 0, 0.12), ssd(a, D('bounce') - 0.2, 0.2)]]), ch('crouch', [[1, ...bc], [0.7, bl]]), ch('hips.bend', [[0.36, ...bc, sin(a, 5.5)]]), ch('hips.roll', [[0.1, ...bc, sin(a, 5.5)]]), ch('pos.y', [[0.06, bj]]), ch('pos.z', [[0.2, ss(a, 0.48, 0.3), ssd(a, D('bounce') - 0.35, 0.35)]]),
    ...both('paw', [[0.9, bj]]), ch('squash', [[0.06, bj], [-0.05, bl]]), ...both('earF', [[-0.5, ...bc], [0.3, bl]]), ch('tailTip', [[1, ...bc]]), ch('tail.x', [[-0.3, bj]])] },
  // peeking over an edge: low, only the head up, ears pricked, the eyes sweeping slowly, the tail tip flicking
  peek: { ch: (() => { const e = env(a, D('peek'), 0.35, 0.45), lk = [ss(a, 0.6, 0.3), e], sw = sin('@_ph', 1); return [ch('_ph', [[0.55, a], [-0.33], [0.4 / TAU, nz(a, 1, 5)]]), ch('crouch', [[0.8, e]]), ch('head.x', [[-0.18, e]]), ch('head.y', [[0.2, sw, ...lk]]), ch('look.x', [[0.7, sw, ...lk]]), ch('look.y', [[0.2, e]]), ...both('earF', [[-0.6, e]]), ...both('earT', [[0.45, sw, ...lk]]), ch('tailTip', [[1, e]])]; })() },
  glance_left: glance(-1), glance_right: glance(1),
  // a blink with an ear flick
  blink: { ch: [ch('lid', [[1 / 0.12, clamp(a, 0, 0.12)], [-1 / 0.15, clamp(a, 0.2, 0.35)], [0.2 / 0.15]], { op: 'max' }), ch('earF.l', [[0.4, bump(a, 0.05, 0.3)]]), ch('head.x', [[0.05, bump(a, 0, 0.35)]])] },
  // a whole-body shake: it starts at the head and runs down the chest and hips to the tail, the fur puffing out
  wobble: { ch: [ch('head.z', [[0.24, ...atL(0)]]), ch('head.y', [[0.1, ...atL(0.02)]]), ch('chest.z', [[0.13, ...atL(0.07)]]), ch('hips.roll', [[0.25, ...atL(0.14)]]), ch('tail.y', [[0.7, ...atL(0.22)]]),
    ...both('earF', [[0.6, ex(a, 3, 0.03), ss(a, 0.03, 0.06), ['abs:sin', a, 7, -TAU * 7 * 0.03], ssd(a, D('wobble') - 0.3, 0.3)]]), ch('fluff', [[0.6, ex(a, 2), ssd(a, D('wobble') - 0.3, 0.3)]]), ch('lid', [[0.6, bump(a, 0, 0.6)]], { op: 'max' })] },
  // a downward dog: front paws walk out, the chest sinks, the bum and tail go up, eyes half shut
  stretch: { ch: (() => { const e = env(a, D('stretch'), 0.45, 0.5), qb = bump(a, 0.3, D('stretch') - 0.2); return [ch('dog', [[1, e]]), ch('head.x', [[-0.35, e]]), ch('lid', [[0.65, qb]], { op: 'max' }), ch('mouth', [[0.3, bump(a, 0.7, 1.4)]], { op: 'max' }), ch('tailUp', [[0.8, e]]), ch('tailTip', [[0.4, qb]]), ...both('earF', [[0.35, qb]])]; })() },
  // a bird overhead: it sits up tall, ears pricked, head tipped right back, and the jaw chatters
  look_up: { ch: (() => { const e = env(a, D('look_up'), 0.2, 0.4), c = bump(a, 0.5, 1.2); return [ch('head.x', [[-0.5, e]]), ch('chest.x', [[-0.07, e]]), ch('squash', [[0.06, e]]), ch('look.y', [[0.9, e]]), ...both('earF', [[-0.6, e]]), ch('mouth', [[0.15, c], [0.15, c, sin(a, 7)]], { op: 'max' }), ch('tailTip', [[1, e]])]; })() },
  // a big cat yawn: head back, the jaw drops wide with the tongue curled out, ears flat, eyes squeezed shut; then a lick
  yawn: { ch: (() => { const e = env(a, D('yawn'), 0.5, 0.6), m = bump(a, 0.3, 2.0), lick = bump(a, 2.05, 2.4);
    return [ch('head.x', [[-0.4, e]]), ch('head.z', [[0.08, e]]), ch('chest.x', [[-0.06, e]]), ch('squash', [[0.07, e]]), ch('_m', [[1.3, m]], { cl: [0, 1] }), ch('mouth', [[1, '@_m'], [0.3, lick]], { op: 'max' }),
      ch('_tg', [[1.5, m]], { cl: [0, 1] }), ch('tongue', [[1, '@_tg', ss(a, 0.6, 0.4)], [1, lick]]), ...both('earF', [[0.8, m]]), ch('lid', [[1.4, m]], { op: 'max', cl: [0, 1] })]; })() },
  // batting at something: one paw comes up and swats, uneven, the other paw joins once; the head tracks it down
  // eating from a red bowl that appears in front of it: down into it, lapping in little beats with the ears and tail alive,
  // then up, two licks of the lips, a slow blink, and the bowl goes
  eat: { dur: EAT, ch: (() => { const dn = [ss(a, 0.35, 0.45), ssd(a, 2.15, 0.45)], lap = ['pos:sin', a, 3.1, -1];
    return [ch('bowl', [[1, ss(a, 0, 0.35), ssd(a, EAT - 0.55, 0.45)]]), ch('crouch', [[0.6, ...dn]]), ch('head.x', [[0.85, ...dn], [0.07, ...dn, lap]]), ch('chest.x', [[0.12, ...dn]]),
      ch('look.y', [[-0.7, ...dn], [0.4, bump(a, 3.1, 3.9)]]), ch('lid', [[0.5, ...dn], [0.9, bump(a, 3.85, 4.35)]], { op: 'max' }), ch('mouth', [[0.3, ...dn, lap], [0.35, bump(a, 3.2, 3.45)], [0.35, bump(a, 3.55, 3.8)]], { op: 'max' }),
      ch('tongue', [[0.9, ...dn, lap], [1, bump(a, 3.2, 3.45)], [1, bump(a, 3.55, 3.8)]]), ch('head.y', [[0.05, ...dn, sin(a, 0.7)]]),
      ch('earT.l', [[0.5, dz(0.9, 7, a)]]), ch('earT.r', [[-0.4, dz(0.8, 9, a)]]), ch('earF.r', [[0.45, bump(a, 1.4, 1.65)]]), ch('earF.l', [[0.4, bump(a, 2.2, 2.45)]]),
      ch('tail.y', [[0.35, sin(a, 0.45)]]), ch('tailTip', [[0.6, ...dn]])]; })() },
  paddle: { ch: (() => { const e = env(a, D('paddle'), 0.3, 0.5);
    return [ch('_r', [[1, pr(0.25, 0.7)], [1, pr(0.95, 1.3)], [1, pr(1.95, 2.45)], [0.7, pr(2.85, 3.25)]]), ch('_l', [[0.8, pr(1.4, 1.85)]]), ch('paw.l', [[0.95, '@_l']]), ch('paw.r', [[0.95, '@_r']]), ch('swat.l', [[-0.5, '@_l']]), ch('swat.r', [[0.5, '@_r']]),
      ch('head.x', [[0.18, e]]), ch('head.y', [[0.12, '@_l'], [-0.12, '@_r']]), ch('look.x', [[0.35, '@_l'], [-0.35, '@_r']]), ch('look.y', [[-0.45, e]]), ch('chest.x', [[0.05, e]]), ch('chest.z', [[0.07, '@_r'], [-0.07, '@_l']]), ...both('earF', [[-0.45, e]]), ch('tailTip', [[1, e]])]; })() },
};

// moods (cat time is the life clock, t)
const pulse = (hz, k, w, src = T) => ['pulse', src, hz, k, w];
const moods = {
  // idle: the ears swivel on their own now and then, the tail tip curls, and once in a while a slow blink
  idle: { ch: [ch('earT.l', [[0.6, dz(0.35, 11)]]), ch('earT.r', [[0.6, dz(0.33, 23)]]), ch('tailTip', [[0.5, ['pos:nz', T, 0.5, 31]]]), ch('tailWrap', [[0.35]]), ch('lid', [[0.85, pulse(0.12, 41, 0.12)]], { op: 'max' })] },
  // listening: both ears prick and swivel to you, it goes still, only the tail tip twitches
  listening: { ch: [...both('earF', [[-0.7]]), ch('earT.l', [[-0.2], [0.4, ['pos:dz', T, 0.4, 13]]]), ch('earT.r', [[0.2]]), ch('tailTip', [[0.8, pulse(0.5, 17, 0.25)]]), ch('tailWrap', [[0.3]])] },
  // thinking: the tail sweeps slowly, one ear turned back, eyes half lidded
  thinking: { ch: [ch('tail.y', [[0.5, nz(T, 0.45, 51)]]), ch('earT.l', [[0.4, dz(0.3, 53)]]), ch('earT.r', [[0.3]]), ch('earF.r', [[0.35]]), ch('lid', [[0.25]], { op: 'max' })] },
  // speaking: a meow moves the ears too (the chin lift is in post)
  speaking: { ch: [ch('earF.l', [[0.15, nz(T, 2, 61)]]), ch('earF.r', [[0.15, nz(T, 2, 67)]]), ch('tail.y', [[0.2, nz(T, 0.8, 71)]])] },
  // happy: slow blinks, tail straight up with a quivering tip, kneading paws in turn
  happy: { ch: [ch('lid', [[0.45], [0.5, pulse(0.3, 81, 0.3)]], { op: 'max' }), ch('tailUp', [[1]]), ch('tailTip', [[1]]), ch('_t36', [[1, T], [0.36]]), ch('paw.l', [[0.35, pulse(1.4, 83, 0.45)]]), ch('paw.r', [[0.35, pulse(1.4, 83, 0.45, '@_t36')]]), ...both('earF', [[-0.2]]), ch('head.z', [[0.06, nz(T, 0.4, 87)]])] },
  // sleepy: down into a loaf, paws tucked away, the tail wrapped round, the chin on its chest
  sleepy: { ch: [ch('loaf', [[1]]), ch('tailWrap', [[1]]), ...both('earF', [[0.15]]), ch('lid', [[0.92]], { op: 'max' }), ch('head.x', [[0.15]]), ch('tailTip', [[0.3, pulse(0.2, 91, 0.2)]])] },
  // surprised: the halloween cat: back arched high, every hair up, tail a bottle brush, ears flat
  surprised: { ch: [ch('arch', [[1]]), ch('fluff', [[1]]), ch('tailUp', [[0.9]]), ch('tailTip', [[0.4]]), ...both('earF', [[1]]), ch('pos.x', [[0.03, nz(T, 1.5, 97)]])] },
};

// the last word on the pose
const W = (m) => '@w.' + m;
const pose = [
  // no hopping for joy: the shared happy bounce comes out; each meow lifts the chin and folds the ears back a touch
  ch('pos.y', [[-0.9, W('happy')]], { op: '*' }), ch('squash', [[-0.8, W('happy')]], { op: '*' }),
  ch('head.x', [[-0.22, W('speaking'), '@mouth']]), ...both('earF', [[0.25, W('speaking'), '@mouth']]),
  // on four feet for a loaf, a crouch, an arch or a stretch; sitting otherwise
  ...['stand', 'loaf', 'crouch', 'arch', 'dog'].map((k) => ch('prowl', [[1, '@' + k]], { op: 'max' })),
  ch('pos.y', [[-0.16, '@loaf'], [-0.07, '@crouch'], [0.04, '@arch'], [-0.03, '@dog']]), ch('body', [[0.12, '@crouch'], [-0.14, '@arch'], [0.35, '@dog']]),
  // sitting, the hips stay planted and the chest sways over them; listening or sleepy it goes still. A sitting spine is never a post:
  // it settles forward and back, side to side, and now and then shifts its weight
  ch('_seat', [[1], [-1, '@prowl']]), ch('_still', [[1, lin(W('listening'), 1, -0.6), lin(W('sleepy'), 1, -0.6)]]),
  ch('chest.x', [[1, '@rot.x', '@_seat', '@_still'], [0.045, '@_seat', '@_still', nz(T, 0.23, 101)]]),
  ch('_shs', [[1], [-2, ['chance', T, 1 / 0.13, 5, 0.5]]]), ch('chest.z', [[1, '@rot.z', '@_seat', '@_still'], [0.05, '@_seat', '@_still', nz(T, 0.19, 103)], [0.06, '@_seat', '@_still', ['pulse', T, 0.13, 107, 0.2], '@_shs']]),
  ch('rot.x', [[-1, '@_seat']], { op: '*' }), ch('rot.z', [[-1, '@_seat']], { op: '*' }),
  // the tail held up or wrapped round takes over from its swing, most at the base, least at the tip
  ch('_up', [[1, '@tailUp']], { cl: [0, 1] }), ch('_wrap', [[1, '@tailWrap', lin('@_up', 1, -1)]], { cl: [0, 1] }), ch('_hold', [[1, '@_up'], [1, '@_wrap']]),
];
// the chain: what it is told (a gesture, the walk, a startle) each joint plays a beat after the one before, so the base leads and the
// tip follows through. Its own lazy swish is a wave running down it, each joint a quarter beat behind, and swings more toward the tip,
// so it bends into a slow S. Each joint turns across its own segment (the base lies along the floor, the rest rise), never about it.
// [bone, delay s, swish ry, swish rz, lift rz, lift rx, how much a held pose stills it]
const CHAINED = [['tail', 0, 0.8, 0, 0.7, 0, 0.9], ['tail2', 0.15, 0.3, 0.6, 0.3, 0.3, 0.75], ['tail3', 0.3, 0, 0.8, 0, 0.45, 0.45], ['tail4', 0.45, 0, 0.7, 0, 0.5, 0.15]];
const swing = CHAINED.flatMap(([b, d, sy, sz, lz, lx, h], i) => { const at = (c) => (d ? ['dl', c, d] : c), wave = sin('@_lph', 1, +(-1.3 * i).toFixed(2));
  const held = (g, ...x) => (g ? [[g, ...x], [-g * h, ...x, '@_hold']] : []), sw = (g) => [...held(g, at('@_sw')), ...held(g, '@_lz', wave)];
  return [ch(b + ':ry', sw(sy)), ch(b + ':rz', [...sw(sz), ...held(lz, at('@_lf'))]), ch(b + ':rx', held(lx, at('@_lf')))].filter((c) => c[1].length); });
const tails = ['tail', 'tail2', 'tail3', 'tail4'], UP = [[1, 0.45, -0.4], [0.2, 1, -0.1], [-0.1, 0.8, 0.5]], WRAP = [[0.5, -0.7, -0.6], [1, -0.2, 0.2], [0.3, -0.1, 1]];
// held tails are solved at build time for the cat sitting, standing (prowl) and arched, and blended by those channels
const aimed = (dirs, k) => { const sit = aimChain(body.bones, tails, dirs), up = aimChain(body.bones, tails, dirs, { torso: Q.pitch }), arch = aimChain(body.bones, tails, dirs, { torso: Q.pitch - 0.14, hips: -0.55 });
  return sit.flatMap((e, i) => ['rx', 'ry', 'rz'].map((p, j) => ch(`${tails[i]}:${p}`, [[e[j], k], [up[i][j] - e[j], k, '@prowl'], [arch[i][j] - up[i][j], k, '@arch']]))); };
const rig = [
  // the chest turns about the hip joint and everything below is turned back, so the hips and front paws stay put
  ch('torso:rx', [[1, '@chest.x']]), ch('torso:rz', [[1, '@chest.z']]), ch('torso:px', [[-0.12, '@chest.z']]), ch('torso:pz', [[0.12, '@chest.x']]),
  ...['hips', 'legFL', 'legFR'].flatMap((b) => [ch(b + ':rx', [[-1, '@chest.x']]), ch(b + ':rz', [[-1, '@chest.z']])]), ch('head:rx', [[-0.5, '@chest.x']]), ch('head:rz', [[-0.6, '@chest.z']]),
  // ears: flatten out and back, prick forward, swivel
  ...[['earL', 1, 'l'], ['earR', -1, 'r']].flatMap(([n, d, s]) => [ch(n + ':rx', [[-0.7, pos('@earF.' + s)], [0.4, neg('@earF.' + s)]]), ch(n + ':rz', [[-d, pos('@earF.' + s)]]), ch(n + ':ry', [[0.9, '@earT.' + s]])]),
  // the legs: a raised paw comes up and out, curled at the wrist; a loaf folds them all under, a crouch halfway; a stretch reaches the front ones out; an arch stands tall
  ch('_fold', [[1, '@loaf'], [0.5, '@crouch']]),
  ...[['legFL', 'l', 1], ['legFR', 'r', -1]].flatMap(([n, s, d]) => [ch(n + ':rx', [[-0.9, '@paw.' + s], [1.2, '@_fold'], [-0.75, '@dog']]), ch(n + ':rz', [[d * 0.8, '@paw.' + s], [1, '@swat.' + s]]), ch('shin' + n.slice(3) + ':rx', [[0.9, '@paw.' + s], [-2.4, '@_fold']])]),
  ...['legBL', 'legBR'].flatMap((n) => [ch(n + ':rx', [[-1, '@_fold']]), ch('shin' + n.slice(3) + ':rx', [[2.1, '@_fold']]), ch(n + ':sy', [[0.18, '@arch'], [0.1, '@dog']])]),
  ch('hips:rx', [[-0.55, '@arch']]), ch('torso:s', [[0.07, '@fluff']]), ch('tail:s', [[0.4, '@fluff']]),
  // the tail: straight up with a hooked tip, or wrapped round the paws on the floor (solved at build time, sitting and standing)
  ...aimed(UP, '@_up'), ...aimed(WRAP, '@_wrap'),
  // what the tail is told: a swish and a lift at its base, kept for a second so each joint can play it back late; startled, it lashes.
  // Its own swish is slow and lazy, gone when it listens or sleeps; the tip wanders and flicks on its own
  ch('_sw', [[1, '@tail.y'], [0.3, W('surprised'), sin(T, 1.3)], [-0.08, '@_spin']], { hist: 1 }), ch('_lf', [[1, '@tail.x'], [0.25, W('sleepy')], [0.6, '@_lz', nz(T, 0.2, 119)]], { hist: 1 }),
  ch('_lph', [[0.3], [0.12, nz(T, 0.15, 111)]], { int: 1 }), ch('_lz', [[0.32, lin(W('listening'), 1, -0.8), lin(W('sleepy'), 1, -0.9)]]),
  ch('_tl', [[1, lin(W('sleepy'), 1, -0.7)]]), ch('tail4:rz', [[0.22, '@_tl', nz(T, 0.4, 113)], [0.45, '@_tl', '@_fk']]), ch('tail3:rz', [[0.1, '@_tl', nz(T, 0.3, 117)], [0.25, '@_tl', '@_fk']]),
  ...swing,
  ch('_qph', [[4.3, T], [2 / TAU, nz(T, 1, 7)]]), ch('tail4:rz', [[0.35, '@tailTip', sin('@_qph', 1)]]), ch('tail3:rz', [[0.12, '@tailTip', sin(T, 4.3, -0.8)]]),
  ch('tongue:s', [[1, '@tongue']], { op: '=', cl: [0.001, 1] }), ch('bowl:s', [[1, '@bowl']], { op: '=', cl: [0.001, 1] }), ch('tongue:pz', [[0.012, '@tongue']]), ch('tongue:py', [[-0.01, '@tongue']]),
];

const fid = alive(ID, { fidget: 0.8 }), live = living(body, { limbs: { hop: 0.15, squash: 0.2 }, quad: Q, motions: [breathe('torso', 'py', 0.012, 0.5), blinkBurst(2)] });
// the shared tail swing turns every joint about the same two axes, which for this curled tail twists the upper joints about themselves
// and leaves the base to swing the whole curve like a stick; the cat's own chain (above) replaces it
const shared = /^(_wag|_wa|_tx|_ty|tail\d*:r[xy])$/, lived = live.post.filter((c) => !shared.test(c[0]));
// about: what a person sees on the round screen, for the agent that speaks as this Molty (docs/CREATURES.md section 1)
const about = {
  look: 'A slim lavender cat sitting up straight, with pointy ears pink inside, green eyes, a pink nose and whiskers. A white patch on its chest, and a long darker purple tail curls up behind it.',
  gestures: {
    wiggle: 'crouches low and shimmies its hips as if about to pounce, the tail tip twitching',
    nod: 'a slow cat nod: the eyes close, the head sinks and comes back up',
    shake_head: 'rattles its head quickly, the ears flapping, then one last ear flick',
    bounce: 'crouches, wiggles its bum and pounces with its front paws out, landing low',
    peek: 'goes low with only its head up, ears pricked, the eyes sweeping slowly',
    glance_left: 'its ears turn to its left first, then the eyes and the head follow',
    glance_right: 'its ears turn to its right first, then the eyes and the head follow',
    blink: 'a blink with a little ear flick',
    wobble: 'a whole-body shake from the head down to the tail, the fur puffing out',
    stretch: 'a downward dog: front paws out, chest down, bum and tail up',
    look_up: 'sits up tall with its head tipped back at a bird overhead, the jaw chattering',
    yawn: 'a big yawn, the jaw wide with the tongue curled, ears flat, then a lick',
    paddle: 'bats at something with one paw, the other paw joining once',
    eat: 'a red bowl of milk appears; it laps from it, ears and tail moving, then licks its lips',
  },
  moods: {
    idle: 'sits with its chest swaying a little, ears swiveling, tail tip curling, slow blinks',
    listening: 'both ears prick toward you and it goes still, only the tail tip twitching',
    thinking: 'eyes half shut, an ear back, the tail sweeping; now and then a glance, an ear flick, a tilt',
    speaking: 'the chin lifts with each meow and the ears move a little',
    happy: 'slow blinks, the tail straight up with a quivering tip, kneading with its paws',
    sleepy: 'curls down into a loaf, paws tucked, the tail wrapped round, chin down',
    surprised: 'the back arched high, fur standing up, the tail a bottle brush, ears flat',
  },
};
const pkg = { v: 'molty-pkg/1', id: ID, name: 'Cat', version: '1.0.0', kind: 'creature',
  creature: { about, body: { palette, ...body }, walk, groups: live.groups, events: { ...fid.events, ...live.events, ponder: ponder({
    // the cat's own: an ear flicks back, an ear swivels to a sound, the tail swishes once
    ear: { len: 0.9, ch: [ch('earF.l', [[0.6, bump(a, 0, 0.3)], [0.4, bump(a, 0.45, 0.75)]])] },
    swivel: { len: 1.6, ch: [ch('earT.r', [[0.8, bump(a, 0, 1.6)]]), ch('earT.l', [[0.4, bump(a, 0.15, 1.5)]])] },
    swish: { len: 1.6, ch: [ch('tail.y', [[0.5, sgn(0), bump(a, 0, 1.6)]]), ch('tailTip', [[0.8, bump(a, 0, 1.6)]])] } }) }, base: fid.base, gestures, moods, post: [...pose, ...lived, ...rig] } };
const out = writePackage(new URL(`./${ID}.json`, import.meta.url).pathname, pkg);
console.log(ID, out.size, 'bytes', out.sha256);
