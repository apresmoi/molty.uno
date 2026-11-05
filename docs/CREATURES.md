# Creatures

A creature package (`kind: "creature"`, [PACKAGES.md](PACKAGES.md) section 5) is one Molty: its body and everything it does,
as data. The device plays it. The firmware has no duck, cat or octopus code; it has a player that knows channels, terms and
a fixed list of factor ops, and nothing about what they draw.

```
packages/<id>/build.mjs ──node──▶ packages/<id>/<id>.json (≤ 40 KB) ──▶ player (JS reference: device/creatures/js/creature-player.js;
        uses tools/creatures/kit.mjs                    C port: main/creature/creature_player.c) ──▶ bone transforms, colours, marks
```

The motion model is the one the lab's animals were written in (`molty-creature-lab/animals/kinds`): a shared pose from the
mood engine and the gestures (`js/mood.js`, `js/actions.js`, `cute_act.h`), then the creature's own layers on top. Every line
of that code became a channel; nothing was dropped on purpose (losses are listed at the end).

## 1. The package

```jsonc
{ "v": "molty-pkg/1", "id": "duck", "name": "Duck", "version": "1.0.0", "kind": "creature",
  "creature": {
    "about":    { "look": "...", "gestures": { "<action>": "..." }, "moods": { "<mood>": "..." } },
    "body":     { "palette": {...}, "bones": [...], "parts": [...] },  // the lab's v2 rig (SPEC.md), unchanged
    "walk":     { "hz", "stride", "pace", "burst", "radial", "quad", "surge": [terms], "ch": [channels] },
    "groups":   { "<name>": { "bones": [ids], "attr": { "<k>": [one number per bone] } } },
    "events":   { "<name>": Event },
    "base":     [channels],
    "gestures": { "<action>": { "ch": [channels], "replace": true, "dur": 4.6 } },
    "moods":    { "<mood>": { "ch": [channels], "now": false } },
    "post":     [channels],
    "tints":    [Tint],
    "ambient":  Ambient } }
```

**About** is for the agent that speaks as this Molty, never for the player: `look`, 2 or 3 plain sentences on what a
person sees on the round screen (at most 240 characters), and one line for every gesture and every mood it plays,
`idle` included, on what that looks like on this animal (at most 100 characters each; printable ASCII).
`packages/tools/format.mjs` checks it is complete. The device sends it to the relay with the gestures and moods it plays
(`cp_info`; [AGENT-LINK.md](AGENT-LINK.md#device-events)), and agents get it as `device.creature`.

**Gestures** are named `[a-z][a-z0-9_]{0,23}`. A shared one (`js/actions.js`, `cute_act.h`: nod, wiggle, ...) plays over the
shared pose and keeps its length (no `dur`); any other name is the creature's **own** gesture and gives its length, `dur` 0.2..10 s
(the cat's `eat`). Every creature plays the shared ones; an agent's `act` plays one of its own too, and the device answers
`unknown_gesture` for a name the creature lacks. Its parts may exist only for it: the cat's bowl is a bone scaled from 0 to 1
by a channel (`bowl:s`, `op: "="`), like its tongue.

Bones may carry `sit` and `fold` (a leg's seated position and folded length, read by the kit's generators at build time),
`wave` (an arm joint's axis, joint and arm index, the same) and `d` (depth down its chain, for colour that ripples).

## 2. Channels and terms

A **channel** is a named number. Each frame it is the sum of its terms:

```
channel = [name, [term, ...], options?]
term    = [gain, factor, factor, ...]          value = gain × factor × factor × ...  (a zero ends the product early)
factor  = "@name"                               another channel's value
        | [op, source, param, ...]              an op applied to a source, see the table
```

- **name**: `pos.x` and the other pose fields write the shared pose; `bone:prop` writes a bone; `%name` is local to one
  group instance; `_name` is scratch; any other name (`duck.wl`, `earF.l`, `flush`) is the creature's own.
- **source**: `"a"` the block's own clock (seconds into the gesture, the mood or the event; the walk cycle in `walk`),
  `"@name"` a channel, `"$k"` the group instance's attribute (`$i` its index), `"~n"` the block's n-th random draw (0..1),
  or a number.
- **param**: a number; `[lo, hi]` drawn once per play; `[lo, hi, n]` drawn from slot n, so ranges with one slot move together
  (a gesture's frequency and twice it); `"D"` the block's length; `"$k"` an attribute; `{ "_": base, "<mood>": k, ... }`
  base plus k times that mood's weight (only in options).
- **modifier**: `abs:`, `pos:` (max 0), `neg:` (max of 0 and minus), `sq:` before an op (`["pos:sin", "@t", 4]`).

| op | params | value (x is the source) |
|---|---|---|
| `v` | | x |
| `sin` | hz, phase (rad) | sin(2π hz x + phase) |
| `nz` | hz, seed, offset | smooth value noise at hz x + offset, -1..1 |
| `env` | d, attack, release | ss(x / a) ss((d - x) / r) |
| `bump` | a, b | sin(π (x - a)/(b - a)) inside a..b, else 0 |
| `ss`, `ssd` | at, len | smoothstep((x - at)/len), and 1 minus it |
| `exp` | k, at | e^(-k max(0, x - at)) |
| `cyc` | hz, phase (cycles), up | a repeating rise over `up` of the cycle and smooth fall |
| `bumpr` | hz, phase, a, b | `bump` on the cycle fraction |
| `step` | hz, phase, d | a foot: -1 to 1 linearly over the stance d, back by a cosine |
| `frac` | hz, phase | the cycle fraction |
| `pulse` | hz, seed, width | beats near hz, each its own height, a little early or late |
| `dz` | hz, seed | noise with a dead zone: mostly still, now and then a decided move |
| `steps` | period, seed | holds a random value, snaps to a new one each period |
| `chance` | period, seed, p, offset | 1 for the periods whose hash is under p |
| `flap` | | an asymmetric beat on a phase in cycles: down fast, up slower |
| `lt`, `gt` | a | 1 when x < a (x > a), else 0 |
| `is` | a | 1 when round(x) = round(a) |
| `sgn` | | -1 under 0.5, else 1 (a side from a draw) |
| `lin` | a, b | a + b x |
| `clamp` | lo, hi | x clamped |
| `pow` | e | sign(x) abs(x)^e |
| `rcp` | a, b | 1 / (a + b x) |
| `asin` | | asin(x) |
| `fl` | k | floor(k x) |
| `dl` | seconds | the source channel's value that long ago (it needs `hist`) |

### Options

| option | effect |
|---|---|
| `op` | `+` (default) adds; `max` keeps the larger (lids, mouths); `*` multiplies by (1 + value); `=` sets |
| `cl` | [lo, hi]: clamp the value |
| `int` | integrate the value over time (a phase in cycles, a breath) |
| `lp` | low-pass at this rate (1/s; may be a mood mix) |
| `sp` | [k, c]: a spring toward the value (4 substeps); with `lag` the output is spring minus value (follow-through) |
| `d` | [lo, hi]: the rate of change per second, clamped (how fast it is turning) |
| `hist` | seconds of history kept for `dl` (at most 1) |
| `keep` | the value survives into the next frame until the channel runs again (read early by an event's gate) |

A stateful channel (`int lp sp d`) sets by default. Its state is its own; the value is what it writes.

## 3. Inputs

Every frame starts with every channel at 0 except: the shared pose (`pos.x/y/z rot.x/y/z head.x/y/z spine.x/y/z squash lid
mouth look.x/y prowl`), `t` and `dt`, `act` (1 while a gesture plays), `work` (1 while the agent is working: mood `working`), the walk (`walk.q k go along side fz up`), per mood
`is.<m>` (1 or 0), `w.<m>` (weight, eased at 4/s), `w6.<m>` (eased at 6/s), `now.<m>` (1 at once when it comes, the
weight as it leaves), `m.<m>` (seconds since it last came), the eye word `eye.wide/happy/sleepy/squint`, and `keep` channels.
Moods are `idle listening thinking speaking happy sleepy surprised`.

## 4. Layers, in play order

1. **walk** `ch`, clock = the walk cycle. `surge` (terms over `walk.q`) is how much ground a burst covers at that point.
2. **events**: each a scheduler. `first` (s), then `gap` [lo, hi] after each one ends, or `rate` (starts per second, times
   the gate). It starts when `gate` (terms) is over 0.5, plays `len` (a range is drawn) and ends early when `stop` is over 0.5.
   With `pick`, one member is chosen at random each time; members have their own `len`. Outputs are scaled by `w` (terms).
3. **base** channels, every frame.
4. **gestures**: the current one at weight 1 and the one fading out (the mood engine's crossfade). `replace` (default) first
   removes the shared gesture's own offset, so the creature does it its own way. Clock = seconds into it, `D` its length.
5. **moods**: each at its weight `w.<m>` (or `now.<m>` with `now: true`) while over 0.01. Clock = seconds since it came.
6. **post**: the last word on the pose (follow-through, smoothing, damping), then the bones.

Draws (`~n`, ranges) are new each time a block plays: each gesture play, each time a mood comes, each event. A block's
weight scales what it writes to the pose and own channels; scratch, locals and stateful channels are never weighted.

### Waiting on the agent

Each package carries a `ponder` event (`kit.mjs` `ponder`): gated on `is.thinking` and `work`, it picks one small expression
every 2 to 5 s: a glance aside, a head tilt, a double blink, a look up, a little yawn once it has thought 14 s, and the
animal's own (the cat's ear flick and tail swish, the duck's ruffle and nibble, the octopus's passing cloud and curl). The
firmware chooses nothing: while a package creature is on screen, `working` hands it no work actions (`fd_set_own_work`).

## 5. Bones

`bone:prop` adds to a bone reset to its rest pose this frame. `rx ry rz` (rad) and `px py pz` add; `sx sy sz s` multiply by
(1 + value), or set with `op: "="`; `sq` squashes keeping volume (y × (1 + v), x and z × 1/√(1 + v)). `root` is the whole body.
Several channels on one output combine (sums, products).

A **group** runs its channels once per bone: `{ "each": "arms", "ch": [...] }`, where `:rx` is that bone, `%c` a local and
`$ax` that bone's attribute. The octopus's 32 arm joints are one list of 9 channels and 24 attributes.

## 6. Colour and the glass

**Tints**: `{ ch, to: { paletteName: "#hex" }, neg: {...}, wave: [hz, lag per depth, k] }`. A palette colour moves toward
`to` by the channel (toward `neg` when negative). With `wave` the amount ripples down the body by bone depth `d`:
k v max(0, sin(2π (hz t - lag d)))². Only the named palette entries change (the octopus's skin, never its eyes).

**Ambient**: particles and a current, around the body. The body is the creature's own coverage this frame, so particles
pass behind it.
- `kinds`: `bubble` (rises at `rise` [base, per size, random], speeds up by `acc`, wobbles, pops at the glass edge `glass`
  into a star with `pop` [time, colour, size over which it splashes, strength, radius]); `blot` (an ink clump of `pts`
  blobs that blooms, sinks, turns and fades over `life`).
- `emit`: `{ p: kind, at: floor|side|top|bottom|center, every: [lo, hi] | on: channel (when it rises past 0.5) |
  rate: [channel, per second, min], n, dx, dy, r, rp (size skew), stack, stagger, twin, impulse: [strength, radius] }`.
- `motes`: a fixed drift of `n` specks. `current`: four channels (x, y, vx, vy) pushed into the fluid each frame.
At most 64 live particles.

## 7. Limits

| | limit | duck / cat / octopus |
|---|---|---|
| file | 40 KB | 30.1 / 31.4 / 32.8 KB |
| bones, parts | 96, 96 | 13, 18 / 22, 41 / 37, 45 |
| channels (all blocks) | 512 | 334 / 398 / 254 |
| terms, factors | 1024, 2048 | 482, 824 / 598, 826 / 341, 500 |
| factors in one term | 8 | |
| group instances, attributes | 64, 32 | octopus 32 × 24 |
| events | 16 | 8 / 7 / 5 |
| history | 1 s per channel | |
| live particles | 64 | |

## 8. Cost on the device

Per frame the player evaluates the base, walk, post, the running events, at most two gestures and the moods in weight.
Measured with the reference player over two minutes of every mood and gesture (`tools/creatures/player.test.mjs`):
worst frame 370 / 533 / 1157 factor evaluations, mean 283 / 464 / 1074 (duck / cat / octopus).

In C on the ESP32-S3 (240 MHz, single-precision FPU), a factor is a switch dispatch, a few loads and its maths: about 20
cycles for sin from a table, 40 for noise (two integer hashes and a smoothstep), 60 for `exp`; 40 on average with the term
and channel loop around it. The octopus's worst frame is then about 46 k cycles, **0.2 ms of a 33 ms frame** (0.5 ms if it
were 100 cycles a factor). The compiled program (24 bytes a factor, 8 a term, 16 a channel, 4 an attribute) is about
28 / 26 / 22 KB: internal RAM or PSRAM, either is fine. Bones, colour and particles cost what the renderer already spends.

The C port must match the JS player's semantics, including the hash: murmur3 fmix32 of floor(16 n), so the same package
draws the same numbers on both.

**As built** (`main/creature/creature_player.c`): channel values are float; the clocks (time, mood clocks, a block's clock,
integrated phases) and the hash's argument are double, and the periodic ops (`sin nz frac cyc bumpr step pulse dz steps chance
flap`) form their argument in double, so a creature moves the same after a day on as after a minute. It is built without fused
multiply-adds (`-ffp-contract=off`): a hash's argument must round as JS rounds it. Against the JS player's goldens it agrees to
1e-6 on the pose and 1e-4 on the bones, with every draw and every ambient particle the same. On the host the player is 2 to
10 µs a frame; the body's raster (2 to 3 thousand facets of the lab's 14×10 ellipsoids, a coarser mesh for an ellipsoid whose
facets are under a sample while the camera is not zoomed) is most of the creature's cost.

## 9. 2D sprites

They stay a **lab-only render** and are not part of the format. The device draws the rig (bones and parts) into the fluid,
so the rig is the one thing a package has to carry. The lab's sprites (`animals/flat.mjs`) are a second, hand-drawn
renderer per animal (its own drawing code for wings, ears and arms), so carrying them would mean a second body format and a
second set of channel bindings for each creature, for a view the device does not show. If a flat look is wanted on the
device later, it should come from the same rig (an orthographic side render), not a second drawing.

## 10. Building and checking

```sh
node packages/duck/build.mjs        # writes packages/duck/duck.json and its packages/index.json entry
node --test tools/creatures/*.test.mjs   # the player's rules, and every creature package plays clean within its limits
```

`tools/creatures/kit.mjs` holds the body helpers (the lab's) and the generators for the layers every Molty shares: the
fidgets (`alive`), the living layer (`living`: pose onto root, torso and head, wings, ears, tails with follow-through,
legs, arm ripples, feet, eyes, mouth) and the blink and breath motions. A package calls them at build time, so the result
is plain data in its own file; another creature can tune or drop any of it.

The lab renders a package through its real renderer beside the code version:
`node pkg/render-pkg.mjs --repo <this checkout> --out <dir> --only duck` in `molty-creature-lab`.

## 11. Fidelity: what the data version loses

Compared over the full reels (every scene, mood and gesture) against the approved clips:

- **Randomness differs in detail, not in kind.** The hash is new (an integer hash the C side can match), so blink timing,
  idle events, the octopus's wandering arm and every per-play draw land on different values than the code version.
- **Living layer ears**: the head's spring follow-through no longer feels the body's vertical speed (a small term).
- **Cat's held tail** (tail up, tail wrapped): the lab bends the chain toward set directions from wherever it is each
  frame; the package carries rotations solved at build time for the cat sitting, standing and arched, blended by those
  channels. Close sitting, happy and loafed. Arched (surprised) it stands straighter and taller than the code's hooked
  bottle brush, the one difference that shows in the reel.
- **Cat's stalk**: the leg lift while planted uses a two-term series for 1 - 1/cos(asin x) (under 0.1% off).
- **Duck's waddle swing**: the foot's return uses a cosine where the code used a smoothstep (under 2% of the stride).
- **Octopus pointing**: an arm lifts when the glance points its way by the glance's amount, not past a threshold, so it
  rises smoothly instead of switching on.
- **Octopus breath bubbles** come at the breath's average rate instead of on each breath's peak.
- **The duck's idle "quiet" gate** is read before the creature's own gestures and moods add to the pose this frame, so a
  pose that tips the head far forward may start an idle event a frame early.
- **The octopus's surprised clock** runs from the last time it was surprised, so leaving the mood early lets its sharp
  moments run on as the weight fades (the code froze them at their settled values). In the reel it stays pale a beat
  longer after a fright.
- **2D sprites**: not carried (section 9).
