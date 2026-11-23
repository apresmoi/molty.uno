// SPDX-License-Identifier: MIT
// A Molty's `life` (docs/CREATURE.md, "Life"): its signals, and the layers they move over its mood loops, checked and
// compiled into what the device runs (docs/CREATURES.md section 12). The signals become channels and events of the
// creature player's engine (the one the duck's formulas run on), each after what it reads; the layers, the clip
// player's. The firmware sorts nothing and draws its own randomness: the same seed plays the same frames.
import { where } from '../display/load.mjs';

/** What the device holds (main/creature/creature_player.h CP_LIFE_STEPS, creature_clip.h CC_LAYERS_MAX). */
export const LIFE_LIMITS = { signals: 64, layers: 16, steps: 600 };
const MOODS = ['idle', 'listening', 'thinking', 'speaking', 'happy', 'sleepy', 'surprised'];
// the device's signals this firmware gives, by the name a Molty reads them by, and the engine's name for each
const DEVICE = { t: 't', free: 'free', idle_time: 'idle_time', life: 'life', working: '_working',
  ...Object.fromEntries(MOODS.flatMap((m) => [[m, `w.${m}`], [`mood_${m}`, `w.${m}`]])) };
const LATER = ['voice_out', 'voice_in', 'talking', 'heard_start', 'heard_pause', 'heard_question', 'said_start', 'said_end', 'emote',
  'touch', 'touch_x', 'shake', 'tilt_x', 'tilt_y', 'motion', 'hour', 'battery'];
const KINDS = ['mood', 'from', 'sum', 'wave', 'noise', 'hold', 'every', 'follow'];
const OPTIONS = { mood: [], from: ['map'], sum: [], wave: ['vary', 'rise'], noise: [], hold: ['range', 'move', 'home', 'waits'], every: ['lasts', 'double'], follow: ['lag', 'bounce'] };
const ANY = ['when', 'range', 'scale', 'ease'];
const NOT_YET = { on: 'a pulse on a trigger (on)', peaks: 'peaks of a level (peaks)' };
const WEIGHT_EASE = 6;   // a layer's weight settles in about 0.8 s
const list = (xs) => xs.join(', ') || 'none';

/** A spring (the engine's `sp`) that settles at rate w (1/s), damped zeta. */
const spring = (w, zeta = 1) => ({ sp: [+(w * w).toFixed(4), +(2 * zeta * w).toFixed(4)] });

/**
 * Check and compile doc.life against the body (nodes, action names). bad(at, message) reports. Returns the package's
 * `life` ({ ch, events, layers }), its cost in engine steps, and the clips the layers play; null when there is none.
 */
export function compileLife(doc, nodes, actions, bad) {
  const life = doc.life;
  if (!life) return null;
  const sigs = life.signals ?? {}, names = Object.keys(sigs), at = (n) => where(sigs, n) || where(life, 'signals') || 'molty.json:1';
  if (life.moments) bad(where(life, 'moments'), 'life.moments: moments (gestures started by a signal) are not in Molty\'s firmware yet: leave them out');
  if (names.length > LIFE_LIMITS.signals) bad(where(life, 'signals'), `life.signals has ${names.length} signals: a Molty holds ${LIFE_LIMITS.signals}`);
  for (const n of names) if (n in DEVICE || LATER.includes(n) || ['dt', 'act', 'work'].includes(n)) bad(at(n), `signal ${n}: ${n} is a name the device uses; call yours something else`);

  const ch = [], events = {}, deps = new Map(), main = new Map(), extra = [];
  let key = 0, eased = false;
  const known = (s) => sigs[s] !== undefined || s in DEVICE;
  /** A signal read by name, as the engine's slot; `who` reads it now unless late (last frame's value is enough). */
  const ref = (who, s, late = false) => {
    if (typeof s !== 'string' || !known(s)) {
      const later = LATER.includes(s);
      bad(at(who), `signal ${who} reads ${JSON.stringify(s)}, which ${later ? 'Molty\'s firmware does not give yet' : `is not a signal: name one of its signals (${list(names)}) or the device's (t, free, idle_time, life, working, a mood's name or mood_<mood>)`}`);
      return '@t';
    }
    if (s === 'working') eased = true;
    if (!late && sigs[s] !== undefined) deps.get(who)?.add(s);
    return `@${s in DEVICE ? DEVICE[s] : s}`;
  };
  /** A number, a signal's name or a mood table, as terms ([g, ...factors] summed). */
  const val = (who, v, what) => {
    if (typeof v === 'number') return [[v]];
    if (typeof v === 'string') return [[1, ref(who, v)]];
    if (v && typeof v === 'object' && !Array.isArray(v) && Object.values(v).every((x) => typeof x === 'number')) {
      const odd = Object.keys(v).filter((m) => m !== '_' && m !== 'working' && !MOODS.includes(m));
      if (odd.length) bad(at(who), `signal ${who}: ${what} names ${list(odd)}, which is not a mood: a mood table names moods (${list(MOODS)}), working, and _ for the rest`);
      const d = v._ ?? 0, mix = { _: d, ...Object.fromEntries(MOODS.filter((m) => m in v).map((m) => [m, v[m] - d])) };
      if (!('working' in v)) return [[mix]];
      eased = true;
      return [[mix], [v.working, '@_working'], [scaleG(mix, -1), '@_working']];
    }
    bad(at(who), `signal ${who}: ${what} must be a number, a signal's name or a mood table ({ "_": 1, "sleepy": 0.5 })`);
    return [[0]];
  };
  /** A range [lo, hi], or a mood table of them: { lo, span } as terms. */
  const range = (who, v, what) => {
    const pair = (p) => Array.isArray(p) && p.length === 2 && p.every((x) => typeof x === 'number');
    if (pair(v)) return { lo: [[v[0]]], span: [[v[1] - v[0]]] };
    if (v && typeof v === 'object' && !Array.isArray(v) && Object.values(v).every(pair)) {
      const side = (i) => Object.fromEntries(Object.entries(v).map(([m, p]) => [m, i ? p[1] - p[0] : p[0]]));
      return { lo: val(who, side(0), what), span: val(who, side(1), what) };
    }
    bad(at(who), `signal ${who}: ${what} must be [least, most], or a mood table of them ({ "_": [3, 7], "sleepy": [8, 14] })`);
    return { lo: [[0]], span: [[0]] };
  };
  const num = (who, v, what, lo, hi, def) => {
    if (v === undefined) return def;
    if (typeof v !== 'number' || v < lo || v > hi) { bad(at(who), `signal ${who}: ${what} must be a number from ${lo} to ${hi}`); return def; }
    return v;
  };

  // each signal into the engine's channels (and an event for hold and every)
  for (const [n, def] of Object.entries(sigs)) {
    deps.set(n, new Set());
    if (!def || typeof def !== 'object' || Array.isArray(def)) { bad(at(n), `signal ${n}: write it as { "<kind>": ... }: a kind is one of ${list(KINDS)}`); continue; }
    const kinds = Object.keys(def).filter((k) => KINDS.includes(k) || k in NOT_YET);
    if (kinds.length !== 1) { bad(at(n), `signal ${n}: say what kind it is, one of ${list(KINDS)}${kinds.length ? ` (it says ${list(kinds)})` : ''}`); continue; }
    const kind = kinds[0];
    if (kind in NOT_YET) { bad(at(n), `signal ${n}: ${NOT_YET[kind]} is not in Molty's firmware yet`); continue; }
    for (const k of Object.keys(def)) if (k !== kind && !OPTIONS[kind].includes(k) && !ANY.includes(k)) bad(at(n), `signal ${n}: a ${kind} signal does not take ${k} (it takes ${list([...OPTIONS[kind], ...ANY])})`);
    if (Array.isArray(def.ease)) bad(at(n), `signal ${n}: ease with two numbers (up and down) is not in Molty's firmware yet: give one`);
    const k = key += 7, slot = `_${n}`;
    let terms, opts = {};
    switch (kind) {
      case 'mood': terms = val(n, def.mood, 'mood'); break;
      case 'from': {
        const x = ref(n, def.from), map = def.map;
        if (map === undefined) { terms = [[1, x]]; break; }
        if (!Array.isArray(map) || map.length < 2 || !map.every((p) => Array.isArray(p) && p.length === 2 && p.every((v) => typeof v === 'number')) || map.some((p, i) => i && p[0] <= map[i - 1][0])) {
          bad(at(n), `signal ${n}: map must be two or more [from, to] points, from rising: [[0, 0.2], [1, 0.4]]`); terms = [[0]]; break;
        }
        // a straight line between points, flat beyond the ends: the first value, plus each stretch's slope times how far
        // into it (clamped to it, less its start: the starts summed into the first term)
        terms = [[map[0][1]]];
        for (let i = 0; i + 1 < map.length; i++) {
          const [x0, y0] = map[i], [x1, y1] = map[i + 1], s = (y1 - y0) / (x1 - x0);
          if (s) { terms.push([s, ['clamp', x, x0, x1]]); terms[0][0] -= s * x0; }
        }
        break;
      }
      case 'sum':
        if (!Array.isArray(def.sum) || !def.sum.length || !def.sum.every((p) => Array.isArray(p) && p.length === 2 && typeof p[1] === 'number')) { bad(at(n), `signal ${n}: sum must be [[signal, amount], ...]`); terms = [[0]]; break; }
        terms = def.sum.map(([s, a]) => [a, ref(n, s)]);
        break;
      case 'wave': case 'noise': {
        // its phase runs at the rate (a little faster or slower as it goes: vary), so a change of rate never jumps
        const rate = val(n, def[kind], 'its rate'), vary = num(n, def.vary, 'vary', 0, 1, 0);
        const wobble = vary ? rate.map((t) => [...t, ['nz', '@t', 0.37, k, [0, 1000]]]).map(([g, ...f]) => [scaleG(g, vary), ...f]) : [];
        extra.push({ who: n, ch: [`${slot}.ph`, [...rate, ...wobble], { int: 1, keep: 1 }] });
        terms = kind === 'wave' ? [[2, ['cyc', `@${slot}.ph`, 1, [0, 1], num(n, def.rise, 'rise', 0.05, 0.95, 0.5)]], [-1]] : [[1, ['nz', `@${slot}.ph`, 1, k, [0, 1000]]]];
        break;
      }
      case 'follow': {
        // a spring: lag is how far behind it runs, bounce how far past it swings (0: none)
        const lag = num(n, def.lag, 'lag', 0.02, 5, 0.25), b = num(n, def.bounce, 'bounce', 0, 0.9, 0);
        const zeta = b > 0 ? -Math.log(b) / Math.sqrt(Math.PI ** 2 + Math.log(b) ** 2) : 1;
        terms = [[1, ref(n, def.follow, true)]]; opts = spring(2 * zeta / lag, zeta);
        break;
      }
      case 'hold': case 'every': {
        // an event that never stops by itself: it waits a drawn while (G, from its range), then ends and starts again
        // with new draws; _<n>.x is its clock less G (the device keeps it, so the end reads last frame's)
        const g = range(n, def[kind], kind), x = [`${slot}.x`, [[1, 'a'], ...neg(g.lo), ...neg(g.span).map((t) => [...t, '~1'])], { op: '=', keep: 1 }];
        const ev = { gap: 0, len: 100000 };
        if (def.when !== undefined) ev.gate = [[1, ref(n, def.when, true)]];
        if (kind === 'every') {
          const L = num(n, def.lasts, 'lasts', 0.05, 5, 0.25), D = num(n, def.double, 'double', 0, 1, 0), r = (v) => +v.toFixed(4);
          // after G, it closes over 0.3 L and opens over 0.6 L; now and then (double) once more right after
          const pulse = [[1, ['ss', `@${slot}.x`, 0, r(0.3 * L)], ['ssd', `@${slot}.x`, r(0.4 * L), r(0.6 * L)]]];
          if (D) pulse.push([1, ['ss', `@${slot}.x`, r(1.3 * L), r(0.3 * L)], ['ssd', `@${slot}.x`, r(1.7 * L), r(0.6 * L)], ['lt', '~2', D]]);
          ev.stop = [[1, ['gt', `@${slot}.x`, r(D ? 2.3 * L : L)]]];
          ev.ch = [x, [`${slot}.p`, pulse]];
          main.set(n, { end: r(D ? 2.3 * L : L) });
        } else {
          // then moves to a new place in range (or back home to 0, with the chance home), settling over move seconds
          const R = def.range === undefined ? { lo: [[-1]], span: [[2]] } : range(n, def.range, 'range'), home = num(n, def.home, 'home', 0, 1, 0);
          let to = [...R.lo, ...R.span.map((t) => [...t, '~2'])];
          if (home) to = [...to, ...neg(to).map((t) => [...t, ['lt', '~3', home]])];
          ev.stop = [[1, ['gt', `@${slot}.x`, 0]]];
          if (def.waits !== undefined) {
            // never mid-blink: it moves only when the signal it waits on is at rest, and for an every, not about to start
            const w = def.waits, other = sigs[w]?.every !== undefined && main.get(w);
            if (other) ev.stop = [[1, ['gt', `@${slot}.x`, 0], ['lt', `@_${w}.x`, -0.25]], [1, ['gt', `@${slot}.x`, 0], ['gt', `@_${w}.x`, other.end]]];
            else ev.stop[0].push(['lt', ref(n, w, true), 0.01]);
            if (sigs[w]?.every !== undefined && !other) bad(at(n), `signal ${n}: waits on ${w}: list ${w} before it`);
          }
          ev.ch = [x, [`${slot}.to`, to]];
          opts = spring(4.7 / num(n, def.move, 'move', 0.02, 10, 0.15));
        }
        events[n] = ev;
        terms = [[1, kind === 'every' ? `@${slot}.p` : `@${slot}.to`]];
        break;
      }
    }
    // the options any kind takes: scale, range (clamped to), when (only while that signal is on, eased in and out), ease
    // (a scale that is a signal or a mood table is worked out beside it, then multiplied in)
    const scale = typeof def.scale === 'number' || def.scale === undefined ? num(n, def.scale, 'scale', -1000, 1000, 1) : `@${slot}.scale`;
    if (typeof scale === 'string') extra.push({ who: n, ch: [`${slot}.scale`, val(n, def.scale, 'scale'), { op: '=', keep: 1 }] });
    const state = Object.keys(opts).length > 0;
    const clamp = kind !== 'hold' && def.range !== undefined;
    if (clamp && !(Array.isArray(def.range) && def.range.length === 2 && def.range[0] < def.range[1])) bad(at(n), `signal ${n}: range must be [least, most]`);
    const when = def.when !== undefined && kind !== 'hold' && kind !== 'every' ? ref(n, def.when) : null;
    if (when) extra.push({ who: n, ch: [`${slot}.when`, [[1, when]], { ...spring(8), keep: 1 }] });
    const ease = typeof def.ease === 'number' ? num(n, def.ease, 'ease', 0.01, 10, 0) : 0;
    const post = (ts) => [when && `@${slot}.when`, typeof scale === 'string' && scale].filter(Boolean)
      .reduce((q, f) => q.map((t) => [...t, f]), ts).map(([g, ...f]) => [typeof scale === 'number' ? scaleG(g, scale) : g, ...f]);
    const fin = { op: '=', keep: 1, ...(clamp ? { cl: def.range } : {}) };
    if (ease || (state && (scale !== 1 || when))) {
      extra.push({ who: n, ch: [`${slot}.raw`, terms, { ...opts, op: '=', keep: 1 }] });
      main.set(n, { ...main.get(n), ch: [n, post([[1, `@${slot}.raw`]]), { ...fin, ...(ease ? spring(4.7 / ease) : {}) }] });
    } else main.set(n, { ...main.get(n), ch: [n, post(terms), { ...opts, ...fin }] });
  }

  // in an order where each comes after what it reads now; a loop is refused
  const order = [], seen = new Map();
  const visit = (n, path) => {
    if (seen.get(n) === 2) return;
    if (seen.get(n) === 1) { bad(at(n), `signals read each other in a loop: ${[...path.slice(path.indexOf(n)), n].join(' -> ')}. Break it, or let one follow the other (follow reads the last frame)`); return; }
    seen.set(n, 1);
    for (const d of deps.get(n) ?? []) visit(d, [...path, n]);
    seen.set(n, 2); order.push(n);
  };
  for (const n of names) visit(n, []);
  for (const n of order) {
    for (const e of extra) if (e.who === n) ch.push(e.ch);
    if (main.get(n)?.ch) ch.push(main.get(n).ch);
  }

  // the layers (their weights' channels after every signal)
  const layers = [], clips = new Set(), lays = life.layers ?? [];
  const bone = (L, b) => { const i = nodes.findIndex((x) => x.name === b); if (i < 0) bad(where(lays, L), `layer ${lays[L].layer ?? L}: there is no bone named ${b} (bones: ${list(nodes.map((x) => x.name))})`); return i; };
  const bones = (L, v) => {
    if (v === undefined) return undefined;
    const out = [];
    for (const b of [v].flat()) {
      const chain = typeof b === 'string' && /^(.+)-(.+)$/.exec(b);
      if (chain && bone(L, chain[1]) >= 0 && bone(L, chain[2]) >= 0) {   // a chain: its first bone down to its last
        const top = bone(L, chain[1]), run = [];
        for (let i = bone(L, chain[2]); i >= 0 && i !== top; i = nodes[i].parent) run.unshift(i);
        if (!run.length || nodes[run[0]].parent !== top) bad(where(lays, L), `layer ${lays[L].layer}: ${chain[2]} does not hang below ${chain[1]}`);
        out.push(top, ...run);
      } else if (!chain) { const i = bone(L, b); if (i >= 0) out.push(i); }
    }
    return [...new Set(out)];
  };
  lays.forEach((l, i) => {
    const name = l?.layer ?? `${i}`, here = where(lays, i) || where(life, 'layers');
    const sig = (s, what) => {
      if (typeof s !== 'string' || !known(s)) { bad(here, `layer ${name}: ${what} ${JSON.stringify(s)} is not a signal (its signals: ${list(names)})`); return 't'; }
      if (s === 'working') eased = true;
      return s in DEVICE ? DEVICE[s] : s;
    };
    if (!l || typeof l !== 'object' || typeof l.layer !== 'string') return bad(here, `life.layers[${i}]: a layer is { "layer": its name, "clip" or "turn", ... }`);
    for (const [k, what] of [['once', 'a picked clip played once (once)'], ['loop', 'a clip on its own clock (loop)']]) if (k in l) return bad(here, `layer ${name}: ${what} is not in Molty's firmware yet`);
    if (l.mode !== undefined && l.mode !== 'add') return bad(here, `layer ${name}: mode ${l.mode} is not in Molty's firmware yet: layers add`);
    const odd = Object.keys(l).filter((k) => !['layer', 'clip', 'by', 'zero', 'weight', 'bones', 'mode', 'turn'].includes(k));
    if (odd.length) bad(here, `layer ${name}: it does not take ${list(odd)}`);
    let w;
    if (l.weight !== undefined && l.weight !== 1) {   // eased, so a layer fades in and out
      w = `_lw.${name}`.slice(0, 32);
      const t = typeof l.weight === 'string' ? [[1, `@${sig(l.weight, 'its weight')}`]] : val(name, l.weight, `layer ${name}'s weight`);
      ch.push([w, t, { ...spring(WEIGHT_EASE), keep: 1 }]);
    }
    if (l.clip !== undefined) {
      if (l.turn !== undefined) return bad(here, `layer ${name}: a layer plays a clip or turns bones, not both`);
      if (!actions.includes(l.clip)) return bad(here, `layer ${name}: there is no action named ${l.clip} (actions: ${list(actions)})`);
      if (l.zero !== undefined && !(typeof l.zero === 'number' && l.zero >= 0 && l.zero <= 1)) bad(here, `layer ${name}: zero must be from 0 to 1`);
      clips.add(l.clip);
      const nodesOf = bones(i, l.bones);
      layers.push({ by: sig(l.by, 'by'), ...(w ? { w } : {}), clip: l.clip, ...(l.zero ? { zero: l.zero } : {}), ...(nodesOf ? { nodes: nodesOf } : {}) });
    } else if (l.turn && typeof l.turn === 'object') {
      const t = l.turn, ns = bones(i, t.bones);
      if (!ns?.length) return bad(here, `layer ${name}: a turn names its bones`);
      if (t.ripple !== undefined) bad(here, `layer ${name}: ripple is not in Molty's firmware yet`);
      const by = new Map();   // one layer for each signal it is turned by
      for (const [k, axis] of [['x', 0], ['y', 1], ['z', 2], ['squash', 3]]) {
        if (t[k] === undefined) continue;
        if (!Array.isArray(t[k]) || t[k].length !== 2 || typeof t[k][1] !== 'number') { bad(here, `layer ${name}: ${k} must be [signal, ${axis < 3 ? 'degrees' : 'amount'}]`); continue; }
        const s = sig(t[k][0], k), e = by.get(s) ?? { by: s, ...(w ? { w } : {}), nodes: ns };
        if (axis < 3) (e.rot ??= [0, 0, 0])[axis] = t[k][1]; else e.sq = t[k][1];
        by.set(s, e);
      }
      for (const k of Object.keys(t)) if (!['bones', 'x', 'y', 'z', 'squash', 'ripple'].includes(k)) bad(here, `layer ${name}: a turn does not take ${k} (it takes bones, x, y, z, squash)`);
      if (!by.size) bad(here, `layer ${name}: a turn says what turns it: x, y or z [signal, degrees], or squash [signal, amount]`);
      layers.push(...by.values());
    } else bad(here, `layer ${name}: say what it moves: "clip" (an action, with "by": a signal) or "turn" ({ bones, x, y, z, squash })`);
  });
  if (eased) ch.unshift(['_working', [[1, '@work']], { lp: 4, keep: 1 }]);   // working, eased as a mood is
  if (layers.length > LIFE_LIMITS.layers) bad(where(life, 'layers'), `life.layers makes ${layers.length} layers: a Molty holds ${LIFE_LIMITS.layers}`);

  // what a frame of it can cost the device: every factor of every channel, every event running
  const cost = (ts) => ts.reduce((s, t) => s + t.length - 1, 0), chCost = (c) => cost(c[1]);
  const steps = ch.reduce((s, c) => s + chCost(c), 0) + Object.values(events).reduce((s, e) => s + e.ch.reduce((q, c) => q + chCost(c), 0) + cost(e.stop) + (e.gate ? cost(e.gate) : 0), 0);
  if (steps > LIFE_LIMITS.steps) bad(where(life, 'signals'), `life takes ${steps} steps a frame: the device gives it ${LIFE_LIMITS.steps}. Drop some signals`);
  return { life: { ch, ...(Object.keys(events).length ? { events } : {}), ...(layers.length ? { layers } : {}) }, steps, clips };
}

/** A term's weight times k: a number, or each value of a mood table. */
function scaleG(g, k) { return typeof g === 'number' ? g * k : Object.fromEntries(Object.entries(g).map(([m, v]) => [m, v * k])); }
function neg(ts) { return ts.map(([g, ...f]) => [scaleG(g, -1), ...f]); }
