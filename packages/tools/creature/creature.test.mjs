// SPDX-License-Identifier: MIT
// node --test packages/tools/creature/: the Molty format (docs/CREATURE.md) breaks nothing and checks what it says.
//   golden     the cat's folder (molty.json + cat.glb) builds packages/cat/cat.json byte for byte, with no Blender
//              to be found; when Blender 5.3 is there, the exporter still writes that cat.glb from cat.blend
//   surface    a second Molty made by editing only the JSON; one written by hand with no 3D tool
//   checks     each rule fails a Molty that breaks it, naming the file and the fix; the device's limits
import assert from 'node:assert/strict';
import { execFileSync, spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { cpSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { after, test } from 'node:test';
import { fileURLToPath } from 'node:url';
import { clipDots } from '../../../device/creatures/js/clip-dots.js';
import { packClip, unpackClip } from '../../../device/creatures/js/clip-pack.js';
import { clipPlayer, decodeClip } from '../../../device/creatures/js/clip-player.js';
import { ANNOTATIONS, KEYWORDS, validate } from '../display/schema.mjs';
import { checkPackage } from '../format.mjs';
import { bakeAction, r6 } from './bake.mjs';
import { readGltf } from './body.mjs';
import { LIMITS, SCHEMA, build, check } from './build.mjs';
import { preview } from './preview.mjs';

const ROOT = fileURLToPath(new URL('../../../', import.meta.url)), CAT = join(ROOT, 'moltys/cat'), PIP = join(ROOT, 'docs/examples/molty-pip');
const GOLDEN = join(ROOT, 'packages/cat/cat.json'), GOLDEN_SHA = 'a49bd3ec39abad3e630b909ba2055c4b78ca52f123f14a24a43a22bf7848cd96';
const CLI = join(ROOT, 'packages/tools/molty-creature.mjs'), EXPORTER = join(ROOT, 'tools/blender/molty_export.py');
const TMP = mkdtempSync(join(tmpdir(), 'molty-creature-'));
after(() => rmSync(TMP, { recursive: true, force: true }));
let made = 0;
const fresh = () => join(TMP, String(made++));
const sha = (bytes) => createHash('sha256').update(bytes).digest('hex');
/** How many dots of the Molty itself (not the floor or the dust) are lit after `frames` frames. */
function drawn(rig, frames, ask = () => {}) {
  const p = clipPlayer(rig), r = clipDots(rig, 52, 32);
  ask(p);
  for (let i = 0; i < frames; i++) p.update(1 / 24);
  return { dots: r.draw(p.world, p.cam, frames).pri.filter((v) => v >= 1).length, cells: r.cells(), clip: p.clip };
}

// ---- a small Molty, and ways to break it
const base = () => JSON.parse(readFileSync(join(PIP, 'molty.json'), 'utf8'));
function checked(mut = () => {}) {
  const doc = base(), dir = fresh();
  mut(doc);
  mkdirSync(dir, { recursive: true });
  writeFileSync(join(dir, 'molty.json'), JSON.stringify(doc, null, 1));
  return check(dir);
}
function catches(cases, list = 'errors') {
  for (const [want, mut] of Object.entries(cases)) {
    const found = checked(mut)[list];
    assert.ok(found.some((e) => e.message.includes(want) && /^molty\.json:\d+$/.test(e.at)), `${want}: got ${JSON.stringify(found)}`);
  }
}

test('golden: the cat\'s folder builds today\'s cat.json byte for byte, with no Blender on the PATH', () => {
  const out = join(TMP, 'cat.json');
  // the build as a command, in a world where only node exists
  const run = spawnSync(process.execPath, [CLI, 'build', CAT, '--out', out], { encoding: 'utf8', env: { PATH: dirname(process.execPath), HOME: TMP } });
  assert.equal(run.status, 0, run.stderr);
  assert.equal(spawnSync('blender', ['--version'], { env: { PATH: dirname(process.execPath) } }).error?.code, 'ENOENT', 'no blender in that world');
  const bytes = readFileSync(out);
  assert.equal(sha(bytes), GOLDEN_SHA);
  assert.ok(bytes.equals(readFileSync(GOLDEN)), 'the built Molty is packages/cat/cat.json');
  assert.equal(bytes.length, 161526);
  const r = check(CAT);
  assert.deepEqual([r.errors, r.warnings], [[], []]);
  assert.equal(r.doc.about, readFileSync(join(ROOT, 'packages/cat/description.txt'), 'utf8').trim(), 'the catalog line is molty.json\'s about');
});

test('the cat: what its names wire up, with nothing said about it in molty.json', () => {
  const g = check(CAT).pkg.creature.rig.graph, clips = check(CAT).pkg.creature.rig.clips;
  assert.deepEqual(g.rest, { sit: 'idle', stand: 'stand_idle', loaf: 'loaf_idle', crouch: 'crouch_idle', walk: 'walk' }, 'a pose rests in <pose>_idle, <pose>, or idle');
  assert.deepEqual([g.edges['sit>stand'], g.edges['sit>walk'], g.edges['walk>sit'], g.edges['sit>loaf'], g.edges['loaf>sit']], ['sit_to_stand', 'walk_start', 'walk_stop', 'lie_down', 'get_up'], 'a way is <a>_to_<b>, or the one free action that goes that way');
  assert.equal(Object.keys(g.edges).length, 16);
  assert.deepEqual(g.enter, { surprised: { sit: 'sit_surprised', stand: 'stand_surprised' } }, '<pose>_<mood> is the way into a mood');
  assert.deepEqual([clips.bounce.from, clips.bounce.to, clips.sleepy.from, clips.surprised.to, clips.walk_stop.from], ['sit', 'crouch', 'loaf', 'stand', 'walk'], 'the pose an action starts and ends in is worked out');
  assert.deepEqual(Object.keys(clips).filter((n) => clips[n].loop).sort(), ['crouch_idle', 'happy', 'idle', 'listening', 'loaf_idle', 'sleepy', 'speaking', 'stand_idle', 'surprised', 'thinking', 'walk'], 'moods and resting actions loop');
});

test('the exporter (only where Blender 5.3 is): cat.blend still gives the committed cat.glb', { skip: !/^Blender 5\.3\./.test(spawnSync('blender', ['--version'], { encoding: 'utf8' }).stdout ?? '') && 'Blender 5.3 is not on the PATH' }, () => {
  // every action when MOLTY_EXPORT_ALL is set (minutes); else a few, compared key for key with the committed file's
  const all = !!process.env.MOLTY_EXPORT_ALL, some = ['idle', 'blink', 'walk_start', 'eat_dish'], out = join(TMP, 'cat.glb');
  const run = spawnSync('blender', ['-b', join(CAT, 'cat.blend'), '-P', EXPORTER, '--', '--out', out, ...(all ? [] : ['--actions', some.join(',')])], { encoding: 'utf8' });
  assert.equal(run.status, 0, run.stderr.slice(-2000));
  if (all) return void assert.ok(readFileSync(out).equals(readFileSync(join(CAT, 'cat.glb'))), 'the same file, byte for byte');
  const got = readGltf(out), want = readGltf(join(CAT, 'cat.glb'));
  assert.deepEqual([got.nodes, got.shapes], [want.nodes, want.shapes]);
  assert.deepEqual(got.actions.map((a) => a.name), want.actions.map((a) => a.name).filter((n) => some.includes(n)));
  for (const a of got.actions) assert.deepEqual(a, want.actions.find((w) => w.name === a.name), a.name);
});

test('the exporter says which Blender it needs', () => {
  const src = readFileSync(EXPORTER, 'utf8');
  assert.match(src, /NEEDS = \(5, 3\)/);
  assert.match(src, /bpy\.app\.version\[:2\] != NEEDS[\s\S]{0,40}fail\('needs Blender %d\.%d; this is %s/);
});

test('a second Molty from the cat, by editing only its JSON: renamed, recoloured, a gesture remapped, a mapping dropped', () => {
  const dir = fresh();
  mkdirSync(dir);
  cpSync(join(CAT, 'cat.glb'), join(dir, 'cat.glb'));
  const doc = JSON.parse(readFileSync(join(CAT, 'molty.json'), 'utf8'));
  Object.assign(doc, { id: 'ember', name: 'Ember', about: 'A ginger cat with amber eyes.', look: 'A slim ginger cat drawn as a fine outline, with amber eyes and a cream chest.' });
  Object.assign(doc.colors, { body: '#f0a060', accent: '#c86a30', iris: '#ffd060', belly: '#fff4e0' });
  doc.gestures.inspect = 'goes low and peers, the eyes sweeping slowly';                       // the mapping dropped: inspect would play an action named inspect
  doc.gestures.wiggle = { about: 'a big stretch instead of a shimmy', plays: 'stretch' };      // remapped
  writeFileSync(join(dir, 'molty.json'), JSON.stringify(doc, null, 2));
  const miss = check(dir);
  assert.deepEqual(miss.errors.map((e) => [e.at, e.message.slice(0, 96)]), [['molty.json:56', 'gesture inspect: there is no action named inspect. Name an action inspect, or say which one it p']], 'the name alone must find an action');
  delete doc.gestures.inspect;
  doc.gestures.lie_down = 'folds down into a loaf';                                            // a new gesture, wired by its name alone
  writeFileSync(join(dir, 'molty.json'), JSON.stringify(doc, null, 2));
  const r = build(dir);
  assert.deepEqual(r.errors, []);
  assert.equal(r.out, join(dir, 'build', 'ember.json'), 'outside moltys/ it builds into its own folder');
  const pkg = JSON.parse(readFileSync(r.out, 'utf8')), cat = JSON.parse(readFileSync(GOLDEN, 'utf8')), g = pkg.creature.rig.graph;
  assert.deepEqual(checkPackage(pkg, 'ember'), []);
  assert.deepEqual([g.gestures.wiggle, g.gestures.lie_down, g.gestures.inspect, pkg.creature.gestures.lie_down], ['stretch', 'lie_down', undefined, { dur: 3, ch: [] }]);
  assert.deepEqual([pkg.creature.rig.palette.body, pkg.creature.body.palette.iris, pkg.creature.rig.palette.nose], ['#f0a060', '#ffd060', cat.creature.rig.palette.nose]);
  assert.deepEqual(pkg.creature.rig.clips, cat.creature.rig.clips, 'the same body file: the same clips');
  assert.equal(g.edges['sit>loaf'], undefined, 'lie_down is a gesture now, so it is no longer the free way from sit to loaf');
  const a = drawn(pkg.creature.rig, 30), b = drawn(cat.creature.rig, 30);
  assert.ok(a.dots > 300 && a.dots === b.dots, `it draws as the cat does (${a.dots} dots)`);
  assert.notDeepEqual([...a.cells.col], [...b.cells.col], 'in its own colors');
});

test('a Molty written by hand, with no 3D tool: it builds what is committed, moves and hops', () => {
  const dir = fresh();
  cpSync(PIP, dir, { recursive: true });
  rmSync(join(dir, 'build'), { recursive: true });
  const r = build(dir);
  assert.deepEqual([r.errors, r.warnings], [[], []]);
  assert.equal(readFileSync(r.out, 'utf8'), readFileSync(join(PIP, 'build/pip.json'), 'utf8'), 'docs/examples/molty-pip/build/pip.json is stale: run molty-creature build');
  assert.deepEqual(checkPackage(r.pkg, 'pip'), []);
  const rig = r.pkg.creature.rig, calm = drawn(rig, 30), up = drawn(rig, 14, (p) => p.act('hop'));
  assert.deepEqual([rig.nodes.length, rig.shapes.length, Object.keys(rig.clips), rig.clips.idle.loop, rig.clips.hop.dur, rig.clips.hop.frames], [4, 7, ['idle', 'hop'], true, 1.1, 27]);
  assert.ok(calm.dots > 100 && up.clip === 'hop', `${calm.dots} dots; asked to hop it plays ${up.clip}`);
  assert.notDeepEqual([...calm.cells.dots], [...up.cells.dots], 'mid hop it is somewhere else');
});

test('docs/CREATURE.md: its first Molty builds as written', () => {
  const dir = fresh();
  mkdirSync(dir);
  writeFileSync(join(dir, 'molty.json'), /```json\n([\s\S]*?)```/.exec(readFileSync(join(ROOT, 'docs/CREATURE.md'), 'utf8'))[1]);
  const r = build(dir);
  assert.deepEqual(r.errors, []);
  assert.ok(drawn(r.pkg.creature.rig, 20).dots > 100);
});

test('docs/CREATURE.md: its life examples build, Blip\'s breath and the cat\'s', () => {
  const doc = readFileSync(join(ROOT, 'docs/CREATURE.md'), 'utf8'), [blip, cat] = [...doc.slice(doc.indexOf('## Life')).matchAll(/```json\n([\s\S]*?)```/g)].map((m) => m[1]);
  const first = JSON.parse(/```json\n([\s\S]*?)```/.exec(doc)[1]), dir = fresh();
  mkdirSync(dir);
  writeFileSync(join(dir, 'molty.json'), JSON.stringify({ ...first, ...JSON.parse(`{${blip}}`) }));
  const r = build(dir);
  assert.deepEqual(r.errors, []);
  assert.deepEqual(r.pkg.creature.life.layers, [{ by: 'breath', nodes: [1], sq: 0.04 }]);
  const cdir = fresh();
  mkdirSync(cdir);
  cpSync(join(CAT, 'cat.glb'), join(cdir, 'cat.glb'));
  writeFileSync(join(cdir, 'molty.json'), JSON.stringify({ ...JSON.parse(readFileSync(join(CAT, 'molty.json'), 'utf8')), life: JSON.parse(`{${cat}}`) }));
  assert.deepEqual(check(cdir).errors, []);
});

test('the schema: what the checker enforces is what editors read', () => {
  const walk = (s, at) => {
    if (!s || typeof s !== 'object' || Array.isArray(s)) return;
    for (const k of Object.keys(s)) assert.ok(KEYWORDS.includes(k) || ANNOTATIONS.includes(k), `${at}: keyword ${k} is not one schema.mjs applies`);
    for (const k of ['properties', '$defs']) for (const [n, c] of Object.entries(s[k] ?? {})) walk(c, `${at}/${n}`);
    for (const k of ['items', 'additionalProperties', 'propertyNames']) walk(s[k], `${at}/${k}`);
    for (const k of ['anyOf', 'oneOf']) (s[k] ?? []).forEach((c, i) => walk(c, `${at}/${k}/${i}`));
  };
  walk(SCHEMA, 'molty-2.json');
  assert.deepEqual(validate(SCHEMA, base()), []);
  assert.deepEqual(validate(SCHEMA, JSON.parse(readFileSync(join(CAT, 'molty.json'), 'utf8'))), []);
});

test('check: a problem names molty.json, the line and the fix', () => {
  assert.deepEqual(checked().errors, []);
  assert.deepEqual(checked((d) => { d.colors.body = 'green'; }).errors, [{ at: 'molty.json:9', message: 'colors.body must be #rrggbb in lower case' }]);
  catches({
    'has no "colours" (did you mean colors?)': (d) => { d.colours = d.colors; },
    'needs moods': (d) => { delete d.moods; },
    'moods needs idle': (d) => { d.moods = { happy: 'smiles' }; },
    'must be molty/2': (d) => { d.molty = 'molty/3'; },
    'must be #rrggbb in lower case': (d) => { d.colors.body = 'green'; },
    'is one line about what it does, or { about, plays, chain, hold, lasts, habit }': (d) => { d.gestures.hop = 5; },
    'must be the name of a .glb or .gltf file in this folder, or the body written out': (d) => { d.body = 'pip.fbx'; },
    'pip.glb: does not exist': (d) => { d.body = 'pip.glb'; },
    'must be three numbers: [x, y, z]': (d) => { d.body.bones[1].at = [0, 1]; },
    'must be one of: blob, ellipsoid, capsule, cone, cylinder, patch': (d) => { d.body.shapes[0].shape = 'cube'; },
    'bone head: list it after its parent body': (d) => { d.body.bones.splice(1, 2, d.body.bones[2], d.body.bones[1]); },
    'a blob: there is no bone named belly (bones: root, body, head, tail)': (d) => { d.body.shapes[0].on = 'belly'; },
    'action hop: there is no bone named leg': (d) => { d.body.actions.hop.keys[0].leg = { turn: [0, 0, 5] }; },
    'gesture wave: there is no action named wave. Name an action wave, or say which one it plays': (d) => { d.gestures.wave = 'waves'; },
    'mood happy: there is no action named happy': (d) => { d.moods.happy = 'beams'; },
    'pose stand has no resting action: name one stand_idle': (d) => { d.poses = ['sit', 'stand']; },
    'wears the color pink, which colors does not have (colors: body, eye, iris, shine)': (d) => { d.body.shapes[1].color = 'pink'; },
    'is part of "belly", which Molty does not know how to draw. Outlined parts: torso, hindL': (d) => { d.body.shapes[0].outline = 'belly'; },
    'colors needs iris: Molty paints the eye with it': (d) => { delete d.colors.iris; d.body.shapes[2].color = d.body.shapes[4].color = 'eye'; },
    'colors needs body: Molty paints every outlined part with it': (d) => { d.colors.green = d.colors.body; delete d.colors.body; for (const s of d.body.shapes) if (s.color === 'body') s.color = 'green'; },
    'a capsule on tail needs to (its other end) and radius': (d) => { delete d.body.shapes[6].to; },
    'feet: there is no bone named paw': (d) => { d.feet = ['paw']; },
    'center: there is no bone named neck': (d) => { d.center = ['body', 'neck']; },
    'body.actions.hop.view must be front, side, three_quarter or degrees': (d) => { d.body.actions.hop.view = 'above'; },
    'look must be 1 to 240 plain ASCII characters': (d) => { d.look = `A round Molty ${String.fromCharCode(0x2014)} green`; },
  });
  catches({
    'nothing plays the action spin': (d) => { d.body.actions.spin = { keys: [{ time: 0, body: { turn: [0, 0, 0] } }, { time: 1, body: { turn: [0, 360, 0] } }] }; },
    'mood idle: its action idle loops, but ends far from the pose it starts in': (d) => { d.body.actions.idle.keys[2].head.turn = [0, 170, 0]; d.body.actions.idle.keys[2].tail.turn = [0, 170, 0]; },
  }, 'warnings');
});

test('conventions: a way between poses is found by name, or because one free action goes that way', () => {
  const two = (d) => {
    d.poses = ['sit', 'tall'];
    d.body.actions.tall_idle = { keys: [{ time: 0, body: { at: [0, 0.5, 0] } }, { time: 1, body: { at: [0, 0.5, 0] } }] };
    d.body.actions.rise = { keys: [{ time: 0, body: { at: [0, 0.2, 0] } }, { time: 0.5, body: { at: [0, 0.5, 0] } }] };
    d.body.actions.tall_to_sit = { keys: [{ time: 0, body: { at: [0, 0.5, 0] } }, { time: 0.5, body: { at: [0, 0.2, 0] } }] };
  };
  const r = checked(two), g = r.pkg.creature.rig.graph, c = r.pkg.creature.rig.clips;
  assert.deepEqual([r.errors, g.rest, g.edges], [[], { sit: 'idle', tall: 'tall_idle' }, { 'sit>tall': 'rise', 'tall>sit': 'tall_to_sit' }]);
  assert.deepEqual([c.rise.from, c.rise.to, c.tall_idle.from, c.tall_idle.loop, c.hop.to], ['sit', 'tall', 'tall', true, 'sit']);
  catches({ '2 actions go from sit to tall (rise, climb): name the one to use sit_to_tall': (d) => { two(d); d.body.actions.climb = d.body.actions.rise; } });
  assert.equal(checked((d) => { two(d); d.body.actions.climb = d.body.actions.rise; d.body.actions.sit_to_tall = d.body.actions.rise; }).pkg.creature.rig.graph.edges['sit>tall'], 'sit_to_tall');
  assert.equal(checked((d) => { two(d); d.body.actions.hop.to = 'tall'; }).pkg.creature.rig.clips.hop.to, 'tall', 'an action may say where it ends');
  assert.deepEqual(checked((d) => { two(d); d.body.actions.sit_idle = d.body.actions.idle; delete d.body.actions.idle; d.moods.idle = { about: 'breathes', plays: 'sit_idle' }; }).pkg.creature.rig.graph.rest.sit, 'sit_idle');
  const sp = checked((d) => { d.moods.surprised = 'jumps'; d.body.actions.surprised = d.body.actions.idle; d.body.actions.sit_surprised = d.body.actions.hop; }).pkg.creature.rig.graph;
  assert.deepEqual(sp.enter, { surprised: { sit: 'sit_surprised' } });
});

test('chains: a gesture strung from actions picked at random, checked to join', () => {
  const ok = checked((d) => { d.body.actions.hop2 = structuredClone(d.body.actions.hop); d.gestures.hop = { about: 'hops, now and then twice', chain: { then: ['hop', 'hop2'], rounds: [1, 3] } }; });
  assert.deepEqual(ok.errors, []);
  assert.deepEqual(ok.pkg.creature.rig.graph.chains.hop, { then: ['hop', 'hop2'], rounds: [1, 3] });
  assert.equal(ok.pkg.creature.rig.graph.gestures.hop, 'hop', 'the gesture names its first action');
  catches({
    'which are not actions': (d) => { d.gestures.hop = { about: 'x', chain: { then: ['nope'], rounds: [1, 1] } }; },
    'rounds from 1': (d) => { d.gestures.hop = { about: 'x', chain: { then: ['hop'], rounds: [0, 1] } }; },
    'must join': (d) => {
      d.body.actions.lean = { keys: [{ time: 0, body: { at: [0, 0.25, 0] } }, { time: 0.5, body: { at: [0.2, 0.25, 0], turn: [0, 0, 20] } }] };
      d.gestures.hop = { about: 'x', chain: { then: ['hop', 'lean'], rounds: [2, 2] } };
    },
  });
});

test('chains: after the first link, a pick is among those that start in the pose the last one ended in', () => {
  const two = (d) => {
    d.poses = ['sit', 'tall'];
    d.body.actions.tall_idle = { keys: [{ time: 0, body: { at: [0, 0.5, 0] } }, { time: 1, body: { at: [0, 0.5, 0] } }] };
    d.body.actions.sit_to_tall = { keys: [{ time: 0, body: { at: [0, 0.25, 0] } }, { time: 0.5, body: { at: [0, 0.5, 0] } }] };
    d.body.actions.tall_to_sit = { keys: [{ time: 0, body: { at: [0, 0.5, 0] } }, { time: 0.5, body: { at: [0, 0.25, 0] } }] };
    d.body.actions.reach = { keys: [{ time: 0, body: { at: [0, 0.5, 0] } }, { time: 0.3, body: { at: [0, 0.55, 0] } }, { time: 0.6, body: { at: [0, 0.5, 0] } }] };
    d.body.actions.hop2 = structuredClone(d.body.actions.hop);
    d.gestures.hop = { about: 'hops, stands up, reaches, sits again', habit: true, chain: { then: ['hop', 'hop2', 'sit_to_tall', 'reach', 'tall_to_sit'], rounds: [6, 10] } };
  };
  const r = checked(two);
  assert.deepEqual(r.errors, [], 'links of different poses need not join each other, only those that can follow');
  const rig = r.pkg.creature.rig, from = (n) => rig.clips[n].from, to = (n) => rig.clips[n].to;
  for (const seed of [1, 2, 3, 4, 5, 6]) {
    const p = clipPlayer(rig, { seed }), seen = [];
    p.act('hop');
    for (let f = 0; f < 24 * 30 && (seen.length < 2 || p.clip !== 'idle'); f++) { p.update(1 / 24); if (p.clip !== seen.at(-1)) seen.push(p.clip); }
    const links = seen.filter((n) => n !== 'idle');
    for (let i = 1; i < links.length; i++) assert.equal(from(links[i]), to(links[i - 1]), `${links[i]} starts where ${links[i - 1]} ends: ${links}`);
    assert.equal(seen.at(-1), 'idle', `home to its mood: ${seen}`);
  }
  catches({ 'must join': (d) => { two(d); d.body.actions.reach.keys[0].body.at = [0.2, 0.5, 0]; } });
});

test('habits: a gesture only its moments play is never told to agents; moments compile for the device', () => {
  const r = checked((d) => {
    d.body.actions.hop2 = structuredClone(d.body.actions.hop);
    d.gestures.groom = { about: 'washes a paw', plays: 'hop2', habit: true };
    d.life = { moments: [{ every: [40, 90], pick: { groom: 3, hop: 1 } }, { every: [300, 600], play: 'groom' }] };
  });
  assert.deepEqual(r.errors, []);
  const cr = r.pkg.creature;
  assert.deepEqual([Object.keys(cr.gestures), Object.keys(cr.about.gestures), cr.rig.graph.gestures.groom], [['hop'], ['hop'], 'hop2'], 'the rig plays it; agents never hear of it');
  assert.deepEqual(cr.life.moments, [{ every: [40, 90], pick: [['groom', 3], ['hop', 1]] }, { every: [300, 600], pick: [['groom', 1]] }]);
  assert.deepEqual(checkPackage(r.pkg, 'pip'), []);
});

test('packed clips: the same keys and numbers as the plain form, about half the room', () => {
  const r = check(CAT), nodes = r.body.nodes;
  let plain = 0, packed = 0;
  for (const a of r.body.actions) {
    const c = r.pkg.creature.rig.clips[a.name], b = bakeAction(nodes, a, 24, c.loop, !!r.life?.clips.has(a.name) && !Object.values(r.pkg.creature.rig.graph.gestures).includes(a.name) && !Object.values(r.pkg.creature.rig.graph.moods).includes(a.name) && !Object.values(r.pkg.creature.rig.graph.rest).includes(a.name));
    assert.equal(c.pk, packClip(b.ch, b.frames), `${a.name}: packed from its bake`);
    const x = decodeClip({ ...c, pk: undefined, ch: b.ch }, r.pkg.creature.rig.scale), y = decodeClip(c, r.pkg.creature.rig.scale);
    assert.equal(x.ch.length, y.ch.length);
    x.ch.forEach((h, i) => {
      const g = y.ch[i];
      assert.deepEqual([h.node, h.path, h.cubic, h.linear, [...h.keys]], [g.node, g.path, g.cubic, g.linear, [...g.keys]], `${a.name} channel ${i}`);
      assert.ok(h.out.every((v, k) => Object.is(v, g.out[k])), `${a.name} channel ${i}: the same numbers`);
    });
    plain += JSON.stringify(b.ch).length; packed += c.pk.length;
  }
  assert.ok(packed < plain * 0.62, `${packed} of ${plain} bytes`);
  const wild = new Int16Array(Array.from({ length: 36 }, (_, i) => (i * 7919) % 65536 - 32768));   // cubic: 3 x 4 a key, every step size
  const odd = [[3, 'r', 'C', 2, Buffer.from([0, 5, 9]).toString('base64'), Buffer.from(wild.buffer).toString('base64')],
    [7, 'r', 'S', 1, Buffer.from([0]).toString('base64'), Buffer.from(new Int8Array([127, -127, 0, 5]).buffer).toString('base64')]];
  const back = unpackClip(packClip(odd, 10));
  assert.deepEqual(back.map((h) => [h.node, h.path, h.interp, h.b8, [...h.keys], [...h.vals]]),
    [[3, 'r', 'C', false, [0, 5, 9], [...wild]], [7, 'r', 'S', true, [0], [127, -127, 0, 5]]], 'cubic, int8, extremes');
  assert.throws(() => unpackClip(packClip(odd, 10).slice(0, 8)), /ends early/);
});

test('chains: the cat plays a different game each seed, the same game for the same seed, no link twice running', () => {
  const rig = check(CAT).pkg.creature.rig, ch = rig.graph.chains.yarn;
  const game = (seed) => {
    const p = clipPlayer(rig, { seed }), seen = [];
    p.act('yarn');
    for (let f = 0; f < 30 * 40 && (seen.length === 0 || p.clip.startsWith('yarn_') || f < 60); f++) {
      p.update(1 / 30);
      if (p.clip.startsWith('yarn_') && p.clip !== seen.at(-1)) seen.push(p.clip);
    }
    return seen;
  };
  const games = [1, 2, 3, 4, 5, 6, 7, 8].map((s) => game(s * 7919));
  for (const g of games) {
    assert.ok(ch.first.includes(g[0]) && g.at(-1) === ch.last[0], `starts with first, ends with last: ${g}`);
    const mid = g.slice(1, -1);
    assert.ok(mid.length >= ch.rounds[0] && mid.length <= ch.rounds[1], `rounds: ${g}`);
    assert.ok(mid.every((c, i) => i === 0 || c !== mid[i - 1]), `no link twice running: ${g}`);
  }
  assert.ok(new Set(games.map((g) => g.join())).size >= 3, 'different seeds, different games');
  assert.deepEqual(game(7919), games[0], 'the same seed, the same game');
});

test('props: a bowl or a ball is never dragged by a crossfade, and is gone once the cat is back in its mood', () => {
  const rig = check(CAT).pkg.creature.rig, props = rig.nodes.map((n, i) => (i > 0 && n.parent < 0 ? i : -1)).filter((i) => i >= 0);
  let worst = 0, after = 0;
  for (const g of ['eat', 'yarn']) for (const seed of [3, 5, 8, 13]) {
    const p = clipPlayer(rig, { seed }), prev = new Map();
    for (let f = 0; f < 30; f++) p.update(1 / 30);
    p.act(g);
    let back = -1;
    for (let f = 0; f < 30 * 40 && (back < 0 || f < back + 45); f++) {
      p.update(1 / 30);
      for (const i of props) {
        const w = p.world, m = i * 12, now = [w[m + 3], w[m + 7], w[m + 11], Math.hypot(w[m], w[m + 4], w[m + 8])], was = prev.get(i);
        if (was && now[3] > 0.05 && was[3] > 0.05) worst = Math.max(worst, Math.hypot(now[0] - was[0], now[1] - was[1], now[2] - was[2]));
        if (back >= 0 && f > back + 15 && now[3] > 1e-4) after++;
        prev.set(i, now);
      }
      if (back < 0 && !p.clip.startsWith(`${g}_`) && f > 30) back = f;
    }
  }
  assert.ok(worst < 0.25, `a showing prop jumps ${worst.toFixed(3)} in a frame`);
  assert.equal(after, 0, 'no prop shows once the cat is back in its mood');
});

test('limits: 80 bones, 128 shapes, 96 actions, 8 poses, 32 gestures, names of 24 letters, 256 KB', () => {
  const lacks = (r, want) => assert.ok(r.errors.some((e) => e.message.includes(want)), `${want}: got ${JSON.stringify(r.errors).slice(0, 600)}`);
  const bones = (n) => (d) => { for (let i = d.body.bones.length; i < n; i++) d.body.bones.push({ name: `b${i}`, parent: 'body' }); };
  const shapes = (n) => (d) => { for (let i = d.body.shapes.length; i < n; i++) d.body.shapes.push({ ...d.body.shapes[0], at: [0, i / 1000, 0] }); };
  const actions = (n) => (d) => { for (let i = Object.keys(d.body.actions).length; i < n; i++) { d.body.actions[`a${i}`] = d.body.actions.hop; d.gestures[`a${i}`] = 'hops again'; } };
  assert.deepEqual(checked(bones(LIMITS.nodes)).errors, []);
  lacks(checked(bones(LIMITS.nodes + 1)), 'body.bones holds at most 80 (it has 81)');
  assert.deepEqual(checked(shapes(LIMITS.shapes)).errors, []);
  lacks(checked(shapes(LIMITS.shapes + 1)), 'body.shapes holds at most 128 (it has 129)');
  assert.deepEqual(checked(actions(33)).errors, [], '32 gestures and idle');
  lacks(checked(actions(34)), 'gestures holds at most 32 (it has 33)');
  lacks(checked((d) => { for (let i = 0; i < 95; i++) d.body.actions[`a${i}`] = d.body.actions.hop; }), 'body.actions holds at most 96 (it has 97)');
  lacks(checked((d) => { d.poses = 'abcdefghi'.split(''); }), 'poses holds at most 8 (it has 9)');
  lacks(checked((d) => { d.body.bones[3].name = 'a'.repeat(25); }), 'must be 1 to 24 of letters, digits, _ . and -');
  // the size: many long actions on many bones
  const heavy = (n) => (d) => { bones(60)(d); for (let i = 0; i < n; i++) { d.body.actions[`a${i}`] = { keys: Array.from({ length: 240 }, (_, k) => ({ time: k / 24, ...Object.fromEntries(d.body.bones.slice(4).map((b, j) => [b.name, { turn: [Math.sin(k * 0.7 + j + i) * 40, Math.cos(k * 1.3 + j) * 40, 0] }])) })) }; d.gestures[`a${i}`] = 'wriggles'; } };
  assert.deepEqual(checked(heavy(1)).errors, []);
  lacks(checked(heavy(12)), 'KB: the device takes 256.0 KB. Its largest actions: a');
});

// ---- life (docs/CREATURE.md, "Life")
const breathing = (d) => { d.life = { signals: { breath: { wave: { _: 0.3, sleepy: 0.15 } } }, layers: [{ layer: 'breathing', turn: { bones: 'body', squash: ['breath', 0.04] } }] }; };

test('life: one breathing layer compiles to a signal the device runs and a turn of the body', () => {
  const r = checked(breathing);
  assert.deepEqual([r.errors, r.warnings], [[], []]);
  const life = r.pkg.creature.life;
  assert.deepEqual(life.layers, [{ by: 'breath', nodes: [1], sq: 0.04 }]);
  assert.deepEqual(life.ch.map((c) => c[0]), ['_breath.ph', 'breath'], 'its phase, then the wave');
  assert.deepEqual(life.ch[0][1], [[{ _: 0.3, sleepy: -0.15 }]], 'the rate a mood table: 0.3, less 0.15 as sleepy weighs in');
  assert.equal(r.life.steps, 1);
  assert.deepEqual(checkPackage(r.pkg, 'pip'), []);
  assert.equal(checked().pkg.creature.life, undefined, 'no life block, no life');
});

test('life: a layer\'s clip keeps only what moves, and counts as played', () => {
  const r = checked((d) => {
    breathing(d);
    d.body.actions.nod_pose = { keys: [{ time: 0, head: { turn: [0, 0, 0] }, tail: { turn: [0, 0, 0] } }, { time: 1, head: { turn: [20, 0, 0] }, tail: { turn: [0, 0, 0] } }] };
    d.life.layers.push({ layer: 'nodding', clip: 'nod_pose', by: 'breath', zero: 0.5, weight: 'free' });
  });
  assert.deepEqual([r.errors, r.warnings], [[], []], 'no "nothing plays the action nod_pose"');
  assert.deepEqual(unpackClip(r.pkg.creature.rig.clips.nod_pose.pk).map((c) => c.node), [2], 'the head only: the held tail is left out');
  assert.deepEqual(r.pkg.creature.life.layers[1], { by: 'breath', w: '_lw.nodding', clip: 'nod_pose', zero: 0.5 });
  assert.deepEqual(r.pkg.creature.life.ch.at(-1), ['_lw.nodding', [[1, '@free']], { sp: [36, 12], keep: 1 }], 'its weight eased');
});

test('life: signals come after what they read; hold and every are events', () => {
  const r = checked((d) => {
    d.life = { signals: {
      rate: { from: 'calm', map: [[0, 0.2], [1, 0.4]] },
      calm: { sum: [['energy', 1], ['tired', -0.3]] },
      energy: { mood: { _: 0.5, sleepy: 0.1, working: 0.8 } },
      tired: { from: 'idle_time', map: [[60, 0], [300, 1]] },
      blink: { every: { _: [3, 7], sleepy: [8, 14] }, double: 0.1 },
      look: { hold: [0.5, 3], range: [-1, 1], move: 0.12, waits: 'blink' },
      head: { follow: 'look', lag: 0.25, bounce: 0.2 },
    }, layers: [{ layer: 'lean', turn: { bones: 'head', y: ['head', 15], z: ['look', 5] } }] };
  });
  assert.deepEqual(r.errors, []);
  const life = r.pkg.creature.life, order = life.ch.map((c) => c[0]);
  assert.ok(order.indexOf('energy') < order.indexOf('calm') && order.indexOf('tired') < order.indexOf('calm') && order.indexOf('calm') < order.indexOf('rate'), order.join(' '));
  assert.equal(order[0], '_working', 'working eased, first');
  assert.deepEqual(Object.keys(life.events), ['blink', 'look']);
  assert.deepEqual(life.events.look.stop.length, 2, 'look waits: never as a blink is about to start or under way');
  assert.deepEqual(life.layers.map((l) => [l.by, l.rot]), [['head', [0, 15, 0]], ['look', [0, 0, 5]]], 'one layer for each signal a turn reads');
  assert.deepEqual(life.ch.find((c) => c[0] === 'rate')[1], [[0.2], [0.2, ['clamp', '@calm', 0, 1]]], 'a map, as straight lines');
  assert.deepEqual(life.ch.find((c) => c[0] === 'tired')[1], [[-0.25], [1 / 240, ['clamp', '@idle_time', 60, 300]]], 'nothing before 60 s, all of it from 300 s');
});

test('life: check says what is wrong in plain words', () => {
  const sig = (s, more = {}) => (d) => { breathing(d); Object.assign(d.life.signals, s); Object.assign(d.life, more); };
  catches({
    'signal breath reads "enrgy", which is not a signal': (d) => { breathing(d); d.life.signals.breath = { wave: 'enrgy' }; },
    'signal breath reads "voice_out", which Molty\'s firmware does not give yet': (d) => { breathing(d); d.life.signals.breath = { wave: 'voice_out' }; },
    'signals read each other in a loop: a -> b -> a': sig({ a: { from: 'b' }, b: { sum: [['a', 1]] } }),
    'signal spin: say what kind it is, one of mood, from, sum, wave, noise, hold, every, follow': sig({ spin: { twirl: 1 } }),
    'signal jaw: a pulse on a trigger (on) is not in Molty\'s firmware yet': sig({ jaw: { on: 'touch' } }),
    'signal free: free is a name the device uses': sig({ free: { mood: { _: 1 } } }),
    'signal e: mood names grumpy, which is not a mood': sig({ e: { mood: { _: 1, grumpy: 0 } } }),
    'signal b: every must be [least, most], or a mood table of them': sig({ b: { every: 3 } }),
    'signal w: a wave signal does not take lag': sig({ w: { wave: 0.3, lag: 1 } }),
    'layer blink: there is no action named blink_pose (actions: idle, hop)': (d) => { breathing(d); d.life.layers.push({ layer: 'blink', clip: 'blink_pose', by: 'breath' }); },
    'layer breathing: there is no bone named belly (bones: root, body, head, tail)': (d) => { breathing(d); d.life.layers[0].turn.bones = 'belly'; },
    'layer breathing: by "breth" is not a signal': (d) => { breathing(d); d.life.layers[0] = { layer: 'breathing', clip: 'hop', by: 'breth' }; },
    'layer breathing: ripple is not in Molty\'s firmware yet': (d) => { breathing(d); d.life.layers[0].turn.ripple = 0.06; },
    'layer mouth: mode replace is not in Molty\'s firmware yet': (d) => { breathing(d); d.life.layers.push({ layer: 'mouth', clip: 'hop', by: 'breath', mode: 'replace' }); },
    'life.moments[0] has no "on": it takes every, play, pick': (d) => { breathing(d); d.life.moments = [{ on: 'breath', every: [20, 40], play: 'hop' }]; },
    'life.moments[0]: skip is not a gesture (gestures: hop)': (d) => { d.life = { moments: [{ every: [20, 40], pick: { hop: 1, skip: 2 } }] }; },
    'life.moments[0]: every must be [least, most] seconds, from 5 to 3600, the least first': (d) => { d.life = { moments: [{ every: [40, 20], play: 'hop' }] }; },
    'life.moments[0]: say what it plays: play (a gesture) or pick': (d) => { d.life = { moments: [{ every: [20, 40] }] }; },
  });
});

test('life: the device\'s limits: 64 signals, 16 layers, 600 steps a frame', () => {
  const many = (n) => (d) => { breathing(d); for (let i = 0; i < n; i++) d.life.signals[`s${i}`] = { sum: [['breath', 1], ['breath', 1], ['breath', 1], ['breath', 1], ['breath', 1], ['breath', 1], ['breath', 1], ['breath', 1], ['breath', 1], ['breath', 1]] }; };
  assert.deepEqual(checked(many(59)).errors, [], '60 signals, 591 steps');
  assert.ok(checked(many(60)).errors.some((e) => e.message.includes('life takes 601 steps a frame: the device gives it 600')));
  assert.ok(checked(many(64)).errors.some((e) => e.message.includes('life.signals holds at most 64 (it has 65)') || e.message.includes('a Molty holds 64')));
  const layers = (n) => (d) => { breathing(d); for (let i = 1; i < n; i++) d.life.layers.push({ layer: `l${i}`, turn: { bones: 'head', x: ['breath', 1] } }); };
  assert.deepEqual(checked(layers(16)).errors, []);
  assert.ok(checked(layers(17)).errors.some((e) => e.message.includes('17')), 'a 17th layer');
});

test('the rounding is the first exporter\'s: six digits and halves to even, as Python does them', () => {
  // float(f'{v:.6g}') in Python, for each
  const cases = [[1.953125, 1.95312], [0.4296875, 0.429688], [2.9000000953674316, 2.9], [-1.953125, -1.95312], [1.953135, 1.95314], [0.9999995, 1], [123456.5, 123456], [0.000123456789, 0.000123457], [0, 0], [-0.36, -0.36]];
  for (const [v, want] of cases) assert.equal(r6(v), want, String(v));
});

test('preview: a local page that plays the built Molty with the device\'s own player', async () => {
  const server = preview(PIP, 0);
  await new Promise((ok) => server.once('listening', ok));
  const at = `http://127.0.0.1:${server.address().port}`;
  try {
    assert.equal(server.address().address, '127.0.0.1');
    assert.match(await (await fetch(at + '/')).text(), /molty-creature preview/);
    const m = await (await fetch(at + '/molty')).json();
    assert.deepEqual([m.problems, m.name, Object.keys(m.rig.clips), Object.keys(m.gestures)], [[], 'Pip', ['idle', 'hop'], ['hop']]);
    assert.match(await (await fetch(at + '/js/clip-player.js')).text(), /export function clipPlayer/);
    assert.equal((await fetch(at + '/js/../../../Makefile')).status, 404);
  } finally { server.close(); }
});

test('the Molty tools never run a 3D tool', () => {
  const src = execFileSync('git', ['grep', '--untracked', '-il', 'blender', '--', 'packages/tools/creature', 'packages/tools/molty-creature.mjs'], { cwd: ROOT, encoding: 'utf8' }).trim().split('\n').filter(Boolean);
  assert.deepEqual(src, ['packages/tools/creature/creature.test.mjs'], 'only this test names Blender (to prove it is not needed, and to check our exporter)');
});
