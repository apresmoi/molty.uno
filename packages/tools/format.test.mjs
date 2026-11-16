// SPDX-License-Identifier: MIT
// node --test packages/tools/: the package checks catch what docs/PACKAGES.md forbids, and the
// shipped astrology package plays its layouts with the params the molty-astro skill sends.
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { test } from 'node:test';
import { ParamError, bind, checkPackage } from './format.mjs';
import { frame, hd, loadPackage, png } from './play.mjs';

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
      const { tense, easy, joined, read } = c.args.params;
      assert.ok(tense.length + easy.length + joined.length <= 3, 'the chart draws at most the three tightest aspects');
      assert.ok(pkg.layouts.chart.items.some((it) => it.k === 'spot' && it.id === 'aspects' && it.say === '$read') && read.length > 0 && read.length <= 160, 'the middle has its reading');
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
