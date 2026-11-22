// The rig's numbers (README.md has each one in words and as a formula). Plain functions over what a
// scene recorded a frame at a time; no module, no files, so the tests can feed them made-up input.
//   pics     the creature's picture a frame: Float32Array, one value a cell (its mean brightness)
//   change   a frame's changed share: the creature's pixels that differ from the frame before, over its lit ones
//   bones    a frame's bones: Float32Array, 12 floats a node (3 rows of 4), the rig's node order

/** Loop echo: how alike the picture is to itself `lag` frames later, for every lag from lo to hi frames (hi at most
 *  half the frames, so every lag compares at least half of the scene). For a lag: the median, over the scene, of the
 *  distance between a frame and the one that lag later, against the median over every lag: echo = 1 - that ratio.
 *  1: the typical moment comes back exactly; about 0: no more alike than any two moments. Medians, so one big gesture
 *  does not drown the small loop under it. The strongest, its lag (the shortest of equals), how far it stands over
 *  the median lag, and the echo at `at` frames (null when out of range). Pictures that never change: null. */
export function loopEcho(pics, lo, hi, at = -1) {
  const T = pics.length, D = T ? pics[0].length : 0;
  hi = Math.min(hi, Math.floor(T / 2));
  if (T < 2 || hi < lo) return null;
  const dist = [], all = [];
  for (let L = lo; L <= hi; L++) {
    const d = new Float64Array(T - L);
    for (let t = 0; t + L < T; t++) {
      const x = pics[t], y = pics[t + L];
      let s = 0;
      for (let k = 0; k < D; k++) s += (x[k] - y[k]) ** 2;
      d[t] = Math.sqrt(s);
    }
    dist.push(median(d)); all.push(d);
  }
  const ref = median(concat(all));
  if (!(ref > 0)) return null;
  const echo = dist.map((m) => 1 - m / ref);
  let best = 0;
  for (let i = 1; i < echo.length; i++) if (echo[i] > echo[best]) best = i;
  return { echo: echo[best], lag: lo + best, standout: echo[best] - median(echo), at: at >= lo && at <= hi ? echo[at - lo] : null, curve: echo };
}
function concat(arrays) {
  const out = new Float64Array(arrays.reduce((n, a) => n + a.length, 0));
  let o = 0;
  for (const a of arrays) { out.set(a, o); o += a.length; }
  return out;
}

/** Still share: the frames (after the first) whose changed share is under `under`. */
export function stillShare(change, under = 0.01) {
  const c = change.slice(1);
  return c.length ? c.filter((v) => v < under).length / c.length : null;
}

/** Rhythm spread: the motion (changed share) smoothed over `win` frames; a burst starts where it rises to its
 *  scene mean, the next only after it fell under 0.7 of that mean. Spread: the gaps' standard deviation over their
 *  mean (frames in, frames out: the ratio has no unit). Fewer than 3 gaps: null. Also the starts and the gaps. */
export function rhythmSpread(change, win = 6) {
  const s = change.map((_, t) => { let a = 0; for (let k = Math.max(0, t - win + 1); k <= t; k++) a += change[k]; return a / win; });
  const mean = s.reduce((a, v) => a + v, 0) / (s.length || 1);
  const starts = [];
  let armed = true;
  for (let t = 0; t < s.length; t++) {
    if (armed && mean > 0 && s[t] >= mean) { starts.push(t); armed = false; }
    else if (!armed && s[t] < 0.7 * mean) armed = true;
  }
  const gaps = starts.slice(1).map((t, i) => t - starts[i]);
  if (gaps.length < 3) return { spread: null, starts, gaps };
  const m = gaps.reduce((a, v) => a + v, 0) / gaps.length;
  const sd = Math.sqrt(gaps.reduce((a, v) => a + (v - m) ** 2, 0) / gaps.length);
  return { spread: sd / m, starts, gaps };
}

/** The body's parts by node name; a node with no parent (the root, a prop) is in none. */
export function partOf(id, parent) {
  if (parent < 0) return null;
  if (/^eye/.test(id)) return 'eyes';
  if (/^ear/.test(id)) return 'ears';
  if (/^tail/.test(id)) return 'tail';
  if (/^(neck|head|mouth|jaw|tongue)/.test(id)) return 'head';
  return 'body';
}
export const PARTS = ['head', 'eyes', 'ears', 'tail', 'body'];
const AXIS = 0.1;                                       // a bone's little frame: its origin and three tips this far out

/** Where each bone's frame is, seen from its anchor (the nearest node above it in another part, or the root): its
 *  origin and three axis tips, 12 numbers a node. nodes: [{ id, parent }]. */
function local(m, nodes, anchor) {
  const out = new Float64Array(nodes.length * 12);
  for (let i = 0; i < nodes.length; i++) {
    const a = anchor[i];
    if (a < 0) continue;
    const A = m.subarray(a * 12, a * 12 + 12), B = m.subarray(i * 12, i * 12 + 12);
    const inv = inverse3(A);
    for (let k = 0; k < 4; k++) {
      // the point in the bone's own frame: its origin, then AXIS along x, y, z
      const p = [0, 0, 0]; if (k) p[k - 1] = AXIS;
      const w = [0, 1, 2].map((r) => B[r * 4] * p[0] + B[r * 4 + 1] * p[1] + B[r * 4 + 2] * p[2] + B[r * 4 + 3] - A[r * 4 + 3]);
      for (let r = 0; r < 3; r++) out[i * 12 + k * 3 + r] = inv[r * 3] * w[0] + inv[r * 3 + 1] * w[1] + inv[r * 3 + 2] * w[2];
    }
  }
  return out;
}
function inverse3(A) {
  const [a, b, c, d, e, f, g, h, i] = [A[0], A[1], A[2], A[4], A[5], A[6], A[8], A[9], A[10]];
  const det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g) || 1;
  return [e * i - f * h, c * h - b * i, b * f - c * e, f * g - d * i, a * i - c * g, c * d - a * f, d * h - e * g, b * g - a * h, a * e - b * d].map((v) => v / det);
}

/** Each bone's motion a frame: how far its little frame moved (the mean of its four points) since the frame
 *  before, seen from its anchor. [frame][node]; frame 0 is all 0. */
export function boneMotion(bones, nodes) {
  const part = nodes.map((n) => partOf(n.id, n.parent));
  const anchor = nodes.map((n, i) => {
    if (!part[i]) return -1;
    let a = n.parent;
    while (a >= 0 && part[a] === part[i]) a = nodes[a].parent;
    return a;
  });
  let prev = null;
  return bones.map((m) => {
    const cur = local(m, nodes, anchor), v = new Float64Array(nodes.length);
    if (prev) for (let i = 0; i < nodes.length; i++) {
      if (!part[i]) continue;
      for (let k = 0; k < 4; k++) v[i] += Math.hypot(cur[i * 12 + k * 3] - prev[i * 12 + k * 3], cur[i * 12 + k * 3 + 1] - prev[i * 12 + k * 3 + 1], cur[i * 12 + k * 3 + 2] - prev[i * 12 + k * 3 + 2]) / 4;
    }
    prev = cur;
    return v;
  });
}

/** Body spread: each part's share of the motion. A part's motion is its bones' mean path (the sum of their per frame
 *  motion over the scene, averaged over the part's bones), so a part with many bones does not weigh more. */
export function bodySpread(motion, nodes) {
  const part = nodes.map((n) => partOf(n.id, n.parent)), sum = Object.fromEntries(PARTS.map((p) => [p, 0]));
  for (const p of PARTS) {
    const ix = part.flatMap((q, i) => (q === p ? [i] : []));
    if (ix.length) sum[p] = motion.reduce((a, v) => a + ix.reduce((b, i) => b + v[i], 0), 0) / ix.length;
  }
  const all = PARTS.reduce((a, p) => a + sum[p], 0);
  return Object.fromEntries(PARTS.map((p) => [p, all > 0 ? sum[p] / all : null]));
}

/** Pops: frames where some bone jumps at least `floor` (world units: a braille dot is about 0.015) and at least 4
 *  times as far as it moved in the frame before: a jump out of nowhere, where a move that eases in grows frame by
 *  frame. Frames within `quiet` of a scripted step are not counted. The frames. */
export function pops(motion, scripted = [], quiet = 15, floor = 0.015) {
  const out = [];
  for (let t = 2; t < motion.length; t++) {
    if (scripted.some((s) => t >= s && t < s + quiet)) continue;
    for (let i = 0; i < motion[t].length; i++) {
      const v = motion[t][i];
      if (v >= floor && v >= 4 * motion[t - 1][i]) { out.push(t); break; }
    }
  }
  return out;
}

/** The first frame from `from` on where two recordings differ (pictures: any cell; Float32Arrays), or -1. */
export function firstDiff(a, b, from) {
  for (let t = from; t < Math.min(a.length, b.length); t++) {
    const x = a[t], y = b[t];
    for (let d = 0; d < x.length; d++) if (x[d] !== y[d]) return t;
  }
  return -1;
}

export function median(v) {
  const s = Float64Array.from(v).sort(), n = s.length;
  return n ? (n % 2 ? s[(n - 1) / 2] : (s[n / 2 - 1] + s[n / 2]) / 2) : 0;
}
