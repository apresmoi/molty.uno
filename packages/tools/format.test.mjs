// SPDX-License-Identifier: MIT
// node --test packages/tools/: the package checks catch what docs/PACKAGES.md forbids, and the
// shipped astrology package plays its layouts with the params the molty-astro skill sends.
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { test } from 'node:test';
import { ParamError, bind, checkPackage, opens } from './format.mjs';
import { frame, hd, lit, loadPackage, png, spotAt } from './play.mjs';

const ROOT = new URL('../../', import.meta.url).pathname;
const base = () => ({
  v: 'molty-pkg/1', id: 'demo', name: 'Demo', version: '1.0.0', kind: 'scenes',
  assets: { dot: { kind: 'mask', w: 2, h: 2, bits: Buffer.from([0xc0]).toString('base64') }, face: { kind: 'image', w: 2, h: 1, px: Buffer.from([0x01]).toString('base64'), palette: ['#000000', '#ff0000'] } },
  layouts: { one: { dur: 2, params: { r: 'number', marks: 'list', g: 'text' }, items: [{ k: 'ring', r: '$r' }, { k: 'line', pts: [[0, 0], ['$r', 1]] }, { k: 'glyph', asset: '$g', size: 0.2, polar: ['@a', 0.5], each: '$marks' }] } },
});
const problems = (mut) => { const p = base(); mut(p); return checkPackage(p, 'demo'); };

test('a valid package passes', () => assert.deepEqual(checkPackage(base(), 'demo'), []));

test('the static check catches every kind of broken reference', () => {
  const cases = {
    'unknown field': (p) => (p.layouts.one.items[0].dash = 0.1),
    'unknown k': (p) => (p.layouts.one.items[0].k = 'poly'),
    'not a param': (p) => (p.layouts.one.items[0].r = '$radius'),
    'needs a number param': (p) => (p.layouts.one.items[0].r = '$g'),
    'outside an each': (p) => (p.layouts.one.items[0].r = '@r'),
    'names no asset': (p) => (p.layouts.one.items.push({ k: 'glyph', asset: 'star', size: 1 })),
    'names no image asset': (p) => (p.layouts.one.items.push({ k: 'ball', img: 'dot', r: 1 })),
    'is never read': (p) => (p.layouts.one.params.spare = 'number'),
    'ring needs r': (p) => delete p.layouts.one.items[0].r,
    'bits must be base64': (p) => (p.assets.dot.w = 40),
    'indexes past the palette': (p) => (p.assets.face.px = Buffer.from([0x21]).toString('base64')),
    'must be #rrggbb': (p) => (p.layouts.one.items[0].color = 'red'),
    'polar replaces x, y': (p) => (p.layouts.one.items[2].x = 0),
    'is out of range': (p) => (p.layouts.one.items[2].size = 3),
    'pts must be 2..16': (p) => (p.layouts.one.items[1].pts = [[0, 0]]),
    'at most 8 params': (p) => Object.assign(p.layouts.one.params, Object.fromEntries('abcdef'.split('').map((k) => [k, 'number']))),
    'name must be [A-Za-z0-9_-]{1,24}': (p) => (p.assets['a.b'] = p.assets.dot),
    'id must be demo': (p) => (p.id = 'other'),
    'when must be "lit"': (p) => (p.layouts.one.items[0].when = 'later'),
    'group must be a name': (p) => (p.layouts.one.items[0].group = 'a b'),
    'unknown field group for spot': (p) => p.layouts.one.items.push({ k: 'spot', r: 0.1, say: 'x', group: 'a' }),
    'lights must be 1..4 names': (p) => p.layouts.one.items.push({ k: 'spot', r: 0.1, say: 'x', lights: 'a b c d e' }),
    'pts must be 2..2': (p) => p.layouts.one.items.push({ k: 'spot', r: 0.1, say: 'x', pts: [[0, 0], [1, 0], [1, 1]] }),
  };
  for (const [want, mut] of Object.entries(cases)) assert.ok(problems(mut).some((m) => m.includes(want)), `${want}: ${problems(mut)}`);
});

test('a creature describes how it looks and every gesture and mood it plays', () => {
  const creature = () => ({ v: 'molty-pkg/1', id: 'demo', name: 'Demo', version: '1.0.0', kind: 'creature', creature: { gestures: { nod: {} }, moods: { happy: {} },
    about: { look: 'A small round demo.', gestures: { nod: 'dips its head' }, moods: { idle: 'sits still', happy: 'beams' } } } });
  const about = (mut) => { const p = creature(); mut(p.creature); return checkPackage(p, 'demo'); };
  assert.deepEqual(about(() => {}), []);
  assert.deepEqual(about((c) => { c.gestures.eat = { ch: [], dur: 4 }; c.about.gestures.eat = 'eats from a bowl'; }), []);
  const cases = {
    'needs about': (c) => delete c.about,
    'about.look must be': (c) => (c.about.look = 'x'.repeat(241)),
    'has no line for nod': (c) => delete c.about.gestures.nod,
    'has no line for idle': (c) => delete c.about.moods.idle,
    'does not play wave': (c) => (c.about.gestures.wave = 'waves'),
    'about.moods.happy must be': (c) => (c.about.moods.happy = 'beams\u2014wide'),
    'unknown field': (c) => (c.about.voice = 'squeaky'),
    'its own gesture needs dur': (c) => { c.gestures.eat = { ch: [] }; c.about.gestures.eat = 'eats'; },
    'keeps its own length': (c) => (c.gestures.nod.dur = 2),
    'name must be': (c) => { c.gestures.Eat = { ch: [], dur: 2 }; c.about.gestures.Eat = 'eats'; },
  };
  for (const [want, mut] of Object.entries(cases)) assert.ok(about(mut).some((m) => m.includes(want)), `${want}: ${about(mut)}`);
});

test('binding rejects missing, unknown and mistyped params, and resolves each, polar and defaults', () => {
  const p = base(), ok = { r: 0.5, marks: [{ a: 90 }], g: 'dot' };
  for (const bad of [{ r: 0.5, marks: [] }, { ...ok, x: 1 }, { ...ok, r: '0.5' }, { ...ok, g: 'star' }, { ...ok, g: '$x' }, { ...ok, marks: [{ b: 1 }] }, { ...ok, marks: [3] }, { ...ok, marks: [{ a: [1, 2] }] }, { ...ok, marks: Array(33).fill({ a: 1 }) }, { ...ok, r: 2.5 }]) assert.throws(() => bind(p, 'one', bad), ParamError, JSON.stringify(bad));
  assert.throws(() => bind(p, 'two', ok), /no_layout/);
  const items = bind(p, 'one', { ...ok, marks: [{ a: 90 }, { a: 180 }] });
  assert.equal(items.length, 4);
  assert.deepEqual([items[2].x.toFixed(3), items[2].y.toFixed(3), items[3].x.toFixed(3)], ['0.000', '0.500', '-0.500']);
  assert.deepEqual([items[0].color, items[0].on, items[0].off], ['#ffffff', 0, 2]);
});

test('an aspect\'s arc runs the short way, from one body to the other, for every kind, order and wrap', async () => {
  // the device draws an arc counter-clockwise from a0 to a1 (scene_player.c ring()): its span must be the bodies' separation
  const { aspect } = await import(`${ROOT}skills/molty-astro/scripts/sky.mjs`), pkg = loadPackage('astrology'), seen = new Set();
  const same = (x, y) => Math.abs((((x - y) % 360) + 540) % 360 - 180) < 0.02, mod = (v) => ((v % 360) + 360) % 360;
  for (let i = 0; i < 50; i++) for (const kind of ['conjunction', 'sextile', 'square', 'trine', 'opposition']) {
    const r = aspect(kind, new Date(Date.UTC(2026, 0, 1) + i * 29 * 864e5)), p = r.scenes[0][1], items = bind(pkg, 'aspect', p);
    const arc = items.find((it) => it.k === 'arc'), spokes = items.filter((it) => it.k === 'ticks' && it.n === 1), span = mod(arc.a1 - arc.a0), at = `${kind} ${r.facts.a}-${r.facts.b} (${p.a}, ${p.b})`;
    assert.ok(span <= 180.01 && Math.abs(span - r.facts.sep) < 0.05, `${at}: the arc sweeps ${span.toFixed(2)}, the bodies are ${r.facts.sep} apart`);
    assert.ok(same(arc.a0, p.bodies[0].a) && same(arc.a1, p.bodies[1].a), `${at}: the arc runs from one body to the other`);
    assert.deepEqual(spokes.map((s) => mod(s.a0).toFixed(2)), [arc.a0, arc.a1].map((a) => mod(a).toFixed(2)), `${at}: a spoke to each end`);
    seen.add(p.bodies[0].g === `g-${r.facts.a.toLowerCase()}` ? 'in order' : 'swapped').add(arc.a1 >= 360 ? 'across 0' : 'within');
  }
  assert.deepEqual([...seen].sort(), ['across 0', 'in order', 'swapped', 'within'], 'both orders of the bodies and an arc across 0 were tried');
});

test('every layout plays with the params the molty-astro skill sends, each under 1 KB (with spot readings: 8 KB)', () => {
  const pkg = loadPackage('astrology'), seen = new Set(), READS = ['chart', 'sign-closeup'];
  // a tap on a crowded sign (Leo: Moon, Mars, Jupiter) and on an empty one (Taurus), in the chart's interactive mode
  for (const scene of [['tonight'], ['chart'], ['signs'], ['aspects'], ['events'], ['chart', '--interactive', '--tap', 'leo'], ['chart', '--interactive', '--tap', 'taurus']]) {
    const out = execFileSync(process.execPath, [`${ROOT}skills/molty-astro/scripts/astro.mjs`, ...scene, '--date', '2026-11-01', '--dry-run'], { encoding: 'utf8', env: { ...process.env, MOLTY_PKG_INDEX: `${ROOT}packages/index.json` } });
    for (const c of out.split('\n').filter((l) => l.startsWith('{')).map((l) => JSON.parse(l)).filter((c) => c.name.startsWith('scene.'))) {
      assert.ok(JSON.stringify(c).length < (READS.includes(c.args.layout) ? 8192 : 1024), `${c.args.layout}: ${JSON.stringify(c).length} B`);
      const t = pkg.layouts[c.args.layout].dur / 2 || 1, dots = frame(pkg, c.args.layout, c.args.params, t);
      assert.ok(dots.size > 200, `${c.args.layout} draws ${dots.size} dots`);
      assert.equal(png(dots).subarray(1, 4).toString(), 'PNG');
      seen.add(c.args.layout);
      if (c.args.layout !== 'chart') continue;
      const { aspects } = c.args.params, items = bind(pkg, 'chart', c.args.params);
      assert.ok(aspects.length >= 1 && aspects.length <= 3, 'the chart draws at most the three tightest aspects');
      // each a line between its two bodies' symbols: from LINE_GAP (0.085) outside one centre to the same outside the other
      for (const a of aspects) for (const [x, y, px, py] of [[a.x1, a.y1, a.px1, a.py1], [a.x2, a.y2, a.px2, a.py2]]) assert.ok(Math.abs(Math.hypot(x - px, y - py) - 0.085) < 0.002, `${a.id}: its end 0.085 from its body`);
      // the middle is empty (no ring, no spot there): the reading's words show there
      assert.ok(!items.some((it) => (it.k === 'ring' && !it.group && it.r < 0.7) || (it.k === 'spot' && Math.hypot(it.x, it.y) < 0.3 && !it.pts)), 'nothing in the middle');
      assert.equal(pkg.layouts.chart.caption_y, 0);
      assert.ok(items.filter((it) => it.k === 'spot' && it.grow === 1.5).length === 12, 'a tapped sign grows 1.5x');
    }
  }
  assert.deepEqual([...seen].sort(), Object.keys(pkg.layouts).sort());
});

test('the calm view in high definition: a ring is round and soft edged at the panel\'s pixels, a ball stays on the dots', () => {
  const p = base();
  p.layouts.one.items.push({ k: 'ball', r: 0.1, lit: 1 });
  const S = 466, { rgb, kept } = hd(p, 'one', { r: 0.8, g: 'dot', marks: [] }, 1, S), at = (x, y) => rgb[(Math.round(y) * S + Math.round(x)) * 3];
  let round = 0, soft = 0;
  for (let a = 0; a < 360; a++) {
    const c = Math.cos((a * Math.PI) / 180), s = Math.sin((a * Math.PI) / 180), R = 0.8 * (S / 2);
    round += Math.max(at(S / 2 + R * c, S / 2 + R * s), at(S / 2 + (R + 0.5) * c, S / 2 + (R + 0.5) * s)) > 200;
    const e = at(S / 2 + (R + 4) * c, S / 2 + (R + 4) * s);                // the edge of its 7 px (w 0.03)
    soft += e > 5 && e < 250;
  }
  assert.ok(round === 360 && soft > 30, `the ring at every degree (${round}), soft edged (${soft})`);
  assert.ok(kept.size > 10, `the ball keeps its dots (${kept.size})`);
  assert.equal(png(kept, rgb, S).subarray(1, 4).toString(), 'PNG');
});

test('a spot lights groups: their items grow and brighten, those shown when lit appear, then all as built', () => {
  const p = base();
  p.layouts.one.items.push({ k: 'disc', x: 0.5, y: 0, r: 0.1, color: '#808080', group: 'p' }, { k: 'ring', x: 0.5, y: 0, r: 0.15, w: 0.01, group: 'p', when: 'lit' },
    { k: 'disc', x: -0.5, y: 0, r: 0.1, group: 'q' }, { k: 'spot', pts: [[0, 0], [0.5, 0]], r: 0.05, say: 'the p', id: 'tap-p', lights: 'p' });
  assert.deepEqual(checkPackage(p, 'demo'), []);
  const items = bind(p, 'one', { r: 0.5, g: 'dot', marks: [] }), held = lit(items, 'tap-p'), calm = lit(items, '');
  assert.ok(!calm.some((it) => it.when === 'lit'), 'calm: the lit-only ring hidden');
  const disc = held.find((it) => it.group === 'p' && it.k === 'disc'), other = held.find((it) => it.group === 'q');
  assert.ok(Math.abs(disc.r - 0.125) < 1e-9 && disc.color === '#c0c0c0' && other.r === 0.1, `tapped: ${JSON.stringify(disc)}`);
  assert.ok(held.some((it) => it.k === 'ring' && it.when === 'lit' && Math.abs(it.r - 0.1875) < 1e-9), 'tapped: its ring shows, grown');
  assert.equal(png(hd(p, 'one', { r: 0.5, g: 'dot', marks: [] }, 1, 200, '', 'tap-p').kept, null).subarray(1, 4).toString(), 'PNG');
});

test('a spot may say how much it grows when tapped; a layout where its caption goes', () => {
  const p = base();
  p.layouts.one.items.push({ k: 'disc', x: 0.5, y: 0, r: 0.1 }, { k: 'spot', x: 0.5, y: 0, r: 0.12, say: 'big', id: 'b', grow: 1.5 });
  p.layouts.one.caption_y = 0;
  assert.deepEqual(checkPackage(p, 'demo'), []);
  const items = bind(p, 'one', { r: 0.5, g: 'dot', marks: [] });
  assert.ok(Math.abs(lit(items, 'b').find((it) => it.k === 'disc').r - 0.15) < 1e-9, 'grown 1.5x');
  // the caption at the layout's caption_y (the middle) instead of the bottom, the same size
  const draw = (cap) => hd(p, 'one', { r: 0.5, g: 'dot', marks: [] }, 1, 200, cap).rgb, none = draw('');
  const word = (rgb, y) => { let n = 0; for (let x = 0; x < 200; x++) n += rgb[(y * 200 + x) * 3] !== none[(y * 200 + x) * 3]; return n; };
  const mid = draw('word');
  delete p.layouts.one.caption_y;
  const low = draw('word');
  assert.ok(word(mid, 100) > 0 && word(mid, 156) === 0 && word(low, 156) > 0 && word(low, 100) === 0, 'the word in the middle, else at the bottom');
  for (const [k, v] of [['grow', 0.5], ['grow', 4]]) { const q = base(); q.layouts.one.items.push({ k: 'spot', r: 0.1, say: 'x', [k]: v }); assert.ok(checkPackage(q, 'demo').some((e) => e.includes('grow')), `grow ${v} refused`); }
  const q = base(); q.layouts.one.caption_y = 2;
  assert.ok(checkPackage(q, 'demo').some((e) => e.includes('caption_y')));
});

test('a spot may also be tapped over a ring sector; its own disc or segment still wins', () => {
  const p = base();
  p.layouts.one.items.push({ k: 'spot', polar: [90, 0.8], r: 0.05, say: 'north', id: 'n', arc: 30, r0: 0.7, r1: 0.9 },
    { k: 'spot', polar: [120, 0.8], r: 0.05, say: 'next', id: 'x', arc: 30, r0: 0.7, r1: 0.9 }, { k: 'spot', polar: [100, 0.75], r: 0.06, say: 'inside', id: 'p' });
  assert.deepEqual(checkPackage(p, 'demo'), []);
  const items = bind(p, 'one', { r: 0.5, g: 'dot', marks: [] }), at = (deg, r) => spotAt(items, r * Math.cos((deg * Math.PI) / 180), r * Math.sin((deg * Math.PI) / 180))?.id;
  assert.deepEqual([at(90, 0.8), at(76, 0.71), at(104, 0.89), at(76, 0.89), at(104.5, 0.71)], ['n', 'n', 'n', 'n', 'n'], 'its centre and corners');
  assert.deepEqual([at(90, 0.69), at(90, 0.91), at(74, 0.8)], [undefined, undefined, undefined], 'just outside it');
  assert.deepEqual([at(106, 0.8), at(100, 0.75), at(98, 0.72)], ['x', 'p', 'p'], 'the next wedge; a disc inside a wedge keeps the tap');
  for (const bad of [{ arc: 30 }, { r1: 0.9 }, { r0: 0.1 }, { arc: 0, r1: 0.9 }, { arc: 30, r1: 3 }]) { const q = base(); q.layouts.one.items.push({ k: 'spot', r: 0.1, say: 'x', ...bad }); assert.ok(checkPackage(q, 'demo').length > 0, `${JSON.stringify(bad)} refused`); }
  // the chart: every sign's whole 30 degree block of the zodiac ring
  const chart = bind(loadPackage('astrology'), 'chart', JSON.parse(execFileSync(process.execPath, [`${ROOT}skills/molty-astro/scripts/astro.mjs`, 'chart', '--date', '2026-11-01', '--dry-run'], { encoding: 'utf8', env: { ...process.env, MOLTY_PKG_INDEX: `${ROOT}packages/index.json` } }).split('\n').find((l) => l.includes('"layout":"chart"'))).args.params);
  const sign = (deg, r) => spotAt(chart, r * Math.cos((deg * Math.PI) / 180), r * Math.sin((deg * Math.PI) / 180))?.id;
  assert.deepEqual([sign(345, 0.825), sign(331, 0.76), sign(359, 0.89), sign(1, 0.8), sign(345, 0.95)], ['virgo', 'virgo', 'virgo', 'libra', undefined]);
});

test('a spot may open another layout of the package, fed from the tapped element and the layout\'s own params', () => {
  const p = base();
  p.layouts.one.params.extra = 'list';
  p.layouts.one.items.push({ k: 'spot', each: '$marks', polar: ['@a', 0.5], r: 0.1, say: '@say', id: '@id', open: 'two', ttl: '@ttl' });
  p.layouts.two = { dur: 0, params: { g: 'text', title: 'text', extra: 'list' }, items: [{ k: 'glyph', asset: '$g', size: 0.5 }, { k: 'text', str: '$title' }, { k: 'disc', each: '$extra', x: '@x', r: 0.05 }] };
  assert.deepEqual(checkPackage(p, 'demo'), [], 'extra is read: the layout a spot opens takes it');
  const params = { r: 0.5, g: 'dot', marks: [{ a: 0, say: 'east', id: 'e', title: 'EAST', ttl: 12 }, { a: 90, say: 'north', id: 'n', title: 'NORTH', ttl: 9 }], extra: [{ of: 'e', x: 0.1 }, { of: 'n', x: 0.2 }, { x: 0.3 }] };
  const o = opens(p, 'one', params, 'n');
  assert.deepEqual(o, { layout: 'two', params: { g: 'dot', title: 'NORTH', extra: [{ of: 'n', x: 0.2 }, { x: 0.3 }] }, ttl: 9 }, 'its element first, then the layout\'s params; a list keeps the elements of this spot (or of none)');
  assert.equal(bind(p, o.layout, o.params).filter((it) => it.k === 'disc').length, 2);
  assert.equal(opens(p, 'one', params, 'nowhere'), null);
  const q = base(); q.layouts.one.items.push({ k: 'spot', r: 0.1, say: 'x', open: 'nothing' });
  assert.ok(checkPackage(q, 'demo').some((e) => e.includes('names no layout')));
  const t = base(); t.layouts.one.items.push({ k: 'spot', r: 0.1, say: 'x', ttl: 5 });
  assert.ok(checkPackage(t, 'demo').some((e) => e.includes('ttl is for open')));
  // the chart: a sign's wedge opens that sign up close, with the params the skill's own close-up has
  const run = (...a) => execFileSync(process.execPath, [`${ROOT}skills/molty-astro/scripts/astro.mjs`, ...a, '--date', '2026-11-01', '--dry-run'], { encoding: 'utf8', env: { ...process.env, MOLTY_PKG_INDEX: `${ROOT}packages/index.json` } }).split('\n').filter((l) => l.startsWith('{')).map((l) => JSON.parse(l));
  const pkg = loadPackage('astrology'), chart = run('chart').find((c) => c.args.layout === 'chart').args, own = run('chart', '--interactive', '--tap', 'leo').find((c) => c.args?.layout === 'sign-closeup').args;
  const up = opens(pkg, 'chart', chart.params, 'leo');
  assert.equal(up.layout, 'sign-closeup');
  assert.equal(up.ttl, own.ttl_s);
  const drawn = (params) => JSON.stringify(bind(pkg, 'sign-closeup', params).map(({ k, x, y, r, size, asset, color, on, off, say, id, str, a0 }) => ({ k, x, y, r, size, asset, color, on, off, say, id, str, a0 })));
  assert.equal(drawn(up.params), drawn(own.params), 'the same close-up the skill would send');
});

test('a spot: a place to tap with its reading, which may run past a text item', () => {
  const p = base();
  p.layouts.one.items.push({ k: 'spot', each: '$marks', polar: ['@a', 0.5], r: 0.1, say: '@say', id: '@id' });
  assert.deepEqual(checkPackage(p, 'demo'), []);
  const say = 'a'.repeat(160);
  const spot = bind(p, 'one', { r: 0.5, g: 'dot', marks: [{ a: 0, say, id: 'east' }] }).find((it) => it.k === 'spot');
  assert.equal(spot.say, say);
  assert.equal(spot.id, 'east');
  assert.ok(Math.abs(spot.x - 0.5) < 1e-9);
  assert.throws(() => bind(p, 'one', { r: 0.5, g: 'dot', marks: [{ a: 0, say: say + 'a', id: 'east' }] }), ParamError);
  p.layouts.one.items.push({ k: 'text', str: '$g' });
  assert.throws(() => bind(p, 'one', { r: 0.5, g: say.slice(0, 41), marks: [] }), ParamError, 'a text item still takes 40');
  const q = base();
  q.layouts.one.items.push({ k: 'spot', r: 0.1 });
  assert.ok(checkPackage(q, 'demo').some((e) => e.includes('spot needs say')));
});
