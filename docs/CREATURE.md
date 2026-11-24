# The Molty format

A Molty is one folder:

| in the folder | what it is |
|---|---|
| `molty.json` | who it is: its name, its colors, what it does in each mood |
| a body file (only for a big one) | its skeleton, its shapes and its actions, saved from any 3D tool as `.glb` |

A small Molty needs no body file and no 3D tool: its body goes in `molty.json` too.

## Your first Molty

Make a folder, say `blip/`, and put this in `blip/molty.json`:

```json
{
  "molty": "molty/2",
  "id": "blip",
  "name": "Blip",
  "version": "1.0.0",
  "about": "A round little Molty that breathes.",
  "look": "A green ball with two pale eyes.",
  "colors": { "body": "#8fd4a0", "iris": "#f4f0d0", "shine": "#ffffff" },
  "moods": { "idle": "breathes slowly" },
  "body": {
    "bones": [{ "name": "root" }, { "name": "body", "parent": "root", "at": [0, 0.25, 0] }],
    "shapes": [
      { "shape": "blob", "on": "body", "radii": [0.25, 0.25, 0.25], "color": "body", "outline": "torso" },
      { "shape": "ellipsoid", "on": "body", "at": [0.08, 0.08, 0.22], "radii": [0.04, 0.04, 0.006], "color": "iris", "outline": "eye" },
      { "shape": "ellipsoid", "on": "body", "at": [-0.08, 0.08, 0.22], "radii": [0.04, 0.04, 0.006], "color": "iris", "outline": "eye" }
    ],
    "actions": {
      "idle": { "keys": [
        { "time": 0, "body": { "scale": [1, 1, 1] } },
        { "time": 1.5, "body": { "scale": [1.05, 0.96, 1.05] } },
        { "time": 3, "body": { "scale": [1, 1, 1] } }
      ] }
    }
  }
}
```

Check it and build it (one command; it names the file and the fix when something is wrong):

```sh
node packages/tools/molty-creature.mjs build blip
```

Watch it move: `node packages/tools/molty-creature.mjs preview blip`, then open the address it prints. The build
writes one small file: the Molty a device installs.

## Space and units

- Up is `y`. The Molty faces `+z` and its left is `+x`. The floor is `y = 0`.
- Sizes are the body's own: a cat stands about 0.6 high, and the screen shows about 1.75 across.
- A turn is degrees about `x`, `y` and `z`: `[10, 0, 0]` nods, `[0, 20, 0]` looks to its left, `[0, 0, 7]` tilts.
- A time is seconds from the start of the action.

## Bones and shapes

A bone has a `name`, the `parent` it hangs from and where it starts (`at`, from its parent's start); list it after
its parent. A shape sits `on` a bone and moves with it.

| shape | takes | is |
|---|---|---|
| `blob` | `at`, `radii`, `turn` | a soft ball that melts into the blobs next to it: a body |
| `ellipsoid` | `at`, `radii`, `turn` | a ball with three radii: a head, a paw, a flat disc for an eye |
| `capsule` | `at`, `to`, `radius` (one, or one for each end) | a rounded stick: a leg, a tail |
| `cylinder` | `at`, `to`, `radius` | the same with flat ends: a bowl |
| `cone` | `at`, `to` (its tip), `radius`, `flat` | a flattened cone: an ear |
| `patch` | `at`, `radii`, `turn` | a region of color on the body: a white chest |

Every shape names a `color` (one of `colors`) and an `outline`: the part it belongs to. Shapes of one part are
drawn as one outlined form. A Molty has these parts, outlined: `torso` `head` `tail` `legF.L` `legF.R` `hindL`
`hindR` `bowl` `yarn`; and these features: `eye` `nose` `maw` `tongue` `earIn` `whisker` `chest` `milk` `yarnline`.
Outlined parts are painted from the color `body`, eyes from `iris` and `shine`; `check` says which colors you need.

## Actions

An action is a list of keys: at a `time`, where some bones are (`at`), how they are turned (`turn`) and how big
(`scale`). Between two keys a bone moves in a straight line; a bone you do not mention stays as it is.

```json
"hop": { "keys": [
  { "time": 0,    "body": { "at": [0, 0.25, 0] } },
  { "time": 0.4,  "body": { "at": [0, 0.55, 0], "scale": [0.92, 1.1, 0.92] } },
  { "time": 0.8,  "body": { "at": [0, 0.25, 0], "scale": [1, 1, 1] } }
] }
```

An action may also say `"view"` (`front`, `side` or `three_quarter`), `"follow": true` (the view goes along, for a
walk) and `"stride"` (a walk's seconds a step).

## Names do the wiring

You do not connect actions to anything. Name them, and they are found.

| name an action | and it is |
|---|---|
| `idle`, `listening`, `thinking`, `speaking`, `happy`, `sleepy`, `surprised` | what the Molty does in that mood, in a loop |
| like a gesture (`nod`, `yawn`, or one of your own) | what it does once when asked for that gesture |
| `<pose>_idle` | how it rests in a pose you listed in `"poses"`: `stand_idle` |
| `<a>_to_<b>` | the way from one pose to another: `sit_to_stand` |
| `<pose>_<mood>` | its own way into a mood from a pose: `sit_surprised` |

In `molty.json`, `moods` and `gestures` give each one a line of words: what it looks like, for the agent that speaks
through the Molty. To play an action under another name: `"inspect": { "about": "peers low", "plays": "peek" }`.
A looping action ends on the pose it starts with. Which pose an action starts and ends in is worked out from how
it looks, so the way between two poses is found even without the `_to_` name.

### A gesture that is different every time

A gesture can be a chain of short actions that the Molty strings together itself, picking at random, so two plays are
never the same. The cat's yarn:

```json
"yarn": { "about": "a ball of yarn bounces in; it swats it about and turns after it ...",
  "chain": { "first": "yarn_in", "then": ["yarn_bat_l", "yarn_bat_r"], "rounds": [1, 4], "last": "yarn_away" } }
```

It plays `first` (the ball bounces in from out of the picture), then from 1 to 4 of `then` (a swat to the left or to the
right), never the same one twice running, then `last` (a swat that knocks it out of sight). `first` and `last` are optional, and each may be one name or a list. The picks come from
the Molty's seed: a new one each time the device starts, a fixed one in tests, so a test plays the same game every time.
Agents ask for it by its name, as for any gesture.

Each action in a chain starts in the pose the one before it ends in, and a prop is where it was (`check` names any two that
do not join). Where the Molty stands and the way it faces carry on: an action that walks or turns it on its feet leaves it
there, facing the new way, and the next one starts from there. That is how a Molty turns without turning as a block. The
cat's eat is a chain too: the bowl comes in to its left or to its right (`first`), it turns to it, and `eat_dish` eats
from it.

## A body from a 3D tool

A bigger Molty keeps its body in a file: `"body": "cat.glb"`. Any tool that saves glTF works. The file needs:

| in the file | becomes |
|---|---|
| nodes | the bones, by name |
| a node whose `extras` say `"shape"`, `"color"`, `"outline"` (and `to`, `radius`, `stiffness` where the shape has them) | a shape on its parent bone, placed, turned and sized as the node is |
| animations, by name, one key a frame | the actions; `extras` may say `view`, `follow`, `stride` |
| `extras.foot` on a node: a point | where that bone touches the floor |

Then list in `molty.json` the bones that stand on the floor (`"feet"`) and two bones the view stays between (`"center"`).

## Life

A mood's action loops, and a loop the eye can count looks wound up. `life` makes a Molty move on its own over its loops:
it breathes, blinks, looks around, never the same way twice. Nothing in it is required, and each part adds on its own.

Blip's whole life is a breath. Add this to `blip/molty.json`:

```json
"life": {
  "signals": { "breath": { "wave": { "_": 0.3, "sleepy": 0.15, "happy": 0.5 } } },
  "layers": [{ "layer": "breathing", "turn": { "bones": "body", "squash": ["breath", 0.04] } }]
}
```

A **signal** is a named number that moves; `breath` swings from -1 to 1, 0.3 times a second, slower when sleepy. A
**layer** is what a signal moves, added over the loop: here Blip's body stretches up 4 % and back as `breath` swings.

Any number in a signal may be a mood table, `{ "_": 0.3, "sleepy": 0.15 }` (`_` for the moods it does not name; it slides
from one mood's value to the next as the mood changes), or another signal's name.

| a signal that | write | is |
|---|---|---|
| a value per mood | `{ "mood": { "_": 0.5, "sleepy": 0.1 } }` | |
| reads another | `{ "from": "idle_time", "map": [[60, 0], [300, 1]] }` | straight lines between the points |
| adds some up | `{ "sum": [["energy", 1], ["tired", -0.3]] }` | |
| swings | `{ "wave": 0.3, "vary": 0.15, "rise": 0.4 }` | -1 to 1, that many times a second, each swing a little different; `rise`: the share going up |
| wanders | `{ "noise": 0.2 }` | -1 to 1, smoothly |
| holds a place | `{ "hold": [0.6, 3], "range": [-1, 1], "move": 0.12, "home": 0.4, "waits": "blink" }` | a new place in `range` after a random 0.6 to 3 s, there in `move` s; `home`: the chance it goes back to 0; `waits`: it never moves while that one is on (or about to start) |
| pulses now and then | `{ "every": [3, 7], "lasts": 0.25, "double": 0.15 }` | 0 to 1 and back, after a random 3 to 7 s; `double`: the chance of a second one right after |
| follows another | `{ "follow": "look_x", "lag": 0.25, "bounce": 0.2 }` | a beat behind it, swinging past by `bounce` |

Any signal also takes `scale` (times this), `range` (kept inside it), `when` (only while that signal is on) and
`ease` (seconds to settle). Signals may read the device's: `t` (seconds), a mood's name or `mood_<mood>` (how much it is
in that mood, 0 to 1), `working`, `free` (1 while no gesture plays), `idle_time` (seconds since anyone did anything) and
`life` (the owner's setting: 1 lively, 0.5 calm, 0 off).

A layer either plays an action at the place a signal sets, or turns bones:

| layer | does |
|---|---|
| `{ "layer": "blink", "clip": "blink_pose", "by": "blink" }` | `blink` 0 shows the action's start, 1 its end. Make such an action short: its first key how the Molty is, its last the full effect (eyes shut) |
| `"zero": 0.5` | the place that adds nothing: an action that goes left, neutral, right, moved by a signal from -1 to 1 |
| `"bones": ["eye.L", "eye.R"]` | only these bones of the action (`"tail6-tail12"`: a chain) |
| `"weight": "free"` | how much of it shows, 0 to 1 (a number, a signal or a mood table), eased: `free` fades it out while a gesture plays |
| `{ "layer": "lean", "turn": { "bones": "head", "z": ["look_x", 8], "squash": ["breath", 0.04] } }` | bones turned by a signal times degrees (`x`, `y`, `z`), or stretched up by it |

A layer adds the difference from its action's neutral place, so a breath made on the sitting cat works standing too.
The cat's life, in `moltys/cat/molty.json`: a breath whose speed and depth follow its mood and slow down as it is left
alone, blinks at uneven gaps (quicker while listening, now and then two), eyes that look somewhere, hold, and come back,
never mid-blink, and the head following them a quarter second late, all faded out while a gesture plays (here the
looks to the side; `look_y` and `head_y` do the same up and down):

```json
"signals": {
  "energy": { "mood": { "_": 0.45, "listening": 0.7, "speaking": 0.7, "happy": 0.9, "sleepy": 0.1, "surprised": 1 } },
  "tired": { "from": "idle_time", "map": [[60, 0], [300, 1]] },
  "calm": { "sum": [["energy", 1], ["tired", -0.3]] },
  "breath_rate": { "from": "calm", "map": [[0, 0.16], [1, 0.42]] },
  "breath": { "wave": "breath_rate", "vary": 0.15, "rise": 0.4, "scale": { "_": 0.6, "sleepy": 0.9, "happy": 0.5 } },
  "blink": { "every": { "_": [2.5, 6.5], "listening": [2, 4], "sleepy": [8, 14] }, "lasts": 0.25, "double": 0.15 },
  "look_x": { "hold": [0.6, 3], "range": [-1, 1], "move": 0.12, "home": 0.4, "waits": "blink" },
  "head_x": { "follow": "look_x", "lag": 0.25, "bounce": 0.2 }
},
"layers": [
  { "layer": "breathing", "clip": "breath", "by": "breath", "zero": 0.5 },
  { "layer": "blink", "clip": "blink_pose", "by": "blink" },
  { "layer": "eyes", "clip": "look_lr", "by": "look_x", "zero": 0.5, "bones": ["eye.L", "eye.R"], "weight": "free" },
  { "layer": "gaze", "clip": "look_lr", "by": "head_x", "zero": 0.5, "bones": ["neck", "head", "ear.L", "ear.R"], "weight": "free" }
]
```

`check` names any signal it cannot find, a loop between signals, an action or a bone a layer names that is not there,
and a life that is more than the device gives it. Gestures started by a signal (`moments`), a pulse on a touch (`on`) and
the mouth following the voice (`peaks`) come in a later firmware.

## Commands and limits

`molty-creature check <folder>` finds what is wrong and says how to fix it; `build` checks, then writes the file a
device installs; `preview` shows every mood, gesture and action moving, on your computer.

| what | limit |
|---|---|
| bones, shapes, actions, poses, gestures; letters in a name | 80, 128, 64, 8, 32; 24 |
| the built Molty | 256 KB, as every file in this repo, which is also the agents' plugin (its loaders refuse a file of 256 KB or more); the cat, with 54 actions, is 255 KB |
| life: signals, layers; its work a frame | 64, 16; 600 steps (`build` says how many; the cat's are 44) |
| words in `about`, `look`, a mood's or a gesture's line | plain ASCII; 160, 240, 100 letters |
| action keys in a body file | straight-line or held keys; curve keys are refused: save one key a frame |

# How we make ours: from Blender

The cat is a Blender file, `moltys/cat/cat.blend`, and one script turns it into the cat's body file. The script is
ours, one way among others to make a body file; the tools above never need Blender.

1. Open `cat.blend` (or a copy, for a new Molty) in Blender 5.3.
2. Reshape. Each shape Molty draws is an Empty named `shape...`, the child of its bone. Move it and scale it; its
   custom properties say what it is (`shape`, `color`, `outline`, and `to`, `radius` for the ones with two ends).
3. Animate, and name your actions as above. Give an action a manual frame range (its length), tick Cyclic
   Animation for a loop, and add the custom properties `view`, `follow` or `stride` when it needs them.
4. Save, then write the body file (about a minute for the cat) and build (needs only the folder):

```sh
blender -b moltys/cat/cat.blend -P tools/blender/molty_export.py -- --out moltys/cat/cat.glb
node packages/tools/molty-creature.mjs build moltys/cat
```
