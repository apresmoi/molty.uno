// SPDX-License-Identifier: MIT
// node --test packages/tools/: the package checks catch what docs/PACKAGES.md forbids, and the
// shipped astrology package plays its layouts with the params the molty-astro skill sends.
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { test } from 'node:test';
import { ParamError, bind, checkPackage } from './format.mjs';
import { frame, loadPackage, png } from './play.mjs';

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

test('binding rejects missing, unknown and mistyped params, and resolves each, polar and defaults', () => {
  const p = base(), ok = { r: 0.5, marks: [{ a: 90 }], g: 'dot' };
  for (const bad of [{ r: 0.5, marks: [] }, { ...ok, x: 1 }, { ...ok, r: '0.5' }, { ...ok, g: 'star' }, { ...ok, g: '$x' }, { ...ok, marks: [{ b: 1 }] }, { ...ok, marks: [3] }, { ...ok, marks: [{ a: [1, 2] }] }, { ...ok, marks: Array(33).fill({ a: 1 }) }, { ...ok, r: 2.5 }]) assert.throws(() => bind(p, 'one', bad), ParamError, JSON.stringify(bad));
  assert.throws(() => bind(p, 'two', ok), /no_layout/);
  const items = bind(p, 'one', { ...ok, marks: [{ a: 90 }, { a: 180 }] });
  assert.equal(items.length, 4);
  assert.deepEqual([items[2].x.toFixed(3), items[2].y.toFixed(3), items[3].x.toFixed(3)], ['0.000', '0.500', '-0.500']);
  assert.deepEqual([items[0].color, items[0].on, items[0].off], ['#ffffff', 0, 2]);
});

test('every layout plays with the params the molty-astro skill sends, each under 1 KB', () => {
  const pkg = loadPackage('astrology'), seen = new Set();
  for (const scene of ['tonight', 'chart', 'signs', 'aspects', 'events']) {
    const out = execFileSync(process.execPath, [`${ROOT}skills/molty-astro/scripts/astro.mjs`, scene, '--date', '2026-11-01', '--dry-run'], { encoding: 'utf8', env: { ...process.env, MOLTY_PKG_INDEX: `${ROOT}packages/index.json` } });
    for (const c of out.split('\n').filter((l) => l.startsWith('{')).map((l) => JSON.parse(l)).filter((c) => c.name.startsWith('scene.'))) {
      assert.ok(JSON.stringify(c).length < 1024, `${c.args.layout}: ${JSON.stringify(c).length} B`);
      const t = pkg.layouts[c.args.layout].dur / 2 || 1, dots = frame(pkg, c.args.layout, c.args.params, t);
      assert.ok(dots.size > 200, `${c.args.layout} draws ${dots.size} dots`);
      assert.equal(png(dots).subarray(1, 4).toString(), 'PNG');
      seen.add(c.args.layout);
    }
  }
  assert.deepEqual([...seen].sort(), Object.keys(pkg.layouts).sort());
});
