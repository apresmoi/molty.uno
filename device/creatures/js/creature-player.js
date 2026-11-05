// The creature player (docs/CREATURES.md): plays one creature package, data only. It knows channels, terms and a
// fixed set of factor ops, never what a duck, a cat or an octopus is. This is the JS reference for the C port.
//
//   const c = creaturePlayer(pkg.creature, { actionPose, ACTIONS });
//   c.move.move(p, walkState)          the walk engine's current cycle (q, k, go, along, side, fz, up)
//   c.gestures                         { name: seconds } for every gesture it plays (a shared one's length is ACTIONS')
//   c.pose(p, dt, ctx)                 every layer, in order; writes the shared pose back into p (ctx: action, prevAction, work)
//   c.apply(bones)                     adds this frame's bone outputs to bones reset to rest (Map id -> {rotation, position, scale})
//   c.tints()                          [[paletteName, '#rrggbb'], ...] for this frame
//   c.ambient(env)                     particles and currents on the glass (env: dt, cov, W, H, win, mark, impulse, push)
const TAU = Math.PI * 2;
const cl = (x, a = 0, b = 1) => (x < a ? a : x > b ? b : x), ss = (x) => { x = cl(x); return x * x * (3 - 2 * x); };
const bump = (x, a, b) => (x < a || x > b ? 0 : Math.sin(Math.PI * (x - a) / (b - a))), fract = (x) => x - Math.floor(x);
// integer hash (murmur3 fmix32) of a real number at 1/16 resolution: the same bits in C
export const hash = (n) => { let h = (Math.floor(n * 16) | 0) ^ 0x9e3779b9; h = Math.imul(h ^ (h >>> 16), 0x85ebca6b); h = Math.imul(h ^ (h >>> 13), 0xc2b2ae35); return ((h ^ (h >>> 16)) >>> 0) / 4294967296; };
const noise = (x, k) => { const i = Math.floor(x), u = ss(x - i); return (hash(i + k) * (1 - u) + hash(i + 1 + k) * u) * 2 - 1; };

// factor ops: x is the source value, then the params. Phases of the cycle ops (cyc bumpr step) are in cycles, sin's in radians.
const OPS = {
  v: (x) => x,
  sin: (x, hz = 1, ph = 0) => Math.sin(TAU * hz * x + ph),
  nz: (x, hz = 1, k = 0, off = 0) => noise(hz * x + off, k),
  frac: (x, hz = 1, ph = 0) => fract(hz * x + ph),
  env: (x, d, a, r) => ss(x / a) * ss((d - x) / r),
  bump: (x, a, b) => bump(x, a, b),
  ss: (x, a, len) => ss((x - a) / len),
  ssd: (x, a, len) => 1 - ss((x - a) / len),
  exp: (x, k, at = 0) => Math.exp(-k * Math.max(0, x - at)),
  cyc: (x, hz, ph, up) => { const u = fract(hz * x + ph); return u < up ? ss(u / up) : 1 - ss((u - up) / (1 - up)); },
  bumpr: (x, hz, ph, a, b) => bump(fract(hz * x + ph), a, b),
  step: (x, hz, ph, d) => { const u = fract(hz * x + ph); return u < d ? -1 + 2 * u / d : Math.cos(Math.PI * (u - d) / (1 - d)); },
  pulse: (x, hz, k, w) => { const y = x * hz + 0.3 * noise(x * 0.6, k), i = Math.floor(y), f = y - i; return f < w ? Math.sin(Math.PI * f / w) * (0.55 + 0.45 * hash(i + k)) : 0; },
  dz: (x, hz, k) => { const v = noise(x * hz, k); return Math.sign(v) * Math.max(0, Math.abs(v) - 0.35) / 0.65; },
  steps: (x, per, k) => { const i = Math.floor(x / per), s = ss((x - i * per) / 0.09); return hash(i - 1 + k) * (1 - s) + hash(i + k) * s; },
  chance: (x, per, k, p, off = 0) => (hash(Math.floor((x - off) / per) + k) < p ? 1 : 0),
  flap: (x) => { const c = Math.cos(TAU * x); return Math.pow(0.5 - 0.5 * c, c < 0 ? 0.7 : 1.4); },
  lt: (x, a) => (x < a ? 1 : 0),
  gt: (x, a) => (x > a ? 1 : 0),
  is: (x, a) => (Math.round(x) === Math.round(a) ? 1 : 0),
  sgn: (x) => (x < 0.5 ? -1 : 1),
  lin: (x, a, b) => a + b * x,
  clamp: (x, a, b) => cl(x, a, b),
  pow: (x, e) => Math.sign(x) * Math.abs(x) ** e,
  rcp: (x, a = 0, b = 1) => { const d = a + b * x; return 1 / (Math.abs(d) < 1e-3 ? (d < 0 ? -1e-3 : 1e-3) : d); },
  asin: (x) => Math.asin(cl(x, -1, 1)),
  fl: (x, k = 1) => Math.floor(k * x),
};
const MOD = { abs: Math.abs, pos: (x) => Math.max(0, x), neg: (x) => Math.max(0, -x), sq: (x) => x * x };
export const FACTOR_OPS = Object.keys(OPS).concat('dl');
const MOODS = ['idle', 'listening', 'thinking', 'speaking', 'happy', 'sleepy', 'surprised'], EYES = ['wide', 'happy', 'sleepy', 'squint'];
const POSE = ['pos.x', 'pos.y', 'pos.z', 'rot.x', 'rot.y', 'rot.z', 'head.x', 'head.y', 'head.z', 'spine.x', 'spine.y', 'spine.z', 'squash', 'lid', 'mouth', 'look.x', 'look.y', 'prowl'];
const WALK = ['q', 'k', 'go', 'along', 'side', 'fz', 'up'], PROPS = ['rx', 'ry', 'rz', 'px', 'py', 'pz', 'sx', 'sy', 'sz', 's', 'sq'];
const UNSHARE = ['pos.x', 'pos.y', 'pos.z', 'rot.x', 'rot.y', 'rot.z', 'head.x', 'head.y', 'head.z', 'spine.x', 'spine.y', 'spine.z', 'squash', 'look.x', 'look.y'];
const hexOf = (c) => { const n = parseInt(c.slice(1), 16); return [n >> 16 & 255, n >> 8 & 255, n & 255]; };
const toHex = (c) => '#' + c.map((v) => Math.round(cl(v, 0, 255)).toString(16).padStart(2, '0')).join('');

export function creaturePlayer(cr, { actionPose, ACTIONS } = {}) {
  const C = Object.create(null), hist = Object.create(null), state = Object.create(null); // channel values, histories, stateful channel state
  const groups = cr.groups ?? {}, B = { a: 0, dur: 0, seed: 0, w: 1 }; let I = null, IL = null, n = 0, frameFactors = 0; // the block running now, the group instance, its locals
  const keepers = new Set(); // channels with `keep` hold last frame's value until they run again
  const out = new Map(); // bone -> Float32Array(PROPS) this frame
  const draw = (slot) => hash(B.seed * 7.31 + slot * 1.37);
  let ranges = 0; // each [lo, hi] param gets its own draw slot
  const param = (v) => {
    if (typeof v === 'number') return () => v;
    if (v === 'D') return () => B.dur;
    if (Array.isArray(v)) { const [lo, hi, k] = v, s = k ?? 1000 + ranges++; return () => lo + (hi - lo) * draw(s); } // [lo, hi, slot]: one draw shared by every range with that slot
    if (typeof v === 'string' && v[0] === '$') { const k = v.slice(1); return () => (k === 'i' ? I.i : I.g.attr[k][I.i]); }
    if (v && typeof v === 'object') { const e = Object.entries(v).map(([m, c]) => [m === '_' ? null : 'w.' + m, c]); return () => e.reduce((s, [m, c]) => s + (m ? c * C[m] : c), 0); }
    throw new Error('bad param ' + JSON.stringify(v));
  };
  const source = (s) => {
    if (typeof s === 'number') return () => s;
    if (s === 'a') return () => B.a;
    if (s[0] === '~') { const k = +s.slice(1); return () => draw(k); }
    if (s[0] === '$') return param(s);
    if (s[0] === '@' && s[1] === '%') { const k = s.slice(1); return () => IL[k] ?? 0; }
    if (s[0] === '@') { const k = s.slice(1); return () => C[k] ?? 0; }
    throw new Error('bad source ' + s);
  };
  const factor = (f) => {
    if (typeof f === 'string') { const g = source(f); return () => { frameFactors++; return g(); }; }
    const [op0, src, ...ps] = f, [mod, op] = op0.includes(':') ? op0.split(':') : [null, op0], x = source(src);
    let fn;
    if (op === 'dl') { const k = src.slice(1), d = param(ps[0]); fn = () => { const h = hist[k]; if (!h) return C[k] ?? 0; const at = h.t - d(); for (let j = h.v.length - 1; j >= 0; j--) if (h.v[j][0] <= at) return h.v[j][1]; return h.v[0]?.[1] ?? 0; }; }
    else { const o = OPS[op]; if (!o) throw new Error('unknown op ' + op); const P = ps.map(param);
      fn = P.length === 0 ? () => o(x()) : P.length === 1 ? () => o(x(), P[0]()) : P.length === 2 ? () => o(x(), P[0](), P[1]()) : () => o(x(), ...P.map((p) => p())); }
    if (mod && !MOD[mod]) throw new Error('unknown modifier ' + mod);
    const m = MOD[mod]; return m ? () => { frameFactors++; return m(fn()); } : () => { frameFactors++; return fn(); };
  };
  const term = (t) => { const [g, ...fs] = t, G = param(g), F = fs.map(factor); return F.length === 0 ? G : F.length === 1 ? () => { const g = G(); return g === 0 ? 0 : g * F[0](); } : () => { let v = G(); for (const f of F) { if (v === 0) return 0; v *= f(); } return v; }; }; // a zero ends the product early
  // a channel: name, terms, options. Its value is the sum of its terms, then state (int lp sp d), clamp, then op into the store
  const channel = ([name, ts, o = {}]) => {
    const stateful = o.int || o.lp != null || o.sp || o.d, T = ts.map(term), id = n++, op = o.op ?? (stateful ? '=' : '+'), lim = o.cl, lp = o.lp != null && param(o.lp), d = o.d, sp = o.sp, keep = o.hist;
    if (o.keep) keepers.add(name);
    const local = name[0] === '%', bone = name.includes(':') && name.split(':'), bprop = bone && PROPS.indexOf(bone[1]);
    if (bone && bprop < 0) throw new Error('unknown bone prop ' + name);
    const scale = bone && bprop >= 6;
    return () => {
      let v = 0; for (const t of T) v += t();
      const st = state, key = I ? id + ':' + I.i : id;
      if (o.int) { v = (st[key] ?? 0) + v * dt; st[key] = v; }
      if (d) { const s = (st[key] ??= { p: null, v: 0 }); const r = s.p === null || !dt ? 0 : cl((v - s.p) / dt, d[0], d[1]); s.p = v; v = r; }
      if (lp) { const s = st['lp' + key]; v = s === undefined ? v : s + (v - s) * (1 - Math.exp(-dt * lp())); st['lp' + key] = v; }
      if (sp) { const s = (st['sp' + key] ??= [v, 0]), target = v; for (let k = 0; k < 4; k++) { s[1] += (sp[0] * (target - s[0]) - sp[1] * s[1]) * dt / 4; s[0] += s[1] * dt / 4; } v = o.lag ? s[0] - target : s[0]; }
      if (lim) v = cl(v, lim[0], lim[1]);
      const w = stateful || name[0] === '_' || local ? 1 : B.w; // a block's weight scales what it writes to real channels, never its own working values
      if (bone) { const nm = bone[0] === '' ? I.g.bones[I.i] : bone[0]; let a = out.get(nm); if (!a) { a = new Float32Array(11); a[6] = a[7] = a[8] = a[9] = 1; out.set(nm, a); }
        if (bprop === 10) { const y = 1 + w * v, xz = 1 / Math.sqrt(Math.max(0.3, y)); a[7] *= y; a[6] *= xz; a[8] *= xz; }
        else if (scale) { const f = op === '=' ? v : 1 + w * v; if (bprop === 9) { a[6] *= f; a[7] *= f; a[8] *= f; } else a[bprop] *= f; }
        else a[bprop] += w * v; return; }
      const S = local ? IL : C, cur = S[name] ?? 0;
      S[name] = op === '+' ? cur + w * v : op === 'max' ? Math.max(cur, w * v) : op === '*' ? cur * (1 + w * v) : cur + w * (v - cur);
      if (keep) { const h = (hist[name] ??= { t: 0, v: [] }); h.t = time; h.v.push([time, S[name]]); while (h.v.length > 2 && h.v[1][0] < time - keep) h.v.shift(); }
    };
  };
  const list = (chs) => chs.map((c) => (c.each ? fan(c) : channel(c)));
  const fan = ({ each, ch }) => { const g = groups[each]; if (!g) throw new Error('unknown group ' + each); const F = list(ch), inst = g.bones.map((_, i) => ({ g, i }));
    return () => { for (const q of inst) { I = q; IL = {}; for (const f of F) f(); } I = IL = null; }; };
  const block = (b) => ({ run: list(b.ch ?? []), b });

  // the layers, in play order: walk, events, base, gestures, moods, post
  const walk = cr.walk ?? {}, L = { walk: block(walk), base: block({ ch: cr.base }), post: block({ ch: cr.post }) };
  const gest = Object.fromEntries(Object.entries(cr.gestures ?? {}).map(([k, g]) => [k, block(g)]));
  const gdur = Object.fromEntries(Object.entries(cr.gestures ?? {}).map(([k, g]) => [k, ACTIONS?.[k] ?? g.dur ?? 1])); // a shared gesture keeps its length; the creature's own carry theirs
  const moods = Object.fromEntries(Object.entries(cr.moods ?? {}).map(([k, m]) => [k, block(m)]));
  const events = Object.entries(cr.events ?? {}).map(([name, e]) => { const pick = e.pick ? Object.entries(e.pick).map(([k, m]) => ({ k, len: m.len ?? e.len, run: list(m.ch) })) : [{ k: name, len: e.len, run: list(e.ch) }];
    return { name, e, pick, gate: e.gate && list([['_gate', e.gate]])[0], wt: e.w && list([['_w', e.w]])[0], stop: e.stop && list([['_stop', e.stop]])[0], gap: param(e.gap ?? 0), len: pick.map((p) => param(p.len ?? 1)), run: null, next: e.first ?? 0, n: 0 }; });
  const surge = walk.surge && list([['_surge', walk.surge]])[0];
  const tints = (cr.tints ?? []).map((t) => ({ ...t, base: null }));
  let time = 0, dt = 0, mood = 'idle', wm = {}, walkIn = {};
  for (const m of MOODS) { C['w.' + m] = C['w6.' + m] = m === 'idle' ? 1 : 0; C['m.' + m] = m === 'idle' ? 0 : 99; }
  const sub = (fn, val) => { const keep = C[val]; C[val] = 0; fn(); const v = C[val]; C[val] = keep; return v; };
  const at = (blk, a, dur, seed, w, run) => { B.a = a; B.dur = dur; B.seed = seed; B.w = w; for (const f of run) f(); B.w = 1; B.seed = 0; B.a = time; };

  return {
    move: { hz: walk.hz ?? 1, stride: walk.stride ?? 0, pace: walk.pace ?? 0, burst: !!walk.burst, radial: !!walk.radial,
      surge: (q) => { if (!surge) return 1; C['walk.q'] = q; return sub(surge, '_surge'); }, move(p, m) { walkIn = m; } },
    quad: walk.quad,
    get factors() { return frameFactors; },
    gestures: gdur,
    pose(p, step, ctx = {}) {
      dt = step; time += dt; frameFactors = 0; out.clear();
      for (const k in C) if (!keepers.has(k) && !/^(w6?|m)\./.test(k)) C[k] = 0; // every channel starts the frame at 0 but the kept ones, the mood weights and clocks
      for (const k of POSE) { const [a, b] = k.split('.'); C[k] = b ? p[a][b] : p[a] ?? 0; }
      const was = mood; mood = p.mood ?? 'idle'; if (mood !== was) { C['m.' + mood] = 0; wm[mood] = (wm[mood] ?? 0) + 1; }
      for (const m of MOODS) { const on = m === mood ? 1 : 0; C['is.' + m] = on; C['w.' + m] += (on - C['w.' + m]) * (1 - Math.exp(-dt * 4)); C['w6.' + m] += (on - C['w6.' + m]) * (1 - Math.exp(-dt * 6)); C['now.' + m] = on || C['w.' + m]; if (m !== mood) C['m.' + m] += dt; }
      C['m.' + mood] += dt; for (const e of EYES) C['eye.' + e] = p.eye === e ? 1 : 0;
      for (const k of WALK) C['walk.' + k] = walkIn[k] ?? 0; C.t = time; C.dt = dt; const act = ctx.action, prev = ctx.prevAction;
      C.act = act && act.name !== 'inspect' ? 1 : 0; C.work = ctx.work ? 1 : 0;
      at(L.walk, C['walk.q'], 1, 0, 1, L.walk.run);
      for (const E of events) {
        if (E.run) { E.run.a += dt; if (E.run.a > E.run.len || (E.stop && sub(E.stop, '_stop') > 0.5)) { E.run = null; B.seed = time * 3.7 + E.n; E.next = time + E.gap(); } }
        if (!E.run) { const g = E.gate ? sub(E.gate, '_gate') : 1, ok = E.e.rate ? hash(time * 61.7 + E.n * 3.1) < E.e.rate * g * dt : time >= E.next && g > 0.5;
          if (ok) { const s = ++E.n * 17.3 + E.name.length; B.seed = s; const i = Math.floor(hash(s * 0.71) * E.pick.length); E.run = { i, a: 0, seed: s, len: (B.seed = s, E.len[i]()) }; } }
        if (E.run) { const w = E.wt ? sub(E.wt, '_w') : 1; at(E, E.run.a, E.run.len, E.run.seed, w, E.pick[E.run.i].run); }
      }
      at(L.base, time, 0, 0, 1, L.base.run);
      for (const [a, w] of [[prev, prev?.w ?? 0], [act, 1]]) { const g = a && gest[a.name]; if (!g || w <= 0) continue;
        if (g.b.replace !== false && actionPose) { const o = actionPose(a.name, a.t); for (const k of UNSHARE) { const [x, y] = k.split('.'); C[k] -= w * (y ? o[x][y] : o[x]); } }
        at(g, a.t, gdur[a.name], a.seed ??= (a.t * 977 + time * 31) % 9973, w, g.run); }
      for (const [m, b] of Object.entries(moods)) { const w = b.b.now ? C['now.' + m] : C['w.' + m]; if (w > 0.01) at(b, C['m.' + m], 0, (wm[m] ?? 0) * 13.7 + MOODS.indexOf(m), w, b.run); }
      at(L.post, time, 0, 0, 1, L.post.run);
      for (const k of POSE) { const [a, b] = k.split('.'); if (b) p[a][b] = C[k]; else p[a] = C[k]; }
    },
    apply(bones) { for (const [id, a] of out) { const g = bones.get(id); if (!g) continue; g.rotation.x += a[0]; g.rotation.y += a[1]; g.rotation.z += a[2]; g.position.x += a[3]; g.position.y += a[4]; g.position.z += a[5]; g.scale.x *= a[6]; g.scale.y *= a[7]; g.scale.z *= a[8]; } },
    // depth: a bone's place down its chain (bones' "d"), for colour that ripples down the body
    tints(palette, depth = 0) { const res = {}; for (const [name, hex] of Object.entries(palette)) { let c = hexOf(hex);
        for (const t of tints) { const to = t.to?.[name], neg = t.neg?.[name]; if (!to && !neg) continue; let v = C[t.ch] ?? 0; if (t.wave) v = cl(v) * t.wave[2] * Math.max(0, Math.sin(TAU * (t.wave[0] * time - t.wave[1] * depth))) ** 2;
          v = cl(v, -1, 1); const goal = v >= 0 ? to : neg; if (!goal || !v) continue; const g = hexOf(goal), k = Math.abs(v); c = c.map((x, i) => x + (g[i] - x) * k); }
        res[name] = toHex(c); } return res; },
    get tinted() { return tints.length > 0; },
    ambient: ambientOf(cr.ambient, C, () => time),
  };
}

// The glass around the creature: particles ("bubble", "blot", "mote") from emitters, and a current that stirs the fluid.
// The body is the creature's own coverage this frame (env.cov), so particles pass behind it.
function ambientOf(A, C, now) {
  if (!A) return () => {};
  let seq = 1; const rnd = () => hash(seq++ * 0.7131 + 3.3), live = [], motes = [], edge = {}, next = {};
  const R = (v) => (Array.isArray(v) ? v[0] + (v[1] - v[0]) * rnd() : v ?? 0);
  return (env) => {
    const { dt, cov, W, H, win, mark, impulse, push } = env, t = now();
    const cell = (x, y) => { const i = Math.floor((x / win + 1) / 2 * W), j = Math.floor((1 - y / win) / 2 * H); return i < 1 || j < 1 || i >= W - 1 || j >= H - 1 ? -1 : j * W + i; };
    const body = (x, y) => { const k = cell(x, y); return k >= 0 && (cov[k] > 0 || cov[k + 1] > 0 || cov[k - 1] > 0 || cov[k + W] > 0 || cov[k - W] > 0); };
    let sx = 0, sy = 0, sn = 0, top = -1, bot = 1; for (let k = 0; k < W * H; k++) if (cov[k] > 0) { const x = ((k % W + 0.5) / W * 2 - 1) * win, y = (1 - ((k / W | 0) + 0.5) / H * 2) * win; sx += x; sy += y; sn++; if (y > top) top = y; if (y < bot) bot = y; }
    const ox = sn ? sx / sn : 0, oy = sn ? sy / sn : 0; top = Math.min(top, oy + 0.5); bot = Math.max(bot, oy - 0.45);
    const AT = { floor: () => { const x = (rnd() * 2 - 1) * 0.6; return [x, -Math.sqrt(Math.max(0, 0.8 - x * x))]; }, side: () => [ox + (top - oy) * 0.55, oy + (top - oy) * 0.15], top: () => [ox + 0.05, top - 0.05], bottom: () => [ox, bot], center: () => [ox, oy - 0.05] };
    for (const [id, e] of Object.entries(A.emit ?? {})) {
      let n = 0; const v = e.on ? C[e.on] ?? 0 : 0;
      if (e.every) { if (t > (next[id] ?? 0)) { n = 1; next[id] = t + R(e.every); } }
      else if (e.on) { if (v > 0.5 && !edge[id] && sn) n = 1; edge[id] = v > 0.5; }
      else if (e.rate) { const k = C[e.rate[0]] ?? 0; if (k > e.rate[2] && rnd() < e.rate[1] * dt * k && sn) n = 1; }
      if (!n) continue; if (e.impulse && sn) { const [x, y] = AT[e.at](); impulse(x / win, -y / win, e.impulse[0], e.impulse[1]); }
      const cnt = Math.round(R(e.n ?? 1)); for (let c = 0; c < cnt; c++) { const [x0, y0] = AT[e.at](), x = x0 + R(e.dx), y = y0 + R(e.dy) - (e.stack ?? 0) * c, r = e.rp ? e.r[0] + (e.r[1] - e.r[0]) * rnd() ** e.rp : R(e.r), P = A.kinds[e.p];
        const q = { kind: e.p, P, x, y, r, age: -(e.stagger ?? 0) * c, ph: rnd() * TAU };
        if (P.type === 'bubble') Object.assign(q, { vy: P.rise[0] + P.rise[1] * r + P.rise[2] * rnd(), f: R(P.f), wob: R(P.wob) });
        if (P.type === 'blot') Object.assign(q, { vx: R(P.vx), pts: Array.from({ length: P.pts }, () => { const u = Math.sqrt(rnd()), an = rnd() * TAU; return [0.4 * u * Math.cos(an), 0.4 * u * Math.sin(an), 0.25 + 0.2 * rnd()]; }) });
        live.push(q); if (live.length > 64) live.shift(); if (e.twin && rnd() < e.twin) live.push({ ...q, x: q.x + 0.05, y: -0.75, r: 0.03, vy: P.rise[0] + P.rise[1] * 0.03 }); }
    }
    for (let i = live.length - 1; i >= 0; i--) { const q = live[i], P = q.P; q.age += dt; if (q.age < 0) continue;
      if (P.type === 'bubble') { q.vy += P.acc * dt; q.y += q.vy * dt; const x = q.x + q.wob * Math.sin(q.ph + q.f * TAU * q.age * 0.5), lim = Math.sqrt(Math.max(0, P.glass - x * x)) - q.r;
        if (q.y > lim) { if (q.pop === undefined) { q.pop = P.pop[0]; if (q.r > P.pop[2]) impulse(x / win, -q.y / win, P.pop[3], P.pop[4]); } q.pop -= dt; if (q.pop > 0) mark({ kind: 'star', x, y: q.y, size: 2.6 * q.r * (1.3 - q.pop * 2), rot: q.ph, color: P.pop[1], d: 5 }); else live.splice(i, 1); continue; }
        if (!body(x, q.y)) { mark({ kind: 'dot', x, y: q.y, size: 2 * q.r, color: q.r > P.big ? P.color[1] : P.color[0], d: 5 }); if (q.r > P.shine[0]) mark({ kind: 'dot', x: x - 0.3 * q.r, y: q.y + 0.35 * q.r, size: 0.6 * q.r, color: P.shine[1], d: 6 }); } }
      else if (P.type === 'blot') { if (q.age > P.life) { live.splice(i, 1); continue; } q.x += q.vx * dt; q.y -= P.sink * dt; q.ph += P.spin * dt; const s = q.r * (0.5 + 2.4 * ss(q.age / 0.5)) * (1 - ss((q.age - 1.4) / 1.2));
        for (const [u, v, z] of q.pts) { const x = q.x + s * (u * Math.cos(q.ph) - v * Math.sin(q.ph)), y = q.y + s * (u * Math.sin(q.ph) + v * Math.cos(q.ph)); if (!body(x, y)) mark({ kind: 'blot', x, y, size: s * z, rot: q.ph + u, color: P.color, d: 2 }); } } }
    if (A.motes) { const M = A.motes; if (!motes.length) for (let i = 0; i < M.n; i++) motes.push({ x: (hash(i * 3.1) * 2 - 1) * 0.8, y: (hash(i * 7.7) * 2 - 1) * 0.8, k: hash(i * 1.3) });
      for (const m of motes) { m.y -= (M.fall[0] + M.fall[1] * m.k) * dt; m.x += M.drift * noise(t * 0.2 + m.k * 9, 5) * dt; if (m.y < -0.85) { m.y = 0.85; m.x = (hash(t + m.k) * 2 - 1) * 0.7; }
        if (Math.hypot(m.x, m.y) < 0.82 && !body(m.x, m.y)) mark({ kind: 'dot', x: m.x, y: m.y, size: M.size[0] + M.size[1] * m.k, color: m.k > 0.6 ? M.color[1] : M.color[0], d: 1 }); } }
    if (A.current) { const [x, y, vx, vy] = A.current.ch.map((k) => C[k] ?? 0); push(x, y, vx, vy, A.current.r); }
  };
}
