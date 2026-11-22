// SPDX-License-Identifier: MIT
// A Molty's actions into the clips the device plays (docs/CREATURES.md section 12): each action is read
// once a frame, then every bone's move, turn and scale keeps only the keys it needs to stay within a
// tolerance of every frame, and the keys are packed as small integers.
// This is the cat's first exporter step for step (it was Python with numpy), so the same frames give
// the same bytes: the sums are done in the same order and halves round to even.

export const SCALE = { t: 4096, r: 32767, s: 4096 };   // int16: a move and a scale at 1/4096, a turn normalised
const TOL = { t: 0.003, r: 0.008, s: 0.02 };           // about 0.15 dot at the body's size (a dot is 0.018)
const TOL_FACE_S = 0.05;                               // eyelids, mouth, tongue: a scale off by this is not a dot
const TOL_SOFT_R = 0.02;                               // tail, ears, face: their turns as one byte, looser
const TOL_LEG_R = 0.0045;                              // the chains from the root to the paws: a planted paw is their sum
// a bone's kind is its name up to the first dot, without trailing digits (tail7 is a tail, ear.L an ear)
const LEGS = ['pelvis', 'spine', 'chest', 'scapula', 'upperarm', 'forearm', 'hand', 'thigh', 'shin', 'foot', 'toe'];
const SOFT = ['tail', 'ear', 'eye', 'mouth', 'tongue', 'neck', 'head'];
const WIDTH = { t: 3, r: 4, s: 3 };

/** Python's round(): a half goes to the even neighbour. */
const round = (v) => { const f = Math.floor(v); return v - f === 0.5 ? f + (f & 1) : Math.round(v); };
/** Six significant digits (Python's '%.6g'): an exact half goes to the even digit, as Python sends it. */
export function r6(v) {
  const s = +v.toPrecision(6);
  if (!Number.isFinite(v) || v === 0) return s;
  const [digits, e] = Math.abs(v).toExponential(60).replace('.', '').split('e');   // every digit it has
  if (!/^50*$/.test(digits.slice(6))) return s;
  const six = BigInt(digits.slice(0, 6));
  return Math.sign(v) * +`${six + (six & 1n)}e${+e - 5}`;
}
const pack = (bytes, vals) => { const b = Buffer.alloc(vals.length * bytes); vals.forEach((v, i) => (bytes === 1 ? b.writeUInt8(v & 255, i) : b.writeUInt16LE(v & 65535, i * 2))); return b.toString('base64'); };
const q16 = (x, k) => Math.max(-32767, Math.min(32767, round(x * k))), q8 = (x) => Math.max(-127, Math.min(127, round(x * 127)));
const row = (v, i, w) => v.subarray(i * w, (i + 1) * w);

/** LINEAR keys: straight segments within tol of every frame. v: frames x w. */
function reduceLinear(v, n, w, tol) {
  let keys = n > 1 ? [0, n - 1] : [0];
  while (keys.length > 1) {
    let worst = 0, at = -1;
    for (let k = 0; k + 1 < keys.length; k++) {
      const a = keys[k], b = keys[k + 1];
      let best = -1, bj = -1;
      for (let j = a + 1; j < b; j++) {
        const u = (j - a) / (b - a);
        let e = 0;
        for (let c = 0; c < w; c++) e = Math.max(e, Math.abs(v[a * w + c] + (v[b * w + c] - v[a * w + c]) * u - v[j * w + c]));
        if (e > best) { best = e; bj = j; }
      }
      if (bj >= 0 && best > worst) { worst = best; at = bj; }
    }
    if (worst <= tol || at < 0) break;
    keys = [...keys, at].sort((x, y) => x - y);
  }
  return keys;
}
/** CUBICSPLINE keys: a Hermite through them, with finite-difference tangents, within tol of every frame. */
function reduceCubic(v, n, w, tol, loop) {
  const d = new Float64Array(n * w), set = (i, f) => { for (let c = 0; c < w; c++) d[i * w + c] = f(c); };
  for (let i = 1; i < n - 1; i++) set(i, (c) => (v[(i + 1) * w + c] - v[(i - 1) * w + c]) / 2);
  set(0, (c) => v[Math.min(1, n - 1) * w + c] - v[c]);
  set(n - 1, (c) => v[(n - 1) * w + c] - v[Math.max(0, n - 2) * w + c]);
  if (loop && n > 2) { set(0, (c) => (v[w + c] - v[(n - 2) * w + c]) / 2); set(n - 1, (c) => d[c]); }   // a loop's ends meet smoothly
  let keys = n > 1 ? [0, n - 1] : [0];
  while (keys.length > 1) {
    let worst = 0, at = -1;
    for (let k = 0; k + 1 < keys.length; k++) {
      const a = keys[k], b = keys[k + 1], L = b - a;
      if (L < 2) continue;
      let best = -1, bj = -1;
      for (let j = a + 1; j < b; j++) {
        const u = (j - a) / L, u2 = u * u, u3 = u * u * u;
        let e = 0;
        for (let c = 0; c < w; c++) e = Math.max(e, Math.abs((2 * u3 - 3 * u2 + 1) * v[a * w + c] + (u3 - 2 * u2 + u) * (d[a * w + c] * L) + (-2 * u3 + 3 * u2) * v[b * w + c] + (u3 - u2) * (d[b * w + c] * L) - v[j * w + c]));
        if (e > best) { best = e; bj = j; }
      }
      if (best > worst) { worst = best; at = bj; }
    }
    if (worst <= tol || at < 0) break;
    keys = [...keys, at].sort((x, y) => x - y);
  }
  return { keys, d };
}

/** One bone's move, turn or scale over a clip: [node, path, S|L|C, bytes a number, key frames, values], the smallest that holds. */
function channel(node, name, path, v, n, loop) {
  const w = WIDTH[path], kind = name.split('.')[0].replace(/\d+$/, ''), soft = path === 'r' && SOFT.includes(kind);
  const tol = soft ? TOL_SOFT_R : path === 'r' && LEGS.includes(kind) ? TOL_LEG_R : path === 's' && SOFT.includes(kind) ? TOL_FACE_S : TOL[path];
  const kin = n < 256 ? 1 : 2, k = SCALE[path], enc = (xs) => (soft ? pack(1, xs.map(q8)) : pack(2, xs.map((x) => q16(x, k))));
  const at = (keys, f) => keys.flatMap((i) => f(i)), head = [node, path];
  let still = 0;
  for (let i = 0; i < n; i++) for (let c = 0; c < w; c++) still = Math.max(still, Math.abs(v[i * w + c] - v[c]));
  if (still <= tol * 0.5) return [...head, 'S', soft ? 1 : 2, pack(kin, [0]), enc([...row(v, 0, w)])];   // held: one key
  const kl = reduceLinear(v, n, w, tol), lin = [...head, 'L', soft ? 1 : 2, pack(kin, kl), enc(at(kl, (i) => [...row(v, i, w)]))];
  if (soft) return lin;
  const { keys: kc, d } = reduceCubic(v, n, w, tol, loop);
  const cub = [...head, 'C', 2, pack(kin, kc), enc(at(kc, (i) => [...row(d, i, w), ...row(v, i, w), ...row(d, i, w)]))];   // in-tangent, value, out-tangent
  return cub[5].length + cub[4].length < lin[5].length + lin[4].length ? cub : lin;
}

/**
 * One action as a clip. nodes: the body's (rest t, r, s). loop: whether it repeats (then its last key, the first pose
 * again, is left off). moving: keep only what moves in it (a clip a life layer adds: a held bone adds nothing).
 * Returns { dur, frames, ch, first, last }: first and last are the poses it starts and ends in.
 */
export function bakeAction(nodes, action, fps, loop, moving = false) {
  const end = Math.max(0, ...action.tracks.map((t) => t.times[t.times.length - 1])), dur = r6(end);
  const n = round(dur * fps) + (loop ? 0 : 1), ch = [], first = [], last = [];
  const tracks = new Map(action.tracks.map((t) => [`${t.node} ${t.path}`, t]));
  nodes.forEach((node, ni) => {
    for (const path of ['t', 'r', 's']) {
      const w = WIDTH[path], t = tracks.get(`${ni} ${path}`), rest = node[path], v = new Float64Array(n * w);
      for (let i = 0; i < n; i++) {
        const time = Math.min(i / fps, dur);
        if (!t) { v.set(rest, i * w); continue; }
        // the key on this frame; between two keys, a straight line (a held key: the earlier one)
        let b = 0;
        while (b < t.times.length - 1 && t.times[b] < time - 1e-4) b++;
        const a = b === 0 || t.times[b] <= time + 1e-4 ? b : b - 1, u = a === b || t.step ? 0 : (time - t.times[a]) / (t.times[b] - t.times[a]);
        for (let c = 0; c < w; c++) v[i * w + c] = u ? t.values[a * w + c] + (t.values[b * w + c] - t.values[a * w + c]) * u : t.values[a * w + c];
        if (path === 'r' && i) {   // one side of the quaternion sphere all through
          let dot = 0;
          for (let c = 0; c < 4; c++) dot += v[(i - 1) * 4 + c] * v[i * 4 + c];
          if (dot < 0) for (let c = 0; c < 4; c++) v[i * 4 + c] = -v[i * 4 + c];
        }
      }
      first.push(...row(v, 0, w)); last.push(...row(v, n - 1, w));
      let dot = 0, away = 0, moves = 0;
      if (path === 'r') for (let c = 0; c < 4; c++) dot += v[c] * rest[c];
      for (let i = 0; i < n; i++) for (let c = 0; c < w; c++) {
        away = Math.max(away, Math.abs(v[i * w + c] - (dot < 0 ? -rest[c] : rest[c])));
        moves = Math.max(moves, Math.abs(v[i * w + c] - v[c]));
      }
      // the rest pose all through: left out; so is a held bone, in a clip that only moves
      if (away > TOL[path] * 0.25 && (!moving || moves > TOL[path] * 0.25)) ch.push(channel(ni, node.name, path, v, n, loop));
    }
  });
  return { dur, frames: n, ch, first, last };
}
