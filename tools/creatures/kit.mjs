// Build helpers for creature packages (docs/CREATURES.md). They only write data: a package's build.mjs uses them to lay out
// its body and to emit the layers every Molty shares (the living layer, the fidgets, the eyes), so each package carries
// all of its own behaviour and the player knows none of it.
import { writeFileSync, mkdirSync, readFileSync, existsSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { dirname } from 'node:path';
import { indexEntry } from '../../packages/tools/index.mjs';

// ---- terms: a channel is [name, terms, options]; a term is [gain, ...factors]; a factor is [op, source, ...params] or '@channel'
export const ch = (name, terms, o) => (o ? [name, terms, o] : [name, terms]);
const trim = (a) => { while (a.length > 2 && a[a.length - 1] === 0) a.pop(); return a; };
export const sin = (s, hz, ph = 0) => trim(['sin', s, hz, ph]);
export const cos = (s, hz) => ['sin', s, hz, Math.PI / 2];
export const nz = (s, hz, k, off = 0) => trim(['nz', s, hz, k, off]);
export const env = (s, d, a, r) => ['env', s, d, a, r];
export const bump = (s, a, b) => ['bump', s, a, b];
export const ss = (s, a, len) => ['ss', s, a, len];
export const ssd = (s, a, len) => ['ssd', s, a, len];
export const ex = (s, k, at = 0) => trim(['exp', s, k, at]);
export const lin = (s, a, b) => ['lin', s, a, b]; // a + b * s
export const lt = (s, a) => ['lt', s, a];
export const gt = (s, a) => ['gt', s, a];
export const clamp = (s, a, b) => ['clamp', s, a, b];
export const pos = (s) => ['pos:v', s];
export const neg = (s) => ['neg:v', s];
export const abs = (s) => ['abs:v', s];
export const snap = (s, at, len = 0.09) => ss(s, at, len); // a bird's head: quick to a new place, then still
export const sgn = (draw) => ['sgn', '~' + draw]; // a random side, drawn once per play
export const rnd = (draw, lo = 0, hi = 1) => (lo === 0 && hi === 1 ? '~' + draw : ['lin', '~' + draw, lo, hi - lo]);
// the lab's hash: per-instance constants (an arm's own speed) are fixed at build time with it, so they match the code version
export const labHash = (n) => { const x = Math.sin(n * 127.1 + 311.7) * 43758.5453; return x - Math.floor(x); };
export const LR = (fn) => [fn(1, 'L'), fn(-1, 'R')].flat();

// ---- bodies: the lab's v2 rig (bones and parts), unchanged
export const E = (bone, size, pos = [0, 0, 0], color = 'body', rot, x = {}) => ({ type: 'ellipsoid', bone, size, pos, color, ...(rot ? { rot } : {}), seg: [14, 10], ...x });
export const B = (bone, size, pos = [0, 0, 0], color = 'body', rot) => ({ type: 'box', bone, size, pos, color, ...(rot ? { rot } : {}) });
export const C = (bone, r, h, pos = [0, 0, 0], color = 'body', rot, seg = 6) => ({ type: 'cone', bone, r, h, pos, color, seg, ...(rot ? { rot } : {}) });
export const CY = (bone, r, h, pos = [0, 0, 0], color = 'body', rot, r2) => ({ type: 'cylinder', bone, r, h, pos, color, ...(r2 != null ? { r2 } : {}), ...(rot ? { rot } : {}) });
const TUBE = (bone, path, r, color = 'body') => ({ type: 'tube', bone, path, r, color, seg: 24 });
export const P = (p) => ({ ...p, plain: true });
const cr = (Q, u) => { const i = Math.min(Q.length - 2, Math.floor(u)), f = u - i, a = Q[Math.max(0, i - 1)], b = Q[i], c = Q[i + 1], d = Q[Math.min(Q.length - 1, i + 2)]; return b.map((_, k) => +(0.5 * (2 * b[k] + (c[k] - a[k]) * f + (2 * a[k] - 5 * b[k] + 4 * c[k] - d[k]) * f * f + (3 * b[k] - a[k] - 3 * c[k] + d[k]) * f * f * f)).toFixed(3)); };
/** A tail or an arm in joints, each segment its own bone, so a swish runs down it. path starts at [0, 0, 0]. */
export const CHAIN = (ids, parent, at, path, r, color = 'body', x = () => ({})) => { const n = ids.length, u = (j) => j * (path.length - 1) / n, J = Array.from({ length: n + 1 }, (_, j) => cr(path, u(j))), rel = (p, o) => p.map((v, k) => +(v - o[k]).toFixed(3));
  return { bones: J.slice(0, n).map((p, j) => ({ id: ids[j], parent: j ? ids[j - 1] : parent, pos: j ? rel(p, J[j - 1]) : at, ...x(j) })), parts: J.slice(0, n).map((p, j) => ({ ...TUBE(ids[j], [rel(p, p), rel(cr(path, u(j + 0.5)), p), rel(J[j + 1], p)], +(r * (1 - 0.3 * j / n)).toFixed(4), color), seg: 8 })) }; };
export const TAIL = (parent, at, path, r, color) => CHAIN(['tail', 'tail2', 'tail3', 'tail4'], parent, at, path, r, color);
/** An eye pair on its own bones, so any kind can blink. */
export function eyes(head, kind, x, y, z, s = 1) {
  const bones = [], parts = [];
  for (const [d, n] of [[1, 'L'], [-1, 'R']]) {
    const id = `eye${n}`; bones.push({ id, parent: head, pos: [d * x, y, z] });
    if (kind === 'glossy') parts.push(P(E(id, [0.078 * s, 0.1 * s, 0.028], [0, 0, 0], 'eye')), P(E(id, [0.024 * s, 0.03 * s, 0.012], [0.02 * s, 0.03 * s, 0.024], 'shine')));
    else if (kind === 'slit') parts.push(P(E(id, [0.085 * s, 0.075 * s, 0.024], [0, 0, 0], 'iris')), P(E(id, [0.016 * s, 0.07 * s, 0.014], [0, 0, 0.02], 'eye')));
  }
  return { bones, parts };
}
export const mouth = (y, z, w = 0.07) => ({ bones: [{ id: 'mouth', parent: 'head', pos: [0, y, z] }], parts: [P(E('mouth', [w, 0.05, 0.012], [0, 0, 0], 'maw'))] });
export const merge = (...a) => ({ bones: a.flatMap((x) => x.bones ?? []), parts: a.flatMap((x) => x.parts ?? []) });

// ---- the layers every Molty shares, as data

/** The fidgets (the lab's `alive`): weight from foot to foot, uneven breaths, glances it chooses, a tail swish, a shake. */
export function alive(id, { fidget = 1, shake = 0 } = {}) {
  const F = fidget, pace = Math.sqrt(F), seed = [...id].reduce((a, c) => a + c.charCodeAt(0) * 7.3, 0);
  const two = (g, hz, k, ...f) => [[g / 1.5, ...f, nz('@t', hz * pace, seed + k * 31)], [g * 0.5 / 1.5, ...f, nz('@t', hz * pace * 2.3, seed + k * 31 + 13)]];
  const still = lin('@walk.k', 1, -0.7), g = ['@_free', lin('@walk.k', 1, -0.6)];
  const base = [
    ch('_free', [[1], [-0.7, '@is.sleepy'], [-0.55, '@is.listening'], [-0.55, '@is.speaking']], { lp: 3 }),
    ch('rot.z', two(0.04 * F, 0.21, 1, '@_free', still)), ch('rot.x', two(0.025 * F, 0.17, 2, '@_free', still)), ch('pos.x', two(0.012 * F, 0.13, 3, '@_free', still)),
    ch('_br', [[0.27, lin('@is.sleepy', 1, -0.4)], ...two(0.07, 0.09, 4, lin('@is.sleepy', 1, -0.4))], { int: 1 }),
    ch('squash', [[0.016, sin('@_br', 1)], ...two(0.016 * 0.6, 0.15, 5, sin('@_br', 1))]),
    ch('_gl.x', [[1, '@_glt.x']], { lp: 10 * pace }), ch('_gl.y', [[1, '@_glt.y']], { lp: 10 * pace }),
    ch('head.y', [[F, ...g, '@_gl.x'], ...two(0.06 * F, 0.37, 6, ...g)]), ch('head.x', [[F, ...g, '@_gl.y'], ...two(0.04 * F, 0.31, 7, ...g)]), ch('head.z', two(0.04 * F, 0.26, 8, ...g)),
    ch('look.x', [[1.3 * F, ...g, '@_gl.x']]), ch('look.y', [[-F, ...g, '@_gl.y']]),
    ch('tail.x', two(0.12 * F, 0.2, 9, '@_free')), ch('tail.y', [...two(0.2 * F, 0.23, 10, '@_free'), [0.45 * F, '@_free', '@_swish']]),
  ];
  const events = {
    glance: { first: 1, gap: 0, len: [1.2 / pace, 5.2 / pace], ch: [ch('_glt.x', [[0.7, lin('~1', -0.5, 1), gt('~0', 0.4)]]), ch('_glt.y', [[0.3, lin('~2', -0.5, 1), gt('~0', 0.4)]])] },
    swish: { first: 2, gap: [2.5 / pace - 0.9, 8.5 / pace - 0.9], len: 0.9, ch: [ch('_swish', [[1, bump('a', 0, 0.9), sin('a', 2 / 0.9)]])] },
  };
  if (shake) events.shake = { first: 6, gap: [9, 21], len: 0.75, gate: [[1, lt('@walk.k', 0.2)]],
    ch: [ch('rot.z', [[0.13 * shake, bump('a', 0, 0.75), sin('a', 8)]]), ch('head.z', [[-0.2 * shake, bump('a', 0, 0.75), sin('a', 8, -1)]]), ch('squash', [[0.05 * shake, bump('a', 0, 0.75)]])] };
  return { base, events };
}

/**
 * The living layer (the lab's molty2-runtime): the shared pose onto the bones, by the bones' names. Root, torso and head take the
 * pose; wings spread, ears droop and twitch, a tail wags with each joint a beat behind, legs sit or stand, arms ripple, feet lift,
 * eyes look and blink, the mouth opens. Options: limbs (strengths), gait (a float), quad (stands on four legs), sides (wings),
 * tailFree (a channel that fades the tail's swing on the joints a pose of the tail takes over).
 */
export function living(body, { limbs = {}, gait = {}, quad, sides = true, tailFree, motions = [] } = {}) {
  const A = { side: 1, ear: 1, tail: 1, foot: 1, wag: 1, hop: 1, squash: 1, ...limbs }, ids = body.bones.map((b) => b.id), has = (id) => ids.includes(id), by = (re) => body.bones.filter((b) => re.test(b.id));
  const post = [], events = {}, groups = {}, w = (m) => '@w6.' + m;
  if (gait.type === 'float') { const k = gait.amp ?? 1, r = gait.rate ?? 1;
    post.push(ch('_lift', [[gait.height ?? 0.08, lin(w('sleepy'), 1, -0.6)]], { lp: 6 }), ch('_Lpy', [[0.03 * k, sin('@t', r / 2.6)]]), ch('_Lrz', [[0.045 * k, sin('@t', r / 3.4)]]), ch('_Lrx', [[0.03 * k, sin('@t', r / 4.1)]])); }
  events.tilt = { first: 2.5, gap: [3.5, 7], len: 1.5, ch: [ch('_Lhz', [[0.2, sgn(0), bump('a', 0, 1.5), '@w6.idle'], [0.12, sgn(0), bump('a', 0, 1.5), w('listening')]])] };
  const hop = 1 - A.hop, plant = gait.type !== 'float', over = plant && !quad && has('torso') ? 'torso' : null;
  const moved = (a) => (plant ? [[1, '@pos.' + a, '@walk.k']] : [[1, '@pos.' + a]]);
  post.push(ch('_py', hop ? [[1, '@pos.y'], [-hop, pos('@pos.y')]] : [[1, '@pos.y']]), ...(hop ? [ch('_spare', [[hop, pos('@pos.y')]])] : []), ch('_Lpy', [[0.35, w('happy'), '@_py']]), ch('_ry', [[1, '@_py'], [1, '@_Lpy']]),
    ch('root:px', moved('x')), ch('root:py', [[1, '@_ry'], [1, '@_lift']]), ch('root:pz', moved('z')),
    ch('root:rx', [[1, '@rot.x'], [1, '@_Lrx']]), ch('root:ry', [[1, '@rot.y']]), ch('root:rz', [[1, '@rot.z'], [1, '@_Lrz']]),
    ch('_sq', [[1.25 * A.squash, '@squash']]), ch('_spin', [[1, '@rot.y']], { d: [-8, 8], lp: 8 }));
  // the feet stay planted: only walking carries the body across the floor. Standing, the sway moves the body over its feet
  // (a quad's legs hang from it, so it stays put); a float drifts as it likes
  if (over) post.push(...['x', 'z'].map((a) => ch(`${over}:p${a}`, [[1, '@pos.' + a, lin('@walk.k', 1, -1)]])));
  if (has('torso')) post.push(ch('torso:sq', [[1, '@_sq']]), ch('torso:rx', [[0.5, '@spine.x'], [0.3, '@head.x']]), ch('torso:ry', [[0.5, '@spine.y']]), ch('torso:rz', [[0.5, '@spine.z']]));
  if (has('head')) post.push(ch('head:sq', [[0.5, '@_sq']]), ch('head:sq', [[-1, '@_sq', ['rcp', '@_sq', 1, 1]]]), ch('head:rx', [[0.45, '@head.x'], ...(hop ? [[-1.5, '@_spare']] : [])]), ch('head:ry', [[1, '@head.y']]), ch('head:rz', [[1, '@head.z'], [1, '@_Lhz']]));
  const ears = by(/^(ear|gill)[LR]$/), wings = sides ? by(/^(wing|fin)[LR]$/) : [];
  if (ears.length) post.push(ch('_lagZ', [[1, '@head.z'], [1, '@_Lhz'], [1, '@rot.z'], [1, '@_Lrz']], { sp: [140, 7], lag: 1, cl: [-0.6, 0.6] }), ch('_lagX', [[1, '@head.x']], { sp: [140, 7], lag: 1, cl: [-0.6, 0.6] }),
    ch('_droop', [[0.6 * A.ear, w('sleepy')], [-0.18 * A.ear, w('listening')], [-0.2 * A.ear, w('surprised')], [0.14 * A.ear, w('happy'), sin('@t', 4)]]));
  for (const e of ears) { const s = Math.sign(e.pos[0]) || 1, n = e.id;
    events['tw' + n] = { first: 1 + 4 * labHash(s * 3.3 + ids.length + 1), gap: [1.5, 7.5], len: 0.34, ch: [ch('_tw' + n, [[1, bump('a', 0, 0.16)], [-0.4, bump('a', 0.16, 0.34)]])] };
    post.push(ch(n + ':rx', [[-0.25 * A.ear, w('listening')], [0.8 * A.ear, '@_lagX'], [0.3 * A.ear, '@_tw' + n]]), ch(n + ':rz', [[-s, '@_droop'], [-s * 0.25 * A.ear, '@_tw' + n], [1.3 * A.ear, '@_lagZ']])); }
  for (const g of wings) { const s = Math.sign(g.pos[0]) || 1;
    post.push(ch(g.id + ':rx', [[-0.15 * A.side, w('happy'), sin('@t', 7, 1)]]), ch(g.id + ':rz', [[0.55 * s * A.side, w('happy')], [0.5 * s * A.side, w('happy'), sin('@t', 7)], [0.95 * s * A.side, w('surprised')], [0.18 * s * A.side, w('speaking')], [0.16 * s * A.side, w('speaking'), sin('@t', 1.4)], [0.12 * s * A.side, w('listening')], [-0.12 * s * A.side, w('sleepy')]])); }
  // standing up: the body tips over the legs, the head stays level
  if (quad && has('torso') && has('head')) post.push(ch('_bp', [[quad.pitch, '@prowl'], [1, '@body']]), ch('torso:rx', [[1, '@_bp']]), ch('head:rx', [[-0.9, '@_bp']]), ch('torso:py', [[quad.rise, '@prowl']]), ch('head:pz', [[-quad.neck, '@prowl']]));
  // standing, the tipped body swings the hips back and the shoulders forward: it moves back by as much, so the paws keep their centre
  if (quad && has('torso')) { const legs = by(/^leg[FB][LR]$/), base = (b) => (b.parent === 'hips' ? body.bones.find((h) => h.id === 'hips').pos : [0, 0, 0]);
    const dz = legs.reduce((d, b) => { const o = base(b), y = o[1] + b.pos[1], z = o[2] + b.pos[2]; return d + y * Math.sin(quad.pitch) + z * Math.cos(quad.pitch) - (o[2] + b.sit[2]); }, 0) / legs.length;
    if (legs.length) post.push(ch('root:pz', [[+(-dz).toFixed(4), '@prowl']])); }
  if (has('hips')) post.push(ch('_hb', [[1, '@hips.bend'], [-0.06, '@_spin']], { cl: [-0.5, 0.5] }), ch('hips:ry', [[1, '@_hb', cos('@_bp', 1 / (2 * Math.PI))], [1, '@hips.roll', sin('@_bp', 1 / (2 * Math.PI))]]), ch('hips:rz', [[-1, '@_hb', sin('@_bp', 1 / (2 * Math.PI))], [1, '@hips.roll', cos('@_bp', 1 / (2 * Math.PI))]]));
  for (const b of by(/^leg[FB][LR]$/)) { const k = b.id.slice(3), back = k[0] === 'B', up = `@legs.${k}.up`, sit = b.sit, fold = b.fold ?? 1;
    for (const [a, i] of [['px', 0], ['py', 1], ['pz', 2]]) { const d = sit[i] - b.pos[i]; if (Math.abs(d) > 1e-4) post.push(ch(`${b.id}:${a}`, [[d], [-d, '@prowl']])); }
    if (fold !== 1) post.push(ch(b.id + ':sy', [[fold - 1], [1 - fold, '@prowl']]));
    post.push(ch(b.id + ':sy', [[1, neg(up)]]), ch(b.id + ':rx', [[-(quad?.pitch ?? 0), '@prowl'], [1, `@legs.${k}.sw`], [-(back ? 0.45 : 0.2) * 5, pos(up)]]), ch('shin' + k + ':rx', [[5, pos(up)]])); }
  // the tail: its base swings, and each joint after it repeats that swing a beat later
  const tails = by(/^tail\d*$/).sort((a, b) => a.id.length - b.id.length || (a.id < b.id ? -1 : 1));
  if (tails.length) {
    events.flick = { first: 2, gap: [2.5, 6], len: 0.28, ch: [ch('_fk', [[1, bump('a', 0, 0.28)]])] };
    post.push(ch('_wag', [[1.1 * A.wag], [3 * A.wag, w('happy')], [A.wag, w('speaking')], [-0.7 * A.wag, w('sleepy')]], { int: 1 }), ch('_wa', [[0.18], [0.3, w('happy')], [-0.14, w('sleepy')]]),
      ch('_tx', [[-0.55 * A.tail, '@_fk', lin(w('sleepy'), 1, -1)], [0.25 * A.tail, w('sleepy')], [A.tail, '@tail.x'], ...(hop ? [[-5, '@_spare']] : [])], { hist: 0.6 }),
      ch('_ty', [[A.tail, '@_wa', sin('@_wag', 1)], [A.tail, '@tail.y'], [-0.08, '@_spin']], { hist: 0.6 }));
    tails.forEach((g, i) => { const k = i ? 0.7 : 1, x = i ? ['dl', '@_tx', +(0.12 * i).toFixed(2)] : '@_tx', y = i ? ['dl', '@_ty', +(0.12 * i).toFixed(2)] : '@_ty'; const f = tailFree && i < tails.length - 1 ? [tailFree] : []; post.push(ch(g.id + ':rx', [[k, x, ...f]]), ch(g.id + ':ry', [[k, y, ...f]])); }); }
  // arms that are never in step: each its own speed and curl, a ripple running out to the tip, quicker when happy, limp when sleepy
  const waves = body.bones.filter((b) => b.wave);
  if (waves.length) { const at = (f) => waves.map((b) => +f(...b.wave).toFixed(4)), hz = (i) => 0.45 + 0.35 * labHash(i * 3.7);
    groups.waves = { bones: waves.map((b) => b.id), attr: { ax: at((ax) => ax), az: at((_, az) => az), hz: at((_, __, j, i) => hz(i)), hz7: at((_, __, j, i) => 0.7 * hz(i)), ph: at((_, __, j, i) => -1.1 * j + 2.3 * i),
      kk: at((_, __, j, i) => i * 11.1), off: at((_, __, j) => 0.3 * j), bias: at((_, __, j, i) => 0.06 * (labHash(i * 9.1) - 0.5)), jk: at((_, __, j) => 0.5 + 0.35 * j), jon: at((_, __, j) => (j ? 1 : 0)), yk: at((_, __, j) => 0.14 * (0.5 + 0.3 * j)), yph: at((_, __, j, i) => 1.7 * i - 0.8 * j) } };
    // pace runs the ripple's clock: quicker when happy, slower when sleepy
    post.push(ch('_pt', [[1, '@t'], [0.8, w('happy'), '@t'], [0.3, w('speaking'), '@t'], [-0.55, w('sleepy'), '@t']]), ch('_gp', [[1, pos('@gather')]]),
      { each: 'waves', ch: [ch('%c0', [[0.22, ['sin', '@_pt', '$hz', '$ph']], [0.12, ['nz', '@t', 0.35, '$kk', '$off']], ['$bias']]),
        ch('%c', [[1, '@%c0', '$jk', lin('@_gp', 1, -0.6)], [-0.18, '$jon', w('sleepy')], [-0.25, '$jon', '@_gp']]),
        ch(':rx', [['$ax', '@%c']]), ch(':rz', [['$az', '@%c']]), ch(':ry', [['$yk', ['sin', '@_pt', '$hz7', '$yph']]])] }); }
  for (const b of by(/^arm\d$/)) post.push(ch(b.id + ':sx', [[-0.55, '@gather']]), ch(b.id + ':sz', [[-0.55, '@gather']]), ch(b.id + ':sy', [[0.9, '@gather']]));
  by(/^foot[LR]$/).forEach((b, i) => { post.push(ch('_up' + i, [[A.foot, '@stride.' + i], [0.15 * A.foot, pos('@_ry')]]), ch(b.id + ':py', [[1, '@_up' + i]]), ch(b.id + ':pz', [[1, '@swing.' + i]]), ch(b.id + ':rx', [[-6, '@_up' + i]])); });
  // eyes look and widen with the mood and shut with the lid; the shared blink (a motion) on top
  const eyeB = by(/^eye/);
  if (eyeB.length) { groups.eyes = { bones: eyeB.map((b) => b.id), attr: {} };
    const big = [[0.28, '@is.surprised'], [0.1, '@is.listening']];
    post.push({ each: 'eyes', ch: [ch(':px', [[0.03, '@look.x']]), ch(':py', [[0.025, '@look.y']]), ch(':sx', big), ch(':sy', big), ch(':sy', [[0.25, '@eye.wide'], [-0.6, '@eye.happy'], [-0.5, '@eye.sleepy'], [-0.4, '@eye.squint']]), ch(':sy', [[-0.92, '@lid']])] }); }
  if (has('mouth')) post.push(ch('mouth:s', [[1.4, '@mouth']], { op: '=', cl: [0.001, 9] }));
  for (const m of motions) post.push(...m(groups));
  return { post, events, groups };
}
/**
 * Life while it waits on the agent (thinking, or `work`: the agent is off working): now and then one small expression, picked at
 * random, never a big gesture. Shared: a glance aside, a head tilt, a double blink, a look up, and a little yawn once it has been
 * thinking a while. `own` adds the creature's own members ({ name: { len, ch } }).
 */
export function ponder(own = {}, { gap = [2, 5] } = {}) {
  const side = sgn(0), b = (x, y) => bump('a', x, y), long = gt('@m.thinking', 14);
  return { first: 1.2, gap, len: 1.6, gate: [[1, '@is.thinking'], [1, '@work']], pick: {
    glance: { ch: [ch('look.x', [[0.7, side, b(0, 1.6)]]), ch('head.y', [[0.16, side, b(0.1, 1.6)]])] },
    tilt: { len: 1.4, ch: [ch('head.z', [[0.14, side, b(0, 1.4)]])] },
    blink: { len: 0.7, ch: [ch('lid', [[1, b(0, 0.25)], [1, b(0.35, 0.6)]], { op: 'max' })] },
    up: { len: 1.8, ch: [ch('look.y', [[0.55, b(0, 1.8)]]), ch('head.x', [[-0.1, b(0.1, 1.8)]])] },
    yawn: { len: 1.8, ch: [ch('mouth', [[0.55, '@is.thinking', long, b(0.2, 1.6)]], { op: 'max' }), ch('lid', [[0.55, '@is.thinking', long, b(0.2, 1.7)]], { op: 'max' }), ch('head.x', [[-0.14, '@is.thinking', long, b(0.1, 1.7)]])] },
    ...own } };
}
// the spec's own motions: a blink in bursts on the eyes, a breath on a bone, a pulse
export const blinkBurst = (seed, amp = -0.9, hz = 1.2, chance = 0.28, width = 0.22) => (groups) => { groups.eyes.attr.bk = groups.eyes.bones.map((_, i) => +(seed + i * 5.1).toFixed(2));
  return [{ each: 'eyes', ch: [ch(':sy', [[amp, ['chance', '@t', 1 / hz, '$bk', chance], ['bumpr', '@t', hz, 0, 0, width]]])] }]; };
export const breathe = (bone, prop, amp, hz) => () => [ch(`${bone}:${prop}`, [[amp, sin('@t', hz)]])];

// ---- writing: rounded, compact, with its size and hash for packages/index.json
const round = (x) => (typeof x === 'number' ? +x.toFixed(4) : Array.isArray(x) ? x.map(round) : x && typeof x === 'object' ? Object.fromEntries(Object.entries(x).map(([k, v]) => [k, round(v)])) : x);
export function writePackage(file, pkg) {
  const text = JSON.stringify(round(pkg)); mkdirSync(dirname(file), { recursive: true }); writeFileSync(file, text + '\n');
  const size = Buffer.byteLength(text + '\n'), sha256 = createHash('sha256').update(text + '\n').digest('hex');
  // packages/index.json: this package's entry, the others left as they are
  const idx = new URL('../../packages/index.json', import.meta.url), all = existsSync(idx) ? JSON.parse(readFileSync(idx, 'utf8')) : {};
  all[pkg.id] = indexEntry(pkg, Buffer.from(text + '\n'));
  writeFileSync(idx, JSON.stringify(Object.fromEntries(Object.entries(all).sort(([a], [b]) => (a < b ? -1 : 1))), null, 2) + '\n');
  return { size, sha256 };
}

// ---- aiming a chain: the lab bends a tail joint by joint so each segment points along a direction (body frame). Here it is solved once,
// at build time, for a body pitched forward by `pitch` (0 sitting, the quad pitch standing), and played as per-joint rotation offsets
const qmul = (a, b) => [a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0], a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]];
const qinv = (q) => [-q[0], -q[1], -q[2], q[3]], unit = (v) => { const l = Math.hypot(...v); return v.map((x) => x / l); };
const qrot = (q, v) => qmul(qmul(q, [...v, 0]), qinv(q)).slice(0, 3), qaxis = (ax, an) => [...ax.map((x) => x * Math.sin(an / 2)), Math.cos(an / 2)];
const qfrom = (a, b) => { const d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2], c = [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]; return unit([...c, 1 + d]); };
const euler = (q) => { const [x, y, z, w] = q, m11 = 1 - 2 * (y * y + z * z), m12 = 2 * (x * y - w * z), m13 = 2 * (x * z + w * y), m22 = 1 - 2 * (x * x + z * z), m23 = 2 * (y * z - w * x), m32 = 2 * (y * z + w * x), m33 = 1 - 2 * (x * x + y * y), ey = Math.asin(Math.max(-1, Math.min(1, m13)));
  return Math.abs(m13) < 0.9999 ? [Math.atan2(-m23, m33), ey, Math.atan2(-m12, m11)] : [Math.atan2(m32, m22), ey, 0]; };
/** Rotation offsets [rx, ry, rz] per joint of `chain` (bone ids) that point its segments along dirs (body frame), with the bones
 *  turned about x by `rx` ({ torso: 1.25, hips: -0.55 }: how the body is posed when the aim is held). */
export function aimChain(bones, chain, dirs, rx = {}) {
  const by = new Map(bones.map((b) => [b.id, b])), fk = (id) => { const b = by.get(id); if (!b) return { p: [0, 0, 0], q: [0, 0, 0, 1] };
    const up = fk(b.parent ?? 'root'), p = qrot(up.q, b.pos).map((x, k) => x + up.p[k]); return { p, q: qmul(up.q, qaxis([1, 0, 0], rx[id] ?? 0)) }; };
  const J = chain.map((id) => fk(id).p), out = []; let acc = fk(by.get(chain[0]).parent).q;
  for (let i = 0; i < chain.length - 1; i++) { const cur = unit(J[i + 1].map((x, k) => x - J[i][k])), q = qfrom(cur, unit(dirs[i])), local = qmul(qmul(qinv(acc), q), acc);
    out.push(euler(local).map((x) => +x.toFixed(4))); for (let j = i + 1; j < J.length; j++) J[j] = qrot(q, J[j].map((x, k) => x - J[i][k])).map((x, k) => x + J[i][k]); acc = qmul(q, acc); }
  return out;
}
