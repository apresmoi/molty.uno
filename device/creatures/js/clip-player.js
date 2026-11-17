// The clip player (docs/CREATURES.md section 12): plays a creature package that carries a `rig` (a skeleton, its body as
// rounded primitives, baked clips in glTF's animation layout and the route map between pose families). The JS reference for
// main/creature/creature_clip.c; the C port follows it step for step, so the goldens (tools/creatures/clip-golden.mjs) match.
//
//   const p = clipPlayer(pkg.creature.rig);
//   p.mood('happy'); p.act('eat');          what the agent asks (mood names, gesture names)
//   p.update(dt);                           one frame: routing, sampling, blending, root motion, the camera
//   p.world                                 Float32Array(nodes * 12): each node's world matrix (3x4, column-major rows)
//   p.cam                                   { x, z, yaw }: where the camera looks (on the floor) and from which side
//   p.clip, p.t                             what plays
//
// Routing: every clip starts and ends in a pose family (rig.graph.families). The player keeps a target (the mood's loop, or a
// gesture once); to get there it plays authored transition clips (graph.edges, one per ordered pair of families), and
// crossfades (graph.blend seconds, eased) only between clips of the same family. A loop listed in graph.sync (the walk) is
// left and joined at the phase that matches, so a stepping paw is never cut mid-stride.

export const MOODS = ['idle', 'listening', 'thinking', 'speaking', 'happy', 'sleepy', 'surprised'];

const b64 = (s) => (typeof atob === 'function' ? Uint8Array.from(atob(s), (c) => c.charCodeAt(0)) : new Uint8Array(Buffer.from(s, 'base64')));
const i16 = (s) => { const u = b64(s); return new Int16Array(u.buffer.slice(u.byteOffset, u.byteOffset + u.length)); };
const i8 = (s) => { const u = b64(s); return new Int8Array(u.buffer.slice(u.byteOffset, u.byteOffset + u.length)); };
const u16 = (s) => { const u = b64(s); return new Uint16Array(u.buffer.slice(u.byteOffset, u.byteOffset + u.length)); };
const ease = (x) => (x <= 0 ? 0 : x >= 1 ? 1 : x * x * (3 - 2 * x));
const WIDTH = { t: 3, r: 4, s: 3 };
const ATTEND = 'listening', ATTEND_S = 3;   // called, it sits up attending in this clip (ears up, eyes on the viewer), this long
const FRONT = 0.15;   // facing front this near the middle (across the frame) it only perks up
const COME_R = 0.08, COME_K = 3, COME_S = 8;   // at the front this near the middle across, facing front; steering; or after walking this long
const PACE = 0.6;   // the most the body moves a second (world units): above a walk's 0.36 (creature_clip.c CC_PACE)
const EDGE = 0.35;  // walking past this far from the frame's centre (world units, across) turns it round (creature_clip.c CC_EDGE)
const EPS = 1e-5;   // a clip's end, against float clocks (the C player's time is float steps)

/** Decode a clip's channels: per channel the node, path, keys (frames) and float outputs. */
const INTERP = { S: 'STEP', L: 'LINEAR', C: 'CUBICSPLINE' };
export function decodeClip(c, scale) {
  return { ...c, ch: c.ch.map((h) => {
    if (Array.isArray(h)) h = { node: h[0], path: h[1], interp: INTERP[h[2]], ct: h[3] === 1 ? 'i8' : 'i16', in: h[4], out: h[5] };   // the package's compact form
    const b8 = h.ct === 'i8', k = b8 ? 127 : scale[h.path], keys = c.frames < 256 ? b64(h.in) : u16(h.in), raw = b8 ? i8(h.out) : i16(h.out), out = new Float32Array(raw.length);
    for (let i = 0; i < raw.length; i++) out[i] = raw[i] / k;
    return { node: h.node, path: h.path, cubic: h.interp === 'CUBICSPLINE', linear: h.interp === 'LINEAR', keys, out, w: WIDTH[h.path] };
  }) };
}

/** A channel's value at frame f (fractional), into dst. Loops wrap from the last key to frame `period` (= the first key). */
export function sampleChannel(h, f, period, loop, dst) {
  const { keys, out, w, cubic, linear } = h, n = keys.length, stride = cubic ? 3 * w : w, vo = cubic ? w : 0;
  if (n === 1 || (!cubic && !linear)) { for (let c = 0; c < w; c++) dst[c] = out[vo + c]; return dst; }
  let a = n - 1, b = 0, fa = keys[n - 1], fb = period;
  if (!loop || f < keys[n - 1]) {
    if (f <= keys[0]) { for (let c = 0; c < w; c++) dst[c] = out[vo + c]; return dst; }
    if (f >= keys[n - 1]) { for (let c = 0; c < w; c++) dst[c] = out[(n - 1) * stride + vo + c]; return dst; }
    let lo = 0, hi = n - 1;                                  // keys[lo] <= f < keys[hi]
    while (hi - lo > 1) { const m = (lo + hi) >> 1; if (keys[m] <= f) lo = m; else hi = m; }
    a = lo; b = hi; fa = keys[lo]; fb = keys[hi];
  }
  const td = fb - fa, u = (f - fa) / td;
  if (linear) {                                             // LINEAR (rotations: the shorter way, then normalised)
    let sg = 1;
    if (h.path === 'r') { let d = 0; for (let c = 0; c < 4; c++) d += out[a * stride + c] * out[b * stride + c]; sg = d < 0 ? -1 : 1; }
    for (let c = 0; c < w; c++) dst[c] = out[a * stride + c] + (sg * out[b * stride + c] - out[a * stride + c]) * u;
  } else {
    const u2 = u * u, u3 = u2 * u;
    const h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
    for (let c = 0; c < w; c++) {
      const v0 = out[a * stride + w + c], m0 = out[a * stride + 2 * w + c], v1 = out[b * stride + w + c], m1 = out[b * stride + c];
      dst[c] = h00 * v0 + h10 * td * m0 + h01 * v1 + h11 * td * m1;
    }
  }
  if (h.path === 'r') { const l = Math.hypot(dst[0], dst[1], dst[2], dst[3]) || 1; for (let c = 0; c < 4; c++) dst[c] /= l; }
  return dst;
}

/** A pose: per node t (3), r (4), s (3), packed in 10 floats. */
const POSE = 10;
export function restPose(rig) {
  const p = new Float32Array(rig.nodes.length * POSE);
  rig.nodes.forEach((n, i) => { p.set(n.t, i * POSE); p.set(n.r, i * POSE + 3); p.set(n.s, i * POSE + 7); });
  return p;
}

/** Sample clip c at t seconds into pose (over the rest pose). */
export function samplePose(rig, rest, c, t, pose) {
  pose.set(rest);
  const period = c.frames, f = c.loop ? ((t * rig.fps) % period + period) % period : Math.min(t * rig.fps, period - 1), tmp = [0, 0, 0, 0];
  for (const h of c.ch) {
    sampleChannel(h, f, period, c.loop, tmp);
    const n = h.keys.length;
    if (c.loop && h.node === 0 && h.path === 't' && n >= 2 && f >= h.keys[n - 1]) {
      // a loop's root travels: past its last key it carries on at the last step's pace (it does not swing back to the
      // first key's place), so a loop's end is its start plus one cycle's travel (the wrap in update)
      const st = h.cubic ? 3 * h.w : h.w, vo = h.cubic ? h.w : 0, a = (n - 2) * st + vo, b = (n - 1) * st + vo, u = (f - h.keys[n - 1]) / (h.keys[n - 1] - h.keys[n - 2]);
      for (let q = 0; q < 3; q++) tmp[q] = h.out[b + q] + (h.out[b + q] - h.out[a + q]) * u;
    }
    const o = h.node * POSE + (h.path === 't' ? 0 : h.path === 'r' ? 3 : 7);
    for (let k = 0; k < h.w; k++) pose[o + k] = tmp[k];
  }
  return pose;
}

/** Blend two poses: lerp t and s, nlerp r (shortest way). */
export function blendPose(a, b, w, out) {
  const n = a.length / POSE;
  for (let i = 0; i < n; i++) {
    const o = i * POSE;
    for (let k = 0; k < 3; k++) { out[o + k] = a[o + k] + (b[o + k] - a[o + k]) * w; out[o + 7 + k] = a[o + 7 + k] + (b[o + 7 + k] - a[o + 7 + k]) * w; }
    const d = a[o + 3] * b[o + 3] + a[o + 4] * b[o + 4] + a[o + 5] * b[o + 5] + a[o + 6] * b[o + 6], sg = d < 0 ? -1 : 1;
    let l = 0;
    for (let k = 3; k < 7; k++) { out[o + k] = a[o + k] + (sg * b[o + k] - a[o + k]) * w; l += out[o + k] * out[o + k]; }
    l = Math.sqrt(l) || 1; for (let k = 3; k < 7; k++) out[o + k] /= l;
  }
  return out;
}

/** World matrices (3x4, rows) from a pose; the root sits at (ox, 0, oz). */
export function forward(rig, pose, ox, oz, world) {
  const N = rig.nodes.length;
  for (let i = 0; i < N; i++) {
    const o = i * POSE, [tx, ty, tz] = [pose[o], pose[o + 1], pose[o + 2]], [x, y, z, w] = [pose[o + 3], pose[o + 4], pose[o + 5], pose[o + 6]];
    const sx = pose[o + 7], sy = pose[o + 8], sz = pose[o + 9];
    const L = [
      (1 - 2 * (y * y + z * z)) * sx, (2 * (x * y - z * w)) * sy, (2 * (x * z + y * w)) * sz, tx,
      (2 * (x * y + z * w)) * sx, (1 - 2 * (x * x + z * z)) * sy, (2 * (y * z - x * w)) * sz, ty,
      (2 * (x * z - y * w)) * sx, (2 * (y * z + x * w)) * sy, (1 - 2 * (x * x + y * y)) * sz, tz];
    const p = rig.nodes[i].parent, W = i * 12;
    if (p < 0) { for (let k = 0; k < 12; k++) world[W + k] = L[k]; world[W + 3] += ox; world[W + 11] += oz; continue; }
    const P = p * 12;
    for (let r = 0; r < 3; r++) for (let c = 0; c < 4; c++)
      world[W + r * 4 + c] = world[P + r * 4] * L[c] + world[P + r * 4 + 1] * L[4 + c] + world[P + r * 4 + 2] * L[8 + c] + (c === 3 ? world[P + r * 4 + 3] : 0);
  }
  return world;
}

const apply = (w, n, p) => [w[n * 12] * p[0] + w[n * 12 + 1] * p[1] + w[n * 12 + 2] * p[2] + w[n * 12 + 3],
  w[n * 12 + 4] * p[0] + w[n * 12 + 5] * p[1] + w[n * 12 + 6] * p[2] + w[n * 12 + 7],
  w[n * 12 + 8] * p[0] + w[n * 12 + 9] * p[1] + w[n * 12 + 10] * p[2] + w[n * 12 + 11]];
export const pointOn = apply;

/** The shortest route of authored clips from family a to family b (breadth first over graph.edges, in key order). */
export function route(graph, a, b) {
  if (a === b) return [];
  const prev = { [a]: null }, q = [a];
  while (q.length) {
    const f = q.shift();
    for (const [k, clip] of Object.entries(graph.edges)) {
      const [x, y] = k.split('>');
      if (x !== f || y in prev) continue;
      prev[y] = [f, clip]; if (y === b) { const r = []; for (let s = b; prev[s]; s = prev[s][0]) r.unshift(prev[s][1]); return r; }
      q.push(y);
    }
  }
  return null;
}

export function clipPlayer(rig) {
  const names = Object.keys(rig.clips), clips = names.map((n) => ({ name: n, ...decodeClip(rig.clips[n], rig.scale) }));
  const byName = Object.fromEntries(clips.map((c, i) => [c.name, i])), N = rig.nodes.length, g = rig.graph;
  const rest = restPose(rig), cur = new Float32Array(N * POSE), from = new Float32Array(N * POSE), tmp = new Float32Array(N * POSE), pose = new Float32Array(N * POSE);
  const world = new Float32Array(N * 12), fromWorld = new Float32Array(N * 12), out = new Float32Array(N * 12), rootNode = 0;
  const contacts = rig.contacts || [];
  const s = { clip: byName[g.rest.sit] ?? 0, t: 0, ox: 0, oz: 0, queue: [], blendT: 1, blendD: g.blend, lockX: 0, lockZ: 0,
    mood: 'idle', gesture: null, gestureT: 0, pending: false, cam: { x: 0, z: 0, yaw: clips[0].view, snap: true }, hd: 0, hbase: 0, sense: 0, come: 0, comeT: 0, px: 0, pz: 0, tx: 0, tz: 0, cx0: 0, cz0: 0 };
  const clipOf = (name) => byName[name];
  const loopOfMood = (m) => byName[g.moods?.[m] ?? m] ?? byName[g.rest.sit];
  const rootAt = (ci, t) => { samplePose(rig, rest, clips[ci], t, tmp); return [tmp[rootNode * POSE], tmp[rootNode * POSE + 2]]; };

  // the phase of a loop that best matches a pose (every frame of the loop, the summed node distance)
  function bestPhase(ci, target) {
    const c = clips[ci]; let best = 0, bd = Infinity;
    for (let f = 0; f < c.frames; f++) {
      samplePose(rig, rest, c, f / rig.fps, tmp); let d = 0;
      for (let i = 1; i < N; i++) { const o = i * POSE; d += 1 - Math.abs(tmp[o + 3] * target[o + 3] + tmp[o + 4] * target[o + 4] + tmp[o + 5] * target[o + 5] + tmp[o + 6] * target[o + 6]); }
      if (d < bd - 1e-9) { bd = d; best = f; }
    }
    return best / rig.fps;
  }

  // start clip ci now, crossfading from the pose on screen; the root stays where it was (root motion carries on)
  function start(ci, blend = g.blend, t0 = 0) {
    const c = clips[ci], [rx, rz] = [cur[rootNode * POSE], cur[rootNode * POSE + 2]];
    from.set(cur); fromWorld.set(world);
    if (c.loop && g.sync?.[c.name] != null) t0 = bestPhase(ci, cur);
    const [nx, nz] = rootAt(ci, t0);
    s.ox += rx - nx; s.oz += rz - nz;
    s.clip = ci; s.t = t0; s.blendT = 0; s.blendD = blend;
  }

  // what plays after this: the gesture's clip, else the mood's loop, reached through authored transitions
  function target() {
    if (s.gesture) return byName[s.gesture.clip];
    return loopOfMood(s.mood);
  }
  function plan() {
    const want = target(), c = clips[s.clip], fam = c.to;
    if (want === s.clip && c.loop) { s.queue = []; return; }
    const w = clips[want], enter = g.enter?.[w.name]?.[fam];
    if (enter) { s.queue = enter === w.name ? [want] : [byName[enter], want]; return; }   // an authored way in (the startle)
    s.queue = (route(g, fam, w.from) || []).map(clipOf).concat([want]);
  }
  function advance() {
    if (!s.queue.length) plan();
    const next = s.queue.shift();
    if (next == null) return;
    if (s.gesture && next === byName[s.gesture.clip]) s.gesture.started = true;
    start(next);
  }

  // The camera stays where it started, looking from the first clip's side: the floor never moves, so a walking frame
  // changes only the body's cells. The body turns instead (about its centre: the same picture of it as the camera going
  // round), to each clip's side, and walks on the floor in the way it faces; near the frame's side edges, walking outward,
  // it turns round (an arc, as it walks). world: the clip's pose (root motion included); out: the same turned and placed.
  function turn(dt) {
    const cc = clips[s.clip], cx = (world[g.center[0] * 12 + 3] + world[g.center[1] * 12 + 3]) / 2, cz = (world[g.center[0] * 12 + 11] + world[g.center[1] * 12 + 11]) / 2;
    const D2R = Math.PI / 180, TAU = 2 * Math.PI, f = Math.fround;
    if (s.cam.snap) { s.cam.x = s.px = s.tx = cx; s.cam.z = s.pz = s.tz = cz; s.cam.yaw = cc.view; s.hd = s.hbase = s.sense = 0; s.cam.snap = false; }
    else {
      const dx = cx - s.cx0, dz = cz - s.cz0, sn = Math.sin(s.hd), k = Math.cos(s.hd);
      const wx = k * dx + sn * dz, wz = -sn * dx + k * dz;
      // where it should be (tx, tz); it goes there at most PACE a second: a walk's own pace, and a crossfade's jump of
      // the centre eased out (as the following camera did)
      s.tx = f(s.tx + wx); s.tz = f(s.tz + wz);
      const ex = s.tx - s.px, ez = s.tz - s.pz, el = Math.hypot(ex, ez), mv = PACE * dt, q = el > mv ? mv / el : 1;
      s.px = f(s.px + ex * q); s.pz = f(s.pz + ez * q);
      const rx = Math.cos(s.cam.yaw * D2R), rz = -Math.sin(s.cam.yaw * D2R);
      const lat = (s.px - s.cam.x) * rx + (s.pz - s.cam.z) * rz, vlat = wx * rx + wz * rz;
      let want = s.hbase + (s.cam.yaw - cc.view) * D2R, gap = want - s.hd;
      gap -= TAU * Math.round(gap / TAU);
      const walking = cc.to === 'walk';
      if (s.come === 1 && walking) {
        // called: it walks toward the viewer, steering to the middle across the frame, and stops there facing front (its
        // stop carries it on toward the viewer, which does not show)
        const fx = Math.sin(s.cam.yaw * D2R), fz = Math.cos(s.cam.yaw * D2R), vx = fx - COME_K * lat * rx, vz = fz - COME_K * lat * rz;   // toward the viewer
        s.comeT = f(s.comeT + dt);
        if ((Math.abs(lat) < COME_R && Math.sin(s.hd) * fx + Math.cos(s.hd) * fz > 0.9) || s.comeT > COME_S) {
          attend();                                    // its stop still walks on toward the viewer
        } else { s.hbase = f(Math.atan2(vx, vz) - (s.cam.yaw - cc.view) * D2R); s.sense = 0; }   // the walk's travel (+z) along v
      }
      if (s.come === 2 && cc.to !== 'walk' && cc.from !== 'walk') {    // out of the walk: it turns to face the viewer
        s.hbase = f(TAU * Math.floor((s.hd - (s.cam.yaw - cc.view) * D2R) / TAU + 0.5)); s.sense = 0;
        s.come = 3;
      }
      if (walking && !s.come && Math.abs(lat) > EDGE && lat * vlat > 0.05 * dt * Math.abs(lat) && Math.abs(gap) < 0.3) {
        // turn round, the way whose arc bends it back toward the frame's middle in depth (turning the same way every time
        // would carry it off into the distance)
        const dep = -(s.px - s.cam.x) * rz + (s.pz - s.cam.z) * rx, bend = wz * -rz - wx * rx;
        s.hbase = f(s.hbase + Math.PI); s.sense = dep * bend > 0 ? -1 : 1;
      }
      want = s.hbase + (s.cam.yaw - cc.view) * D2R; gap = want - s.hd;
      gap -= TAU * Math.round(gap / TAU);
      if (s.sense !== 0 && Math.abs(gap) < 1) s.sense = 0;
      else if (s.sense * gap < 0) gap += s.sense * TAU;
      s.hd = f(s.hd + gap * (1 - Math.exp(-2 * dt)));
    }
    s.cx0 = cx; s.cz0 = cz;
    const sn = Math.sin(s.hd), k = Math.cos(s.hd);
    for (let i = 0; i < N; i++) {
      const m = i * 12;
      for (let r = 0; r < 4; r++) { const a = world[m + r], b = world[m + 4 + r], e = world[m + 8 + r]; out[m + r] = k * a + sn * e; out[m + 4 + r] = b; out[m + 8 + r] = -sn * a + k * e; }
      const tx = world[m + 3] - cx, tz = world[m + 11] - cz;
      out[m + 3] = k * tx + sn * tz + s.px; out[m + 11] = -sn * tx + k * tz + s.pz;
    }
  }

  // a call does not wait: a gesture or a rest loop playing ends now (crossfading into the way to the call), and so does a clip
  // with more than 1.2 s to go; a shorter transition finishes and then routes to it; a walk leaves at its stride's end
  function interrupt() {
    const c = clips[s.clip];
    if (c.loop && c.to === 'walk') return;                           // a walk leaves where a stride ends
    s.queue = [];
    if (c.loop || c.from === c.to || c.dur - s.t > 1.2) { s.pending = false; advance(); }
  }
  function attend() {
    if (byName[ATTEND] == null) { s.come = 0; return; }
    s.gesture = { name: ATTEND, clip: ATTEND, hold: ATTEND_S, started: false }; s.gestureT = 0; s.pending = true; s.come = 2;
    if (byName[ATTEND] === s.clip) { s.gesture.started = true; s.pending = false; }   // attending already: it holds
    interrupt();
  }

  const api = {
    nodes: rig.nodes, clips, world: out, clipWorld: world, pose: cur, get clip() { return clips[s.clip].name; }, get t() { return s.t; }, get cam() { return s.cam; }, get facing() { return s.hd - (s.cam.yaw - clips[s.clip].view) * Math.PI / 180; },
    get queue() { return s.queue.map((i) => clips[i].name); }, get offset() { return [s.ox, s.oz]; },
    mood(m) { if (!MOODS.includes(m) || m === s.mood) return; s.mood = m; s.pending = true; },
    /** Called (a tap, a shake): first its own reaction if given (a shake: 'shaken'), then it walks to the front of the frame facing the viewer and sits up attending (ATTEND), or,
     *  facing front near the middle already, just perks up; then back to its mood. act and play end a call. */
    come(first) {
      if (first) {                                     // its own reaction first (a shake's)
        const clip = g.gestures?.[first] ?? (byName[first] != null ? first : null);
        if (clip != null) { s.gesture = { name: first, clip, hold: g.hold?.[first] ?? 0, started: false }; s.gestureT = 0; s.pending = true; interrupt(); s.come = 4; return true; }
      }
      const cc = clips[s.clip], Y = s.cam.yaw * Math.PI / 180, TAU = 2 * Math.PI;
      let fc = s.hd - (s.cam.yaw - cc.view) * Math.PI / 180; fc -= TAU * Math.floor(fc / TAU + 0.5);
      const ox = s.px - s.cam.x, oz = s.pz - s.cam.z;
      if (Math.abs(fc) < 0.4 && Math.abs(ox * Math.cos(Y) - oz * Math.sin(Y)) < FRONT && cc.to !== 'walk' && cc.from !== 'walk') { attend(); return s.come !== 0; }
      const clip = g.gestures?.walk; if (clip == null) return false;
      s.gesture = { name: 'walk', clip, hold: g.hold?.walk ?? 0, started: false }; s.gestureT = 0; s.pending = true;
      interrupt();
      s.come = 1; s.comeT = 0;
      return true;
    },
    get coming() { return s.come !== 0; },
    /** Where it is in the frame: [across (screen x, world units), depth (away from the viewer)]. */
    get place() { const Y = s.cam.yaw * Math.PI / 180, ox = s.px - s.cam.x, oz = s.pz - s.cam.z; return [ox * Math.cos(Y) - oz * Math.sin(Y), -(ox * Math.sin(Y) + oz * Math.cos(Y))]; },
    act(name) {
      s.come = 0;
      const clip = g.gestures?.[name] ?? (byName[name] != null ? name : null);
      if (clip == null) return false;
      s.gesture = { name, clip, hold: g.hold?.[name] ?? 0, started: false }; s.gestureT = 0; s.pending = true; return true;
    },
    /** Jump straight to a clip (the preview's route player). */
    play(name) { s.come = 0; s.gesture = null; s.queue = []; start(byName[name], 0); },
    /** Queue a route from the current family to clip `name` (the preview's matrix). */
    goto(name) { s.gesture = { name, clip: name, hold: 0, started: false }; s.queue = []; s.pending = true; },
    update(dt) {
      const c = clips[s.clip];
      s.t += dt; s.blendT += dt;
      if (s.gesture?.started) s.gestureT += dt;
      // a held gesture (the walk) ends after its time; a clip gesture ends with its clip
      if (s.come === 4 && !s.gesture) { s.come = 0; api.come(); }       // its reaction done: now it comes
      else if (s.come >= 2 && s.come <= 3 && !s.gesture) s.come = 0;     // attended: back to its mood
      if (s.come !== 1 && s.gesture?.started && s.gesture.hold && s.gestureT >= s.gesture.hold && clips[s.clip].loop) { s.gesture = null; s.pending = true; }
      if (c.loop) {
        const sync = g.sync?.[c.name];
        if (s.pending) {
          if (sync == null) { s.pending = false; s.queue = []; advance(); }
          else {                                             // leave the walk only where a stride ends
            const nextT = s.t % sync;
            if (nextT < dt) { s.pending = false; s.queue = []; advance(); }
          }
        }
        if (s.clip === clips.indexOf(c) && s.t >= c.dur - EPS) {    // wrap: the root's travel over one cycle carries on
          const [ax, az] = rootAt(s.clip, c.dur - 1e-6), [bx, bz] = rootAt(s.clip, 0);
          s.ox += ax - bx; s.oz += az - bz; s.t -= c.dur;
        }
      } else if (s.t >= c.dur - EPS) {
        if (s.gesture?.started && byName[s.gesture.clip] === s.clip && !clips[s.clip].loop) s.gesture = null;
        s.t = c.dur; s.pending = false; advance();
      }
      samplePose(rig, rest, clips[s.clip], s.t, pose);
      const w = ease(s.blendD > 0 ? s.blendT / s.blendD : 1);
      if (w < 1) blendPose(from, pose, w, cur); else cur.set(pose);
      forward(rig, cur, s.ox, s.oz, world);
      // a light paw lock while blending: the paws that were on the floor are held where they were, fading out with the blend
      if (w < 1 && contacts.length) {
        let dx = 0, dz = 0, n = 0;
        for (const [node, at] of contacts) {
          const a = apply(fromWorld, node, at), b = apply(world, node, at);
          if (a[1] < 0.03 && b[1] < 0.03) { dx += a[0] - b[0]; dz += a[2] - b[2]; n++; }
        }
        if (n) { const k = (1 - w) / n; s.lockX = dx * k; s.lockZ = dz * k; forward(rig, cur, s.ox + s.lockX, s.oz + s.lockZ, world); }
      } else { s.lockX = 0; s.lockZ = 0; }
      turn(dt);
      return api;
    },
  };
  samplePose(rig, rest, clips[s.clip], 0, cur); s.blendT = s.blendD = 1;
  forward(rig, cur, 0, 0, world); out.set(world);
  return api;
}
