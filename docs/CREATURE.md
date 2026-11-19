# Make a Molty

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

Watch it move on your computer: `node packages/tools/molty-creature.mjs preview blip`, then open the address it prints.
The build writes one small file, the Molty a device installs. That is the whole loop.

## Space and units

- Up is `y`. The Molty faces `+z` and its left is `+x`. The floor is `y = 0`.
- Sizes are the body's own: a cat stands about 0.6 high, and the screen shows about 1.75 across.
- A turn is degrees about `x`, `y` and `z`: `[10, 0, 0]` nods, `[0, 20, 0]` looks to its left, `[0, 0, 7]` tilts.
- A time is seconds from the start of the action.

## Bones and shapes

A bone has a `name`, the `parent` it hangs from and where it starts (`at`, from its parent's start). List a bone
after its parent. A shape sits `on` a bone and moves with it.

| shape | takes | is |
|---|---|---|
| `blob` | `at`, `radii`, `turn` | a soft ball that melts into the blobs next to it: a body |
| `ellipsoid` | `at`, `radii`, `turn` | a ball with three radii: a head, a paw, a flat disc for an eye |
| `capsule` | `at`, `to`, `radius` (one, or one for each end) | a rounded stick: a leg, a tail |
| `cylinder` | `at`, `to`, `radius` | the same with flat ends: a bowl |
| `cone` | `at`, `to` (its tip), `radius`, `flat` | a flattened cone: an ear |
| `patch` | `at`, `radii`, `turn` | a region of color on the body: a white chest |

Every shape names a `color` (one of `colors`) and an `outline`: the part it belongs to. Shapes of one part are
drawn as one outlined form. These are the parts a Molty has:

| outlined parts | features |
|---|---|
| `torso` `head` `tail` `legF.L` `legF.R` `hindL` `hindR` `bowl` `yarn` | `eye` `nose` `maw` `tongue` `earIn` `whisker` `chest` `milk` `yarnline` |

Molty paints outlined parts from the color `body`, and eyes from `iris` and `shine`. `check` tells you which colors
your parts need.

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

An action may also say `"view"` (`front`, `side` or `three_quarter`: where it is seen from), `"follow": true` (the
view goes along, for a walk) and `"stride"` (a walk's seconds a step).

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
through the Molty. To play an action under another name, say so: `"inspect": { "about": "peers low", "plays": "peek" }`.
A looping action ends on the pose it starts with. Which pose an action starts and ends in is worked out from how
it looks, so the ways between poses are found even without the `_to_` name.

## A body from a 3D tool

A bigger Molty keeps its body in a file: `"body": "cat.glb"`. Any tool that saves glTF works. The file needs:

| in the file | becomes |
|---|---|
| nodes | the bones, by name |
| a node whose `extras` say `"shape"` (with `color`, `outline`, and `to`, `radius`, `stiffness` where the shape has them) | a shape on its parent bone: its position, rotation and scale are the shape's `at`, turn and `radii` |
| animations, by name, one key a frame | the actions; `extras` may say `view`, `follow`, `stride` |
| `extras.foot` on a node: a point | where that bone touches the floor |

Then list in `molty.json` the bones that stand on the floor (`"feet"`), and two bones the view stays between
(`"center"`).

## Commands and limits

| command | does |
|---|---|
| `molty-creature check <folder>` | finds what is wrong and says how to fix it |
| `molty-creature build <folder>` | check, then the file a device installs |
| `molty-creature preview <folder>` | a page on your computer: every mood, gesture and action, moving |

| what | limit |
|---|---|
| bones, shapes, actions | 80, 128, 64 |
| poses, gestures | 8, 32 |
| names | 24 letters |
| the built Molty | 256 KB (the cat, with 45 actions, is 242 KB) |
| words in `about`, `look`, a mood's or a gesture's line | plain ASCII; 160, 240, 100 letters |
| action keys in a body file | straight-line or held keys; curve keys are refused: save one key a frame |

# How we make ours: from Blender

The cat is a Blender file, `moltys/cat/cat.blend`, and one script turns it into the cat's body file. The script is
ours, an example of one way to make a body file; the Molty tools above never need Blender.

1. Open `cat.blend` (or a copy, for a new Molty). It needs Blender 5.3.
2. Reshape. Each shape Molty draws is an Empty named `shape...`, the child of its bone. Move it and scale it; its
   custom properties say what it is (`shape`, `color`, `outline`, and `to`, `radius` for the ones with two ends).
3. Animate, and name your actions as above. Give an action a manual frame range (its length), tick Cyclic
   Animation for a loop, and add the custom properties `view`, `follow` or `stride` when it needs them.
4. Save, then write the body file and build:

```sh
blender -b moltys/cat/cat.blend -P tools/blender/molty_export.py -- --out moltys/cat/cat.glb
node packages/tools/molty-creature.mjs build moltys/cat
```

The first command poses the rig on every frame of every action and saves what it sees. The second needs only the
folder.
