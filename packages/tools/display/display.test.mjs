// SPDX-License-Identifier: MIT
// node --test packages/tools/display/: display v2 (docs/DISPLAY.md) breaks nothing and checks what it says.
//   golden      the astrology skill folder builds packages/astrology/astrology.json, byte for byte
//   round trip  build(convert(v1)) gives every scenes package back
//   checks      each rule of "what each check catches" fails a skill that breaks it, naming file and line
//   limits      64 items a screen, 8 inputs, 32 screens, 128 items at the fullest, 256 KB
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { cpSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { after, test } from 'node:test';
import { fileURLToPath } from 'node:url';
import { deflateSync } from 'node:zlib';
import { LIMITS, MAX_PKG_BYTES, bind, checkPackage, opens } from '../format.mjs';
import { chunk, frame, spotAt } from '../play.mjs';
import { assetPng, buildAssets, readPng } from './assets.mjs';
import { SCHEMA, build, check, exampleParams, inputSchema } from './build.mjs';
import { COMMON, SHAPED, SHAPES } from './compile.mjs';
import { convert } from './convert.mjs';
import { preview } from './preview.mjs';
import { ANNOTATIONS, KEYWORDS, validate } from './schema.mjs';

const ROOT = fileURLToPath(new URL('../../../', import.meta.url)), ASTRO = join(ROOT, 'skills/molty-astro'), DIAL = join(ROOT, 'docs/examples/molty-dial');
const GOLDEN = join(ROOT, 'packages/astrology/astrology.json'), GOLDEN_SHA = 'cb320aaee9e993ccb861eb8ec8abcedebd112832028abdaf227508b9d5f36dd7';
const TMP = mkdtempSync(join(tmpdir(), 'molty-display-'));
after(() => rmSync(TMP, { recursive: true, force: true }));
let made = 0;
const fresh = () => join(TMP, String(made++));
/** A folder holding these files ({ path: text or bytes }). */
function folder(files) {
  const dir = fresh();
  for (const [f, body] of Object.entries(files)) { mkdirSync(dirname(join(dir, f)), { recursive: true }); writeFileSync(join(dir, f), body); }
  return dir;
}
const tree = (dir) => readdirSync(dir, { recursive: true, withFileTypes: true }).filter((e) => e.isFile()).map((e) => join(e.parentPath, e.name).slice(dir.length + 1)).sort();

// ---- a small valid skill, and ways to break it
const DOT = assetPng({ kind: 'mask', w: 2, h: 2, bits: Buffer.from([0xc0]).toString('base64') });
const picture = (side, seed = 1) => assetPng({ kind: 'image', w: side, h: side, px: Buffer.from(Array.from({ length: Math.ceil((side * side) / 2) }, (_, i) => ((i * seed * 7 + seed) % 255) & 0xff)).toString('base64'), palette: Array.from({ length: 16 }, (_, i) => `#${(i * 17).toString(16).padStart(2, '0').repeat(3)}`) });
const base = () => ({
  molty: 'display/2',
  package: { id: 'demo', name: 'Demo', version: '1.0.0', about: 'A demo.' },
  assets: [{ file: 'assets/dot.png', as: 'symbol' }, { file: 'assets/face.png', as: 'picture' }],
  screens: { one: {
    about: 'A ring, a mark for each element, a centre to tap.', duration: 2,
    inputs: {
      size: { type: 'distance', about: 'The ring.', example: 0.5 },
      marks: { type: 'list', max: 4, about: 'The marks.', of: { angle: { type: 'angle', about: 'Where.' }, name: { type: 'text', about: 'Its group.' } }, example: [{ angle: 90, name: 'a' }] },
      note: { type: 'reading', about: 'What a tap shows.', example: 'Hello.' },
    },
    draw: [
      { circle: { radius: '{{ size }}' } },
      { for: 'mark in marks', draw: [{ symbol: { asset: 'dot', size: 0.2, at: { angle: '{{ mark.angle }}', distance: 0.5 }, group: '{{ mark.name }}' } }] },
      { tap_zone: { radius: 0.3, reads: '{{ note }}', id: 'middle' } },
    ],
  } },
});
/** check() of the base skill after `mut(doc, files)` changed it. */
function checked(mut = () => {}) {
  const doc = base(), files = { 'SKILL.md': '# Demo\n\nShow [one](display.json#one).\n', 'assets/dot.png': DOT, 'assets/face.png': picture(4) };
  mut(doc, files);
  return check(folder({ ...files, 'display.json': JSON.stringify(doc, null, 1) }));
}
const one = (doc) => doc.screens.one;
/** Each case breaks the base skill; check must then say `want`, at a file and line. */
function catches(cases, list = 'errors') {
  for (const [want, mut] of Object.entries(cases)) {
    const found = checked(mut)[list];
    assert.ok(found.some((e) => e.message.includes(want) && /^[\w./-]+:\d+$/.test(e.at)), `${want}: got ${JSON.stringify(found)}`);
  }
}

test('golden: the astrology skill folder builds today\'s package, byte for byte, and its committed docs', () => {
  const dir = fresh(), out = join(TMP, 'astrology.json');
  cpSync(ASTRO, dir, { recursive: true });
  rmSync(join(dir, 'build'), { recursive: true });
  const r = build(dir, out);
  assert.deepEqual(r.errors, []);
  assert.deepEqual(r.warnings, [], 'no screen left out of SKILL.md, no about left TODO');
  const bytes = readFileSync(out);
  assert.equal(createHash('sha256').update(bytes).digest('hex'), GOLDEN_SHA);
  assert.ok(bytes.equals(readFileSync(GOLDEN)), 'the built package is packages/astrology/astrology.json');
  assert.equal(bytes.length, 74507);
  assert.deepEqual(tree(join(dir, 'build')), tree(join(ASTRO, 'build')));
  for (const f of tree(join(dir, 'build'))) assert.equal(readFileSync(join(dir, 'build', f), 'utf8'), readFileSync(join(ASTRO, 'build', f), 'utf8'), `skills/molty-astro/build/${f} is stale: run molty-display build`);
  assert.equal(r.skill.doc.package.about, readFileSync(join(ROOT, 'packages/astrology/description.txt'), 'utf8').trim(), 'the catalog line is package.about');
});

test('round trip: build(convert(v1)) is the v1 file for every scenes package in the repo', () => {
  const index = JSON.parse(readFileSync(join(ROOT, 'packages/index.json'), 'utf8')), scenes = Object.keys(index).filter((id) => index[id].kind === 'scenes');
  assert.ok(scenes.includes('astrology'));
  for (const id of scenes) {
    const src = join(ROOT, index[id].path), dir = fresh(), out = join(TMP, `${id}.rt.json`);
    convert(src, dir);
    const r = build(dir, out);
    assert.deepEqual(r.errors, [], id);
    assert.ok(readFileSync(out).equals(readFileSync(src)), `${id}: the same bytes`);
    assert.ok(r.warnings.filter((w) => w.message.includes('TODO')).length > 10, 'convert cannot know what an input means: every about is reported until someone writes it');
  }
  assert.throws(() => convert(join(ROOT, 'packages/duck/duck.json'), fresh()), /creature package/);
});

test('round trip: a v1 package with its fields in any order comes back equal, name for name and value for value', () => {
  const v1 = {
    v: 'molty-pkg/1', id: 'odd', name: 'Odd', version: '0.1.0', kind: 'scenes',
    assets: { dot: { kind: 'mask', w: 2, h: 2, bits: Buffer.from([0xc0]).toString('base64') }, face: { kind: 'image', w: 2, h: 1, px: Buffer.from([0x21]).toString('base64'), palette: ['#102030', '#ff0000', '#00ff00'] }, world: { kind: 'image', w: 4, h: 2, px: Buffer.from([0x01, 0x10, 0x11, 0x00]).toString('base64'), palette: ['#000000', '#3366ff'] } },
    layouts: {
      'odd-one': { items: [
        { color: '#c8cce0', k: 'ring', x: 0.1, r: '$big-R', y: -0.1, w: 0.012 },
        { k: 'glyph', asset: '$g', each: '$marks', polar: ['@a', 0.5], off: '@t1', size: 0.2, on: '@t0', group: '@Name' },
        { k: 'ticks', n: 1, r: 0.2, a0: 30 }, { k: 'ticks', r: 0.2, n: 1, a0: '$big-R', len: -0.1 },
        { k: 'curve', pts: [[0, 0], ['$big-R', 0.2], [0.4, 0.4]], w: 0.05 }, { k: 'ball', r: 0.2, img: 'world', side: -1, lit: 0.4 },
        { k: 'text', str: 'HI', size: 0.1 }, { k: 'spot', r: 0.2, say: '$say', pts: [[0, 0], [0.5, 0]], lights: '@Name', each: '$marks' },
      ], params: { 'big-R': 'number', g: 'text', marks: 'list', say: 'text' }, caption_y: -0.2, dur: 3 },
      still: { dur: 0, items: [{ k: 'disc', r: 0.1, when: 'lit', group: 'g1' }, { k: 'glyph', asset: 'face', rot: 90 }] },
    },
  };
  assert.deepEqual(checkPackage(v1, 'odd'), []);
  const dir = fresh(), out = join(TMP, 'odd.json');
  convert(folder({ 'odd.json': JSON.stringify(v1) }) + '/odd.json', dir);
  assert.deepEqual(build(dir, out).errors, []);
  assert.deepEqual(JSON.parse(readFileSync(out, 'utf8')), v1);
  assert.match(readFileSync(join(dir, 'display.yaml'), 'utf8'), /big_r: \{ type: number, about: TODO, sent_as: big-R \}/, 'a name the v2 format cannot spell keeps going out as it was');
});

test('the schema: what the checker enforces is what editors read, and it covers every shape the compiler knows', () => {
  const walk = (s, at) => {
    if (!s || typeof s !== 'object' || Array.isArray(s)) return;
    for (const k of Object.keys(s)) assert.ok(KEYWORDS.includes(k) || ANNOTATIONS.includes(k), `${at}: keyword ${k} is not one schema.mjs applies`);
    for (const k of ['properties', '$defs']) for (const [n, c] of Object.entries(s[k] ?? {})) walk(c, `${at}/${n}`);
    for (const k of ['items', 'additionalProperties', 'propertyNames']) walk(s[k], `${at}/${k}`);
    for (const k of ['anyOf', 'oneOf']) (s[k] ?? []).forEach((c, i) => walk(c, `${at}/${k}/${i}`));
  };
  walk(SCHEMA, 'display-2.json');
  for (const [name, { def }] of Object.entries(check(ASTRO).skill.screens)) walk(inputSchema('astrology', name, def, check(ASTRO).files), `inputs/${name}`);
  const entry = SCHEMA.$defs.entry.properties;
  assert.deepEqual(Object.keys(entry).filter((k) => !['for', 'draw', 'use', 'with'].includes(k)), Object.keys(SHAPES));
  for (const [shape, [, fields]] of Object.entries(SHAPES)) {
    const known = [...Object.keys(fields), ...SHAPED, ...(shape === 'tap_zone' ? [] : Object.keys(COMMON))];
    for (const p of Object.keys(SCHEMA.$defs[shape].properties)) assert.ok(known.includes(p), `${shape}.${p} is in the schema; the compiler does not know it`);
    for (const p of Object.keys(fields)) assert.ok(p in SCHEMA.$defs[shape].properties, `${shape}.${p} is in the compiler; the schema does not have it`);
  }
  const screen = JSON.parse(readFileSync(join(ROOT, 'schema/display-2-screen.json'), 'utf8'));
  assert.equal(screen.$ref, 'display-2.json#/$defs/screen');
  assert.deepEqual(validate(SCHEMA, base()), []);
});

test('check, the base skill: nothing to fix, and a problem names its file and line', () => {
  const ok = checked();
  assert.deepEqual([ok.errors, ok.warnings], [[], []]);
  assert.deepEqual(checkPackage(ok.pkg, 'demo'), []);
  const dir = folder({ 'display.yaml': 'molty: display/2\npackage: { id: demo, name: Demo, version: 1.0.0, about: A demo. }\nscreens:\n  one:\n    about: A ring.\n    draw:\n      - circle: { radius: 0.5 }\n      - circle:\n          radius: 0.2\n          colour: ink\n' });
  assert.deepEqual(check(dir).errors, [{ at: 'display.yaml:10', message: 'screens.one.draw[1].circle has no "colour" (did you mean color?)' }]);
  const split = folder({ 'display.yaml': 'molty: display/2\npackage: { id: demo, name: Demo, version: 1.0.0, about: A demo. }\nscreens:\n  one: screens/one.yaml\n  two: screens/two.yaml\n', 'screens/one.yaml': 'about: A ring.\ndraw:\n  - circle: { radius: 0.5 }\n  - spoke: { angle: "{{ turn }}" }\n' });
  assert.deepEqual(check(split).errors.map((e) => e.at), ['display.yaml:5']);
  writeFileSync(join(split, 'screens/two.yaml'), 'about: [\n');
  assert.match(check(split).errors[0].at, /^screens\/two\.yaml:\d+$/);
  writeFileSync(join(split, 'screens/two.yaml'), 'about: A dot.\ndraw:\n  - dot: { radius: 0.1 }\n');
  assert.deepEqual(check(split).errors, [{ at: 'screens/one.yaml:4', message: 'spoke.angle "{{ turn }}": no input named turn (inputs: none)' }]);
});

test('check, layer 1 (the schema): unknown keys, misspelt shapes, wrong types, a bad color, a missing about', () => {
  catches({
    'has no "radus" (did you mean radius?)': (d) => { one(d).draw[0].circle = { radus: 0.5 }; },
    'has no "cirle" (did you mean circle?)': (d) => { one(d).draw.push({ cirle: { radius: 0.5 } }); },
    'an entry draws one thing': (d) => { one(d).draw.push({ dot: { radius: 0.1 }, circle: { radius: 0.2 } }); },
    'must be a number (a fraction of the screen\'s radius), a size token or a quoted binding': (d) => { one(d).draw[0].circle.radius = true; },
    'must be a color token (ink, grid, dim, white, erase or one of your own), #rrggbb': (d) => { one(d).draw[0].circle.color = '#fff'; },
    'screens.one needs about': (d) => { delete one(d).about; },
    'inputs.size needs about': (d) => { delete one(d).inputs.size.about; },
    'inputs.marks.of.angle needs about': (d) => { delete one(d).inputs.marks.of.angle.about; },
    'is a binding without quotes: write "{{ name }}"': (d) => { one(d).draw[0].circle.radius = { '[object Object]': null }; },
    'a place is center, { x, y } or { angle, distance }': (d) => { one(d).draw[0].circle.at = 'middle'; },
    'the name must be a snake_case name': (d) => { one(d).inputs.Size = one(d).inputs.size; },
    'must be display/2': (d) => { d.molty = 'display/3'; },
    'a line has from and to, or points': (d) => { one(d).draw.push({ line: { from: 'center' } }); },
    'must be seconds (0 to 600) or hold': (d) => { one(d).duration = 'forever'; },
    'holds at most 16': (d) => { one(d).draw.push({ line: { points: Array(17).fill('center') } }); },
  });
});

test('check, layer 2 (the device rules): bindings, names, text', () => {
  catches({
    'circle.radius "{{ sise }}": no input named sise (inputs: size, marks, note)': (d) => { one(d).draw[0].circle.radius = '{{ sise }}'; },
    'circle.radius: note is a reading, and circle.radius takes a distance or a number': (d) => { one(d).draw[0].circle.radius = '{{ note }}'; },
    'circle.radius: marks is a list': (d) => { one(d).draw[0].circle.radius = '{{ marks }}'; },
    '{{ mark.angle }} only works inside "for: mark in ..."': (d) => { one(d).draw.push({ spoke: { angle: '{{ mark.angle }}' } }); },
    'symbol.at.angle "{{ mark.angel }}": marks has no field angel (fields: angle, name)': (d) => { one(d).draw[1].draw[0].symbol.at.angle = '{{ mark.angel }}'; },
    'symbol.asset: no asset named "star" (assets: dot, face)': (d) => { one(d).draw[1].draw[0].symbol.asset = 'star'; },
    'globe.map: dot is a symbol, and a globe wears a map': (d) => { one(d).draw.push({ globe: { radius: 0.2, map: 'dot' } }); },
    'circle.color: no color token named red (tokens: ink, grid, dim, white, erase)': (d) => { one(d).draw[0].circle.color = 'red'; },
    'circle.width: no line token named fat': (d) => { one(d).draw[0].circle.width = 'fat'; },
    'tokens.colors.ink reuses a built-in token\'s name': (d) => { d.tokens = { colors: { ink: '#000000' } }; },
    'starts with $, which the device reads as a binding': (d) => { one(d).draw.push({ label: { text: '$5' } }); },
    'starts with @, which the device reads as a binding': (d) => { one(d).draw[2].tap_zone.id = '@home'; },
    'input size is never used': (d) => { one(d).draw[0].circle.radius = 0.5; },
    'shapes is neither a list input nor a data table': (d) => { one(d).draw[1].for = 'mark in shapes'; },
    'a for over the input marks sits inside another': (d) => { one(d).draw[1].draw.push({ for: 'again in marks', draw: [{ dot: { radius: 0.1 } }] }); },
    'no part named wheel': (d) => { one(d).draw.push({ use: 'wheel' }); },
    'the part ring takes no size': (d) => { d.parts = { ring: { draw: [{ circle: { radius: 0.3 } }] } }; one(d).draw.push({ use: 'ring', with: { size: 1 } }); },
    'a part reads only what use passes it': (d) => { d.parts = { ring: { draw: [{ circle: { radius: '{{ size }}' } }] } }; one(d).draw.push({ use: 'ring' }); },
    'has the name of a data table': (d) => { d.data = { marks: [{ angle: 1 }] }; },
    'is sent as size, like another input': (d) => { one(d).inputs.width = { type: 'distance', about: 'Too.', sent_as: 'size' }; one(d).draw.push({ dot: { radius: '{{ width }}' } }); },
    'is out of range (5) (the device\'s own check of the built package)': (d) => { one(d).draw.push({ dot: { radius: 5 } }); },
    'its example must be at most 2': (d) => { one(d).inputs.size.example = 3; },
    'its example[0].angle must be a number': (d) => { one(d).inputs.marks.example = [{ angle: 'up', name: 'a' }]; },
    'tap_zone has both at and along': (d) => { one(d).draw[2].tap_zone.along = { from: 'center', to: { x: 1 } }; one(d).draw[2].tap_zone.at = 'center'; },
  });
  catches({ 'field name of marks is never read': (d) => { delete one(d).draw[1].draw[0].symbol.group; } }, 'warnings');
});

test('check, layer 2: assets exist, are named once, and fit the device', () => {
  catches({
    'assets/none.png does not exist': (d) => { d.assets.push({ file: 'assets/none.png', as: 'symbol' }); },
    'assets/*.gif matches no file': (d) => { d.assets.push({ files: 'assets/*.gif', as: 'symbol' }); },
    'has the same name as assets/dot.png': (d, f) => { f['assets/more/dot.png'] = DOT; d.assets.push({ file: 'assets/more/dot.png', as: 'symbol' }); },
    'has a name the device cannot take': (d, f) => { f['assets/a b.png'] = DOT; d.assets.push({ file: 'assets/a b.png', as: 'symbol' }); },
    'is 129 x 129 px: the device takes at most 128 a side': (d, f) => { f['assets/big.png'] = picture(129); d.assets.push({ file: 'assets/big.png', as: 'picture' }); },
    'is 2 x 2 px but the group says pixels 64': (d) => { d.assets[0].pixels = 64; },
    'uses <path>: a symbol\'s SVG is made of line, polyline and polygon only': (d, f) => { f['assets/p.svg'] = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 2 2"><path d="M0 0L1 1"/></svg>'; d.assets.push({ file: 'assets/p.svg', as: 'symbol', pixels: 8 }); },
    'needs pixels (its width on the device)': (d, f) => { f['assets/l.svg'] = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 2 2" stroke-width="0.2"><line x1="0" y1="0" x2="2" y2="2"/></svg>'; d.assets.push({ file: 'assets/l.svg', as: 'symbol' }); },
    'is an SVG: a picture comes from a PNG': (d, f) => { f['assets/l.svg'] = '<svg viewBox="0 0 2 2"/>'; d.assets.push({ file: 'assets/l.svg', as: 'picture' }); },
    '65 assets: the device holds 64': (d, f) => { for (let i = 0; i < 63; i++) { f[`assets/n${i}.png`] = DOT; d.assets.push({ file: `assets/n${i}.png`, as: 'symbol' }); } },
    'no asset is in assets/planets': (d) => { one(d).inputs.pick = { type: 'asset', of: 'assets/planets', about: 'One.' }; one(d).draw.push({ symbol: { asset: '{{ pick }}' } }); },
  });
  assert.deepEqual(checked((d, f) => { for (let i = 0; i < 62; i++) { f[`assets/n${i}.png`] = DOT; d.assets.push({ file: `assets/n${i}.png`, as: 'symbol' }); } }).errors, [], '64 assets fit');
});

test('check: what needs firmware is refused, with the way round it', () => {
  catches({
    'move needs firmware that slides a shape along keyframes': (d) => { one(d).draw.push({ dot: { radius: 0.1, move: { along: '{{ marks }}' } } }); },
    'default needs firmware that fills in a missing input': (d) => { one(d).inputs.size.default = 0.5; },
    'type path goes with move, which needs firmware': (d) => { one(d).inputs.size.type = 'path'; },
    'a point read from inputs must be { x, y }': (d) => { one(d).draw[1].draw.push({ line: { from: 'center', to: { angle: '{{ mark.angle }}', distance: 0.5 } } }); },
    'mixes words with an input: the device builds no text': (d) => { one(d).draw[2].tap_zone.reads = 'It says {{ note }}'; },
    'a binding fills the whole value': (d) => { one(d).draw[1].draw[0].symbol.asset = 'g-{{ note }}'; },
    'must be a number (a fraction of the screen\'s radius), a size token or a quoted binding': (d) => { one(d).draw[0].circle.radius = '{{ size }} * 2'; },
  });
  // a literal polar point is worked out here; lit_side takes the words
  const ok = checked((d) => { one(d).draw.push({ line: { from: 'center', to: { angle: 90, distance: 0.5 } } }, { globe: { radius: 0.1, lit_side: 'left' } }); });
  assert.deepEqual(ok.errors, []);
  assert.deepEqual(ok.pkg.layouts.one.items.slice(-2), [{ k: 'line', pts: [[0, 0], [0, 0.5]] }, { k: 'ball', r: 0.1, side: -1 }]);
});

test('a tap zone as a wedge of a ring, and a tap that opens another screen', () => {
  // two screens: a wheel of slices, and the close-up a slice opens with what the wheel was sent
  const wheel = (d, f) => {
    one(d).inputs.slices = { type: 'list', max: 4, about: 'The slices.', of: { angle: { type: 'angle', about: 'Its middle.' }, id: { type: 'text', about: 'Its name.' }, title: { type: 'text', about: 'The close-up\'s word.' }, secs: { type: 'seconds', min: 1, about: 'How long the close-up stays.' } }, example: [{ angle: 45, id: 'ne', title: 'NE', secs: 5 }, { angle: 135, id: 'nw', title: 'NW', secs: 7 }] };
    one(d).inputs.dots = { type: 'list', max: 8, about: 'Marks for the close-up.', of: { slice: { type: 'text', sent_as: 'of', about: 'The slice it belongs to.' }, x: { type: 'number', about: 'Where.' } }, example: [{ slice: 'ne', x: 0.1 }, { slice: 'nw', x: 0.2 }, { slice: 'ne', x: 0.3 }] };
    one(d).draw.push({ for: 's in slices', draw: [{ tap_zone: { at: { angle: '{{ s.angle }}', distance: 0.7 }, radius: 0.05, reads: 'A slice.', id: '{{ s.id }}', sector: { width: 90, inner: 0.5, outer: 0.9 }, opens: { screen: 'close', seconds: '{{ s.secs }}' } } }] });
    one(d).draw.push({ tap_zone: { reads: 'South.', id: 'south', sector: { from: 225, to: 315, inner: 0.5, outer: 0.9 }, opens: 'plain' } });
    d.screens.close = { about: 'One slice up close.', inputs: { title: { type: 'text', about: 'Its word.', example: 'NE' }, size: { type: 'distance', about: 'The ring.', example: 0.5 }, dots: { type: 'list', max: 8, about: 'Its marks.', of: { x: { type: 'number', about: 'Where.' } }, example: [{ x: 0.1 }] } },
      draw: [{ label: { text: '{{ title }}' } }, { circle: { radius: '{{ size }}' } }, { for: 'm in dots', draw: [{ dot: { radius: 0.03, at: { x: '{{ m.x }}' } } }] }] };
    d.screens.plain = { about: 'A dot.', draw: [{ dot: { radius: 0.1 } }] };
    f['SKILL.md'] += '[close](display.json#close) [plain](display.json#plain)\n';
  };
  const r = checked(wheel), items = r.pkg.layouts.one.items;
  assert.deepEqual([r.errors, r.warnings], [[], []], 'what only the opened screen reads counts as read');
  assert.deepEqual(items.at(-2), { k: 'spot', polar: ['@angle', 0.7], r: 0.05, say: 'A slice.', id: '@id', arc: 90, r0: 0.5, r1: 0.9, open: 'close', ttl: '@secs', each: '$slices' });
  assert.deepEqual(items.at(-1), { k: 'spot', say: 'South.', id: 'south', polar: [270, 0.7], r: 0.2, arc: 90, r0: 0.5, r1: 0.9, open: 'plain' }, 'from one angle to another: the zone is the wedge');
  // the device's rules, on the built package: a tap in a wedge lands on its zone; what it opens is found by name
  const params = { size: 0.5, marks: [{ angle: 90, name: 'a' }], note: 'Hello.', slices: [{ angle: 45, id: 'ne', title: 'NE', secs: 5 }, { angle: 135, id: 'nw', title: 'NW', secs: 7 }], dots: [{ of: 'ne', x: 0.1 }, { of: 'nw', x: 0.2 }, { of: 'ne', x: 0.3 }] };
  const at = (deg, dist) => spotAt(bind(r.pkg, 'one', params), dist * Math.cos((deg * Math.PI) / 180), dist * Math.sin((deg * Math.PI) / 180), 1)?.id;
  assert.deepEqual([at(10, 0.8), at(170, 0.6), at(270, 0.55), at(45, 0.95), at(0, 0)], ['ne', 'nw', 'south', undefined, 'middle']);
  assert.deepEqual(opens(r.pkg, 'one', params, 'ne'), { layout: 'close', params: { title: 'NE', size: 0.5, dots: [{ of: 'ne', x: 0.1 }, { of: 'ne', x: 0.3 }] }, ttl: 5 });
  assert.equal(bind(r.pkg, 'close', opens(r.pkg, 'one', params, 'nw').params).filter((it) => it.k === 'disc').length, 1, 'the opened screen plays with them');
  const broken = (mut) => checked((d, f) => { wheel(d, f); mut(d); }).errors.map((e) => e.message);
  const cases = {
    'tap_zone.opens: no screen named "closer" (screens: one, close, plain)': (d) => { one(d).draw.at(-2).draw[0].tap_zone.opens.screen = 'closer'; },
    'tap_zone.opens: close needs title, and neither an element of slices nor this screen sends one. Add the field title to slices': (d) => { delete one(d).inputs.slices.of.title; },
    'tap_zone.opens: close needs dots, and neither an element of slices nor this screen sends one. Add the input dots here': (d) => { delete one(d).inputs.dots; },
    'tap_zone.opens: close reads x from each element of dots, and dots here has no such field (fields: of)': (d) => { delete one(d).inputs.dots.of.x; },
    'tap_zone.opens: close takes title as a text, and here it is a seconds': (d) => { one(d).inputs.slices.of.title = { type: 'seconds', about: 'Wrong.' }; },
    'tap_zone.sector: a sector with from and to places the zone itself, so drop at': (d) => { one(d).draw.at(-1).tap_zone.at = { x: 0.1 }; },
    'tap_zone.sector: it runs from a smaller angle to a larger one': (d) => { one(d).draw.at(-1).tap_zone.sector.to = 200; },
    'a sector is { from, to, inner, outer } (two angles and two distances) or { width, inner, outer }': (d) => { delete one(d).draw.at(-1).tap_zone.sector.outer; },
    'tap_zone.opens needs screen': (d) => { one(d).draw.at(-1).tap_zone.opens = { seconds: 3 }; },
    'opens is a screen\'s name, or { screen, seconds }': (d) => { one(d).draw.at(-1).tap_zone.opens = 5; },
    'tap_zone needs radius (how far from at a tap still counts), or a sector with from and to': (d) => { delete one(d).draw.at(-2).draw[0].tap_zone.radius; },
    'ttl is out of range (0)': (d) => { one(d).draw.at(-2).draw[0].tap_zone.opens.seconds = 0; },
    'arc is out of range (400)': (d) => { one(d).draw.at(-2).draw[0].tap_zone.sector.width = 400; },
  };
  for (const [want, mut] of Object.entries(cases)) assert.ok(broken(mut).some((m) => m.includes(want)), `${want}: got ${JSON.stringify(broken(mut))}`);
  // back out of the package: the same two forms
  const dir = fresh(), out = join(TMP, 'wheel.json');
  convert(folder({ 'demo.json': r.bytes }) + '/demo.json', dir);
  assert.deepEqual(build(dir, out).errors, []);
  assert.deepEqual(JSON.parse(readFileSync(out, 'utf8')), r.pkg);
});

test('the astrology chart: a tap on a sign opens its close-up with what the skill itself would send', () => {
  const run = (...args) => execFileSync(process.execPath, [join(ASTRO, 'scripts/astro.mjs'), ...args, '--date', '2026-11-01', '--dry-run'], { encoding: 'utf8', env: { ...process.env, MOLTY_PKG_INDEX: join(ROOT, 'packages/index.json') } }).split('\n').filter((l) => l.startsWith('{')).map((l) => JSON.parse(l));
  const pkg = JSON.parse(readFileSync(GOLDEN, 'utf8')), chart = run('chart').find((c) => c.args.layout === 'chart').args.params;
  const leo = chart.signs.find((s) => s.id === 'leo'), rad = (leo.a * Math.PI) / 180, items = bind(pkg, 'chart', chart);
  assert.equal(spotAt(items, 0.88 * Math.cos(rad + 0.2), 0.88 * Math.sin(rad + 0.2), 1).id, 'leo', 'anywhere in the sign\'s block of the ring');
  const up = opens(pkg, 'chart', chart, 'leo'), sent = run('chart', '--interactive', '--tap', 'leo').find((c) => c.args.layout === 'sign-closeup').args;
  assert.deepEqual([up.layout, up.ttl], ['sign-closeup', sent.ttl_s]);
  assert.deepEqual(JSON.parse(JSON.stringify(up.params, (k, v) => (k === 'of' || ['a', 'r'].includes(k) ? undefined : v))), JSON.parse(JSON.stringify(sent.params, (k, v) => (['a', 'r'].includes(k) ? undefined : v))));
});

test('check, layer 3 (the links): SKILL.md names real screens, and every screen', () => {
  catches({ 'links to screen two, which display.json does not have (screens: one)': (d, f) => { f['SKILL.md'] += 'And [two](display.json#two).\n'; } });
  catches({
    'never mentions the screen one': (d, f) => { f['SKILL.md'] = '# Demo\n'; },
    'no SKILL.md yet': (d, f) => { delete f['SKILL.md']; },
    'about is still TODO': (d) => { one(d).inputs.size.about = 'TODO'; },
    'give every input an example': (d) => { delete one(d).inputs.size.example; },
  }, 'warnings');
});

test('limits: 64 items a screen, 8 inputs, 32 screens, 128 items and 40 letters and 32 groups at the fullest, 256 KB', () => {
  const dots = (n) => Array.from({ length: n }, () => ({ dot: { radius: 0.1 } })), lacks = (r, want) => assert.ok(r.errors.some((e) => e.message.includes(want)), `${want}: got ${JSON.stringify(r.errors)}`);
  // items written out: the base has 3
  assert.deepEqual(checked((d) => { one(d).draw.push(...dots(LIMITS.items - 3)); }).errors, []);
  lacks(checked((d) => { one(d).draw.push(...dots(LIMITS.items - 2)); }), 'writes out 65 items: the device takes 64 a screen');
  lacks(checked((d) => { d.data = { twelve: Array.from({ length: 12 }, (_, i) => ({ a: i * 30 })) }; one(d).draw.push({ for: 'x in twelve', draw: [{ spoke: { angle: '{{ x.a }}' } }, { dot: { radius: 0.1 } }, ...dots(4)] }); }), 'writes out 75 items');
  // inputs
  const more = (n) => (d) => { for (let i = 0; i < n; i++) { one(d).inputs[`n${i}`] = { type: 'distance', about: 'More.', example: 0.1 }; one(d).draw.push({ dot: { radius: `{{ n${i} }}` } }); } };
  assert.deepEqual(checked(more(LIMITS.params - 3)).errors, []);
  lacks(checked(more(LIMITS.params - 2)), 'screens.one.inputs holds at most 8 (it has 9)');
  // screens
  const screens = (n) => (d, f) => { for (let i = 1; i < n; i++) { d.screens[`s${i}`] = { about: 'A dot.', draw: dots(1) }; f['SKILL.md'] += `[s${i}](display.json#s${i})\n`; } };
  assert.deepEqual(checked(screens(LIMITS.layouts)).errors, []);
  lacks(checked(screens(LIMITS.layouts + 1)), 'screens holds at most 32 (it has 33)');
  // the fullest screen: 2 still items + marks.max x the shapes repeated
  const full = (max, per, still = 0) => (d) => { one(d).inputs.marks.max = max; one(d).draw[1].draw.push(...dots(per - 1)); one(d).draw.push(...dots(still)); };
  assert.equal(checked(full(21, 6)).screens.one.fullest, LIMITS.scene);
  assert.deepEqual(checked(full(21, 6)).errors, []);
  lacks(checked(full(21, 6, 1)), 'can reach 129 items (9 written, with 21 marks): the device shows at most 128 at once');
  lacks(checked((d) => { delete one(d).inputs.marks.max; one(d).draw[1].draw.push(...dots(3)); }), 'can reach 130 items (6 written, with 32 marks)');
  lacks(checked((d) => { one(d).inputs.marks.max = 33; }), 'max must be a whole number from 1 to 32 for a list');
  // letters and groups
  const label = (d, max) => { one(d).inputs.title = { type: 'text', about: 'A word.', example: 'HI', ...(max ? { max } : {}) }; one(d).draw.push({ label: { text: '{{ title }}' } }, { label: { text: 'TEN LETTER' } }); };
  assert.deepEqual(checked((d) => label(d, 30)).errors, []);
  lacks(checked((d) => label(d, 31)), 'can reach 41 letters in its labels: the device shows at most 40');
  lacks(checked((d) => label(d)), 'can reach 50 letters');
  assert.deepEqual(checked((d) => { one(d).inputs.marks.max = LIMITS.groups; }).errors, []);
  lacks(checked((d) => { one(d).inputs.marks.max = LIMITS.groups; one(d).draw.push({ dot: { radius: 0.1, group: 'extra' } }); }), 'can name 33 groups: the device lights at most 32');
  // the package's size: each 128 x 128 picture is about 10.9 KB of it
  const pictures = (n) => (d, f) => { for (let i = 0; i < n; i++) { f[`assets/p${i}.png`] = picture(128, i + 2); d.assets.push({ file: `assets/p${i}.png`, as: 'picture' }); } };
  const fits = checked(pictures(23)), over = checked(pictures(24));
  assert.ok(fits.errors.length === 0 && fits.bytes.length <= MAX_PKG_BYTES && over.bytes.length > MAX_PKG_BYTES);
  lacks(over, 'the device takes 256.0 KB. Its largest assets: p0 ');
});

test('assets: a PNG as a symbol, a picture kept as it is or cut to 15 colors; an SVG\'s lines as a mask', () => {
  const rgba = (w, h, px) => { const head = Buffer.alloc(13); head.writeUInt32BE(w, 0); head.writeUInt32BE(h, 4); head[8] = 8; head[9] = 6; return { w, h, rows: Buffer.concat(Array.from({ length: h }, (_, y) => Buffer.from([0, ...px.slice(y * w, (y + 1) * w).flat()]))), head }; };
  const dir = folder({ 'a/face.png': picture(6, 3), 'a/dot.png': DOT, 'a/tick.svg': '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 8 4" stroke-width="2"><line x1="0" y1="2" x2="8" y2="2"/></svg>', 'a/ring.svg': '<svg xmlns="http://www.w3.org/2000/svg" viewBox="-1 -1 2 2"><polygon points="-0.5,-0.5 0.5,-0.5 0.5,0.5 -0.5,0.5"/></svg>' });
  const face = readPng(readFileSync(join(dir, 'a/face.png')));
  assert.deepEqual([face.w, face.h, face.palette.length, face.palette[0][3], face.palette[1][3], face.clear], [6, 6, 16, 0, 255, true]);
  const bad = [], { assets } = buildAssets(dir, [{ files: 'a/{face,dot}.png', as: 'picture' }, { file: 'a/tick.svg', as: 'symbol', pixels: 8 }, { file: 'a/ring.svg', as: 'symbol', pixels: 4, stroke: 0.5 }], (n, m) => bad.push(m));
  assert.deepEqual(bad, []);
  assert.deepEqual(Object.keys(assets), ['face', 'dot', 'tick', 'ring'], 'in the order written');
  assert.ok(assetPng(assets.face).equals(readFileSync(join(dir, 'a/face.png'))), 'an indexed picture comes back as it went');
  assert.deepEqual([assets.dot.kind, assets.dot.palette], ['image', ['#000000', '#ffffff', '#000000']], 'a grey PNG as a picture: its colors as they come');
  assert.deepEqual(assets.tick, { kind: 'mask', w: 8, h: 4, bits: Buffer.from([0x00, 0xff, 0xff, 0x00]).toString('base64') }, 'a line 2 wide in a box 4 tall covers its two middle rows');
  assert.deepEqual(assets.ring, { kind: 'mask', w: 4, h: 4, bits: Buffer.from([0x6f, 0xf6]).toString('base64') }, 'the group\'s stroke (a fraction of half the drawing\'s width) over the SVG\'s own; corners round');
  // more than 15 colors: the 15 that fit best, the same ones every time; see-through stays index 0
  const many = Array.from({ length: 64 }, (_, i) => [i * 4, 255 - i * 4, (i * 37) % 256, i === 0 ? 0 : 255]), p = rgba(8, 8, many);
  writeFileSync(join(dir, 'a/many.png'), Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', p.head), chunk('IDAT', deflateSync(p.rows)), chunk('IEND', Buffer.alloc(0))]));
  const cut = () => buildAssets(dir, [{ file: 'a/many.png', as: 'picture' }, { file: 'a/many.png', as: 'symbol' }], () => {}).assets.many;
  const m = cut(), px = Buffer.from(m.px, 'base64');
  assert.deepEqual([m.palette.length, px[0] >> 4, px[0] & 15 ? 'drawn' : 'clear', JSON.stringify(cut())], [16, 0, 'drawn', JSON.stringify(m)]);
  assert.deepEqual(checkPackage({ v: 'molty-pkg/1', id: 'a', name: 'A', version: '1.0.0', kind: 'scenes', assets, layouts: { l: { dur: 0, items: [{ k: 'glyph', asset: 'face' }] } } }, 'a'), []);
});

test('the molty-astro skill sends what its screens declare: every dry-run command fits build/inputs', () => {
  let seen = 0;
  for (const scene of [['tonight'], ['chart', '--all'], ['chart', '--interactive', '--tap', 'leo'], ['signs'], ['aspects'], ['events']]) {
    const out = execFileSync(process.execPath, [join(ASTRO, 'scripts/astro.mjs'), ...scene, '--date', '2026-11-01', '--dry-run'], { encoding: 'utf8', env: { ...process.env, MOLTY_PKG_INDEX: join(ROOT, 'packages/index.json') } });
    for (const c of out.split('\n').filter((l) => l.startsWith('{')).map((l) => JSON.parse(l)).filter((c) => c.name.startsWith('scene.') && c.args.layout)) {
      const schema = JSON.parse(readFileSync(join(ASTRO, 'build/inputs', `${c.args.layout}.schema.json`), 'utf8'));
      assert.deepEqual(validate(schema, c.args.params), [], `${scene.join(' ')}: ${c.args.layout}`);
      seen++;
    }
  }
  assert.ok(seen >= 20, `${seen} commands`);
});

test('the dial example: one file, checks clean, builds what is committed, plays in the reference player', () => {
  const dir = fresh();
  cpSync(DIAL, dir, { recursive: true });
  rmSync(join(dir, 'build'), { recursive: true });
  const r = build(dir);
  assert.deepEqual([r.errors, r.warnings], [[], []]);
  for (const f of tree(join(dir, 'build'))) assert.equal(readFileSync(join(dir, 'build', f), 'utf8'), readFileSync(join(DIAL, 'build', f), 'utf8'), `docs/examples/molty-dial/build/${f} is stale: run molty-display build`);
  assert.deepEqual(checkPackage(r.pkg, 'dial'), []);
  const params = exampleParams(r.skill.screens.dial.def), dots = frame(r.pkg, 'dial', params, 1), other = frame(r.pkg, 'dial', { ...params, value: 300 }, 1);
  assert.ok(dots.size > 150, `the dial draws ${dots.size} dots`);
  assert.ok([...dots.keys()].some((k) => !other.has(k)), 'the pointer follows the value');
  assert.match(readFileSync(join(dir, 'build/display.md'), 'utf8'), /"layout":"dial","params":\{"value":135,"reading":"Humidity 64 percent, a bit damp\."\}/);
});

test('docs/DISPLAY.md: its first screen builds as written, and the call it shows is the one the build writes', () => {
  const md = readFileSync(join(ROOT, 'docs/DISPLAY.md'), 'utf8'), dir = folder({ 'display.yaml': /```yaml\n([\s\S]*?)```/.exec(md)[1] }), r = build(dir);
  assert.deepEqual(r.errors, []);
  assert.ok(frame(r.pkg, 'hello', { angle: 90 }, 1).size > 150);
  const call = /```json\n(.*)\n```/.exec(md)[1];
  assert.ok(readFileSync(join(dir, 'build/display.md'), 'utf8').includes(call), call);
});

test('preview: a local page that draws a screen with the reference player, a tapped zone lit', async () => {
  const server = preview(ASTRO, 0);
  await new Promise((ok) => server.once('listening', ok));
  const at = `http://127.0.0.1:${server.address().port}`, get = (p) => fetch(at + p);
  try {
    assert.equal(server.address().address, '127.0.0.1');
    assert.match(await (await get('/')).text(), /molty-display preview/);
    const skill = await (await get('/skill')).json(), chart = skill.screens.find((s) => s.name === 'chart');
    const show = async (screen, params) => (await fetch(`${at}/scene`, { method: 'POST', body: JSON.stringify({ screen, params: JSON.stringify(params, null, 1) }) })).json();
    assert.deepEqual([skill.problems, skill.screens.length], [[], 12]);
    assert.equal((await show('chart', chart.params)).zones, 10 + 12 + 3, 'a zone for each body, sign and aspect of the example');
    const calm = Buffer.from(await (await get('/frame.png?t=1')).arrayBuffer()), lit = Buffer.from(await (await get('/frame.png?t=1&tap=sun-moon')).arrayBuffer());
    assert.equal(calm.subarray(1, 4).toString(), 'PNG');
    assert.ok(!calm.equals(lit), 'the tapped aspect draws differently');
    const leo = chart.params.signs.find((s) => s.id === 'leo'), rad = (leo.a * Math.PI) / 180, hit = await (await get(`/tap?t=1&x=${0.88 * Math.cos(rad + 0.2)}&y=${0.88 * Math.sin(rad + 0.2)}`)).json();
    assert.deepEqual([hit.id, hit.opens.layout, hit.opens.params.sign, hit.opens.params.bodies.every((b) => b.of === 'leo')], ['leo', 'sign-closeup', 'leo', true], 'a tap in a sign\'s block opens its close-up');
    assert.deepEqual(await (await get('/tap?t=1&x=0&y=0')).json(), {}, 'the middle is empty');
    assert.equal((await show('sign-closeup', hit.opens.params)).zones, hit.opens.params.bodies.length + 1, 'the opened screen shows with what the chart was sent');
    assert.match((await show('chart', {})).error, /the device would refuse these inputs: missing param/);
  } finally { server.close(); }
});
