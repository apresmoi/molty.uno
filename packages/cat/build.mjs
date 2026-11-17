// The cat as a creature package with a rig (docs/CREATURES.md section 12): modelled, rigged and animated in Blender
// (packages/cat/blender/: cat_model.py builds cat.blend, cat_clips.py authors every clip and transition, export_molty.py bakes
// them to blender/cat-export.json). This wraps that export into packages/cat/cat.json and its packages/index.json entry.
//   blender -b packages/cat/blender/cat.blend -P packages/cat/blender/export_molty.py   (after a change in Blender)
//   node packages/cat/build.mjs
import { readFileSync } from 'node:fs';
import { writePackage } from '../../tools/creatures/kit.mjs';

const ID = 'cat';
const ex = JSON.parse(readFileSync(new URL('./blender/cat-export.json', import.meta.url), 'utf8'));

const about = {
  look: 'A slim lavender cat drawn as a fine outline: pointy ears pink inside, round green eyes, a pink nose, whiskers, a white chest. A long darker tail. It sits, stands, lies in a loaf and walks.',
  gestures: {
    wiggle: 'crouches low and shimmies its hips as if about to pounce, the tail tip twitching',
    nod: 'a slow cat nod: the eyes close, the head sinks and comes back up',
    shake_head: 'rattles its head quickly, the ears flapping, then one last ear flick',
    bounce: 'crouches, wiggles its bum and pounces forward, landing in a play crouch',
    peek: 'goes low with only its head up, ears pricked, the eyes sweeping slowly',
    glance_left: 'its ears turn to its left first, then the eyes and the head follow',
    glance_right: 'its ears turn to its right first, then the eyes and the head follow',
    blink: 'a blink with a little ear flick',
    wobble: 'a whole-body shake from the head down to the tail, the fur puffing out',
    stretch: 'front paws walk out and the chest sinks low in a long stretch, then it sits back up',
    look_up: 'tips its head right back at a bird overhead, the jaw chattering',
    yawn: 'a big yawn, the jaw wide with the tongue curled, ears flat, then a lick',
    inspect: 'goes low and peers, the eyes sweeping slowly',
    paddle: 'bats at something with one paw, the other paw joining once',
    eat: 'a red bowl of milk on the floor; it leans down and laps, ears and tail moving, then licks its lips',
    yarn: 'a ball of yarn rolls in; it watches, crouches, wiggles and pounces, then bats it away',
    shaken: 'jostled by the shake and dizzy a moment, it shakes it off like a wet cat and looks at you, ready',
    walk: 'gets up and walks a few steps across the floor, then settles again',
  },
  moods: {
    idle: 'sits with its chest breathing, ears swiveling, tail tip curling, slow blinks',
    listening: 'both ears prick toward you and it goes still, only the tail tip twitching',
    thinking: 'eyes half shut, an ear back, the tail sweeping slowly; now and then a tilt',
    speaking: 'the chin lifts with each meow and the ears move a little',
    happy: 'slow blinks, the tail up with a quivering tip, kneading with its front paws',
    sleepy: 'lies down into a loaf, paws tucked, the tail along its side, eyes closed, breathing slowly',
    surprised: 'jumps up with the back arched high, fur standing up, the tail a bottle brush, ears flat',
  },
};

// what an agent's mood and gesture names play (the shared gestures keep their names; inspect is the peek)
const SHARED = ['wiggle', 'nod', 'shake_head', 'bounce', 'peek', 'glance_left', 'glance_right', 'blink', 'wobble', 'stretch', 'look_up', 'yawn', 'inspect', 'paddle'];
const OWN = { eat: 'eat', yarn: 'yarn', shaken: 'shaken', walk: 'walk' };
const gestureClip = { ...Object.fromEntries(SHARED.map((g) => [g, g === 'inspect' ? 'peek' : g])), ...OWN };
const MOODS = ['idle', 'listening', 'thinking', 'speaking', 'happy', 'sleepy', 'surprised'];
const dur = (clip) => Math.min(10, Math.max(0.2, Math.round(ex.clips[clip].dur * 10) / 10));

// the route map: an ordered pair of families goes through its authored transition (`<a>_to_<b>`, or the sitting cat's own:
// walk_start, walk_stop, lie_down, get_up); never through a gesture that happens to end in another family
const own = { 'sit>walk': 'walk_start', 'walk>sit': 'walk_stop', 'sit>loaf': 'lie_down', 'loaf>sit': 'get_up' };
const edges = {};
for (const a of ex.graph.families) for (const b of ex.graph.families) {
  const c = own[`${a}>${b}`] ?? `${a}_to_${b}`;
  if (a !== b && ex.clips[c]) edges[`${a}>${b}`] = c;
}
// each channel as a compact array (the same glTF channel + sampler): [node, path, interpolation S|L|C (STEP, LINEAR,
// CUBICSPLINE), bytes per output component 1|2 (int8 normalised | int16), input, output]
const clips = Object.fromEntries(Object.entries(ex.clips).map(([n, c]) => [n, { ...c,
  ch: c.ch.map((h) => [h.node, h.path, h.interp[0], h.ct === 'i8' ? 1 : 2, h.in, h.out]) }]));
const rig = {
  ...ex,
  clips,
  graph: {
    ...ex.graph,
    edges,
    moods: Object.fromEntries(MOODS.map((m) => [m, m])),
    gestures: gestureClip,
    hold: { walk: 6 },                                           // the walk gesture walks for 6 s, then settles
    enter: { surprised: { sit: 'sit_surprised', stand: 'stand_surprised' } },   // the startle is its own way in
    sync: { walk: 1 },                                           // the walk is left and joined on a stride (1 s)
  },
};
for (const [g, c] of Object.entries(gestureClip)) if (!ex.clips[c]) throw new Error(`gesture ${g}: no clip ${c}`);

const pkg = { v: 'molty-pkg/1', id: ID, name: 'Cat', version: '2.0.0', kind: 'creature',
  creature: {
    about,
    // the channel layers stay empty: everything the cat does is in its rig's clips
    body: { palette: ex.palette, bones: [], parts: [] },
    gestures: { ...Object.fromEntries(SHARED.map((g) => [g, { ch: [] }])), ...Object.fromEntries(Object.keys(OWN).map((g) => [g, { dur: g === 'walk' ? 8 : dur(OWN[g]), ch: [] }])) },
    moods: Object.fromEntries(MOODS.map((m) => [m, { ch: [] }])),
    rig,
  } };
const out = writePackage(new URL(`./${ID}.json`, import.meta.url).pathname, pkg);
console.log(ID, out.size, 'bytes', out.sha256);
