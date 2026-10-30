# Cute kit

A creature is a data spec plus optional small hooks. The kit builds rigid low-poly
parts (flat Lambert, one colour each) on a small rig and animates them with a few
matrices per frame. Every mood, poke, autopilot state, the device preview, compare
mode and the customise panel work with no extra code.

## Add a creature

Add a `defineCreature({...})` call to one of the pack files (`pack-a.js` … `pack-e.js`),
or create a new pack file and import it from `index.js`. Then export it to a `.molty`
file with `node device/creatures/tools/molty-export.mjs <id>` (see `../../MOLTY-FORMAT.md`).

```js
// js/cute/pack-a.js
import { defineCreature } from './kit.js';

defineCreature({
  id: 'cute-frog', name: 'Frog', archetype: 'Amphibian',     // id must be unique across all packs
  palette: { body: '#9bd77a', accent: '#5fa34a', belly: '#f2f7d8' },
  body: { size: [0.3, 0.22, 0.3] },                              // optional; at defaults to [0, ry, 0]
  head: { size: [0.34, 0.28, 0.3], at: [0, 0.6, 0.04] },         // required; head ≥ body
  eyes: { x: 0.45, y: 0.1, r: 0.085 },
  mouth: { type: 'cat', y: -0.35 },
  motion: 'hop',
  parts: [
    { type: 'sphere', at: 'top', size: 0.08, pos: [0.15, -0.02, 0.05], mirror: true },
    { type: 'ellipsoid', at: 'foot', size: [0.08, 0.03, 0.1], color: 'accent' },
  ],
});
```

To check your work, serve `device/creatures/` (see `../../README.md`), click the creature's card,
try all seven moods, turn on the device preview and read the tri counter in the stage
header. The kit logs a console warning when a creature goes over 1000 tris.

## Spec fields

| field | meaning |
|---|---|
| `id`, `name` | required. `id` should start with `cute-`. |
| `archetype`, `roster`, `source` | card text |
| `palette` | colour keys → hex. Defaults: `body accent belly eye shine cheek mouth tongue white dark`. Add your own keys. |
| `head` | **required.** `{ size:[rx,ry,rz], at:[x,y,z], type, color, seg, pivot, squash, pitch }`. `pivot` is the neck point (defaults to the top of the body, or the floor when there is no body). `squash` is the share of body squash the head takes (0.5 with a body, 1 without). `pitch` scales the mood's head nod (default 0.65). |
| `body` | optional. `{ size, at, type, color, seg }`. Leave it out for a blob that is all head (see `mochi2.js`). |
| `eyes` | `{ x, y, r, shape:'round'\|'tall'\|'wide', tilt, shine, color }`. `x` and `y` are normalised head coordinates (−1…1): keep eyes low and wide, about `x` 0.35–0.45 and `y` −0.1–0.1. `r` is in world units (0.07–0.09). |
| `mouth` | `{ type:'smile'\|'cat'\|'jaw'\|'none', y, w, open }`. `jaw` creates a `jaw` socket that pitches by `pose.mouth × open` (beaks and bills: upper half `at:'head'`, lower half `at:'jaw'`). |
| `cheeks` | `{ x, y, r }`, or `false` to turn them off |
| `motion` | `'waddle'\|'hop'\|'slither'\|'float'\|'flutter'\|'wiggle'\|'none'`, or `{ type, amp, rate, height }` (`height` applies to `float` and `flutter`) |
| `limbs` | amplitude multipliers `{ side, ear, tail, foot, antenna }`. Set one to 0 to freeze that limb. |
| `sockets` | overrides: `{ ear:[x,y,z] }` or `{ ear:{ pos, rot, on:'head'\|'body'\|'root' } }`. Positions are in creature space, rest pose, for the +x side. |
| `items` | default customise slots `{ head, neck }` (`HEAD_ITEMS` / `NECK_ITEMS`) |
| `parts` | list of parts (see below) |
| `extras` | free-form data passed to your hooks |
| `build(ctx)` | optional hook, called once after the build |
| `update(ctx)` | optional hook, called every frame after the kit has animated |

The creature is authored about 1 unit tall with feet at y = 0. It is normalised to height 1
using the spec parts (items are left out). It faces +z, and x+ is screen-right.

## Parts

`{ name, type, at, pos, rot, scale, color, seg, mirror, visible, rotOrder }`

- **Types:** `ellipsoid`/`sphere` (`size`), `teardrop` (`size`, `pinch`: tip at +y; use it for tufts, ears, wings, tails, leaves and flames), `capsule` (`r`, `len`), `cone` (`r`, `h`), `cylinder` (`r` \| `rTop`/`rBottom`, `h`), `torus` (`R`, `tube`, `arc`), `box` (`size`), `disc` (`r`, faces +z), `custom` (`geometry: (THREE) => BufferGeometry`).
- **`color`** is a palette key or a `#hex`. Use palette keys for anything that should follow the customise body/accent pickers.
- **`seg`** is `[widthSegs, heightSegs]` for round parts. The automatic choice is 5–10 × 3–7, scaled to size. Ellipsoid tris = 2·w·(h−1).
- **Sockets** (`at`):
  - On the root: `root`, `foot`. Feet stay planted while the body squashes.
  - On the body: `body`, `side` (aliases `wing`, `arm`, `fin`), `tail`, `neck`, `back`.
  - On the head: `head`, `top`, `ear`, `antenna`, `horn`, `jaw`.
  - `pos` is relative to the socket. For `body` and `root` that origin is the floor centre, so `pos` is in creature space. For `head` it is the head centre. For every other socket it is the pivot, which is where rotations swing from.
- **Paired sockets** (`ear`, `side`, `foot`, `antenna`, `horn`) mirror the part automatically: write the +x copy and the kit makes the L copy. On other sockets, `mirror: true` duplicates the part with x flipped.

## What the kit animates for you

- **Eyes**, the main mood channel:
  - normal: big glossy eyes with 2 white highlight dots, lids close top-down
  - listening: 1.1× wide
  - surprised: white sclera ring with a small pupil (O O)
  - happy: ^ ^ arcs
  - sleepy: flat-top half-lids, then ‿ ‿ when shut
  - thinking: gaze slides up and to the side
  - the kit adds extra blinks every 1.8–4 s on top of the engine's
- **Mouth:** a closed smile or ω; a D-shaped open smile when happy; an ellipse that opens with the speech envelope; an "o" when surprised; or a `jaw` that opens.
- **Cheeks:** grow when happy or surprised.
- **Body:** squash and stretch (×1.25 the engine's value), lean, hops (happy hops 35% higher), a curious head tilt every 3.5–7 s, and the chosen `motion` as an in-place idle that is active when awake, bigger when happy and still when asleep.
- **Limbs:**
  - `side`: flap when happy, raise when surprised, gesture when speaking, droop when sleepy
  - `ear`: perk when listening, droop when sleepy
  - `ear`, `antenna`, sprout: spring follow-through
  - `tail`: wags (faster when happy) plus an occasional flick
  - `foot`: steps with the waddle and hop
- **Moods** come from `pose.mood`, smoothed into weights `ctx.w.{idle,listening,thinking,speaking,happy,sleepy,surprised}` (0…1).

## Hooks (`ctx`)

`ctx.t, ctx.dt, ctx.pose, ctx.w, ctx.L` (this frame's locomotion layer), `ctx.lag` (spring
lag `{x,z}`), `ctx.part(name) → Mesh[]`, `ctx.socket(name) → Group[]`, `ctx.addPart(spec)`
(only valid in `build`), `ctx.mat(key)`, `ctx.face.{eyes,mouth,cheeks}`, `ctx.extras`, `ctx.THREE`.
Every part keeps `userData.basePos/baseRot/baseScale`, so `update` can write an absolute pose
each frame without drift. For example, a glowing tip that pulses when speaking:

```js
update(ctx) { for (const m of ctx.part('glow')) m.scale.setScalar(1 + 0.4 * ctx.pose.mouth); }
```

## Tri budget

- **Target:** about 600 visible tris per creature (the device budget in `stage.js`).
- **Hard ceiling:** 1000, enforced by a console warning.
- **Fixed cost of the face:** about 75–100 tris.
- **Head and body:** 10×7 ≈ 120 tris each.
- **Small parts:** 30–50 tris each.
- **Where to cut first:** the head gives the most visible faceting, so spend tris there and lower `seg` on everything else.

Reference counts: Duck ≈ 594–626, Mochi 2 ≈ 450–490. They vary with the eye and mouth state.
