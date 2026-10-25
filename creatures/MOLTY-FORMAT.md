# .molty — swappable creatures for the device

A `.molty` file is one cute-kit creature (`js/cute/kit.js` `defineCreature` spec) in a
compact binary form. The firmware holds the generic kit **runtime**; it does not hold any
creature. A creature is data, uploaded at runtime, so creatures can be swapped without a
reflash.

```
js/cute/*.js spec ──molty-export.mjs──▶ name.molty (≈0.4–1.3 KB) ──POST /api/creature/file──▶ /spiffs/creature.molty
                                                                                          │ hot reload
            firmware: cute_spec.c (parse+validate) → cute_rig.c (meshes, rig, moods, raster) → fluid target
```

- Writer: `creatures/tools/molty-export.mjs` (`node … molty-export.mjs <id|all> [outdir] [--json]`).
  Hook stand-ins live in `tools/molty-behaviours.mjs`.
- Reader: `firmware/main/creature/cute_spec.c`. The runtime is
  `cute_rig.c`. Host test: `firmware/test/host/cute_build.sh`.
- Exported files: `creatures/molty/`. The firmware default (`firmware/main/creature/default.molty`)
  is the duck.

## What is (and is not) in the file

**In the file:** every value `defineCreature` declares, **fully resolved** by the exporter.
That covers:
- kit defaults applied;
- all 13 sockets placed (creature space, rest pose, +x side);
- neck pivot, jaw point, neck ring and per-part segment counts computed;
- the palette merged with `DEFAULT_PALETTE`.

So the device carries no default logic.

**Not in the file:**
- **Meshes.** The device generates each part from its primitive parameters, with the same
  vertex order and winding as three.js.
- **Per-frame animation.** It comes from the runtime: the mood engine (`mood.js`) and the kit
  (`update`, `MOTIONS`, `_face`, `_limbs`).
- **JS hooks.** They become declarative **behaviours** (below).

`custom` geometry cannot ship. The exporter approximates a custom part from its declared
`size` (teardrop if it has `pinch`, ellipsoid otherwise, cylinder if `r`+`h`), or skips it
with a warning. The robot's screen, stalk and spring and the ghost's skirt are skipped today.

## Layout (v1)

Little-endian. `q` = int16 fixed point, value = q / 4096 (±8): lengths, angles (rad),
amplitudes and gains. `q8` = int16, value = q / 256 (±128): rates (Hz, 1/s) and periods (s).
`str` = u8 length + UTF-8.

| field | type | notes |
|---|---|---|
| magic | 4 B | `MLTY` |
| version | u8 | 1 |
| flags | u8 | b0 has body · b1 cheeks · b2 eye shine |
| length | u16 | whole file including the CRC |
| id, name | str, str | |
| palette | u8 n, n × (str key, u8 r, g, b) | default keys first, then the spec's, then `#hex` literals |
| head | q3 size, q3 at, q3 pivot, q squash, q pitch | pivot = neck point (resolved) |
| body | q3 size, q3 at | only if flags.b0 |
| face | q3 r, q3 offset | the ellipsoid the face sits on |
| eyes | q x, q y, q r, u8 shape, q tilt, u8 colour | shape: round / tall / wide |
| mouth | u8 type, q y, q w, q open | smile / cat / jaw / none |
| cheeks | q x, q y, q r | used only if flags.b1 |
| face colours | u8 shine, white, mouth, tongue, cheek | palette indices |
| motion | u8 type, q amp, q rate, q height | none / waddle / hop / slither / float / flutter / wiggle |
| limbs | q side, ear, tail, foot, antenna | amplitude multipliers |
| sockets | u8 n = 13, n × (u8 on, q3 pos, q3 rot) | kit `SOCKETS` order; on: root / body / head |
| neck ring | q ex, q ez | for neck items |
| items | u8 head, u8 neck | `HEAD_ITEMS` / `NECK_ITEMS` index (runtime v1 does not draw items yet) |
| names | u8 n, n × str | part names (behaviour targets) |
| parts | u8 n, n × part | body and head come first, as parts |
| behaviours | u8 n, n × behaviour | |
| crc | u32 | CRC-32 (IEEE, zlib) over everything before it |

**part:**
- `u8 type`, `u8 socket`, `u8 colour`, `u8 name`
- `u8 flags`: b0 mirror · b1 hidden · b2 scale · b3 offset
- `u8 rotOrder`: XYZ YXZ ZXY XZY YZX ZYX
- `u8 segA`, `u8 segB`
- `q3 pos`, `q3 rot`, then `[q3 scale]`, then `[q3 offset]` (geometry translate, e.g. pack-b `hinged()`)
- `u8 np`, then `np × q` params

| type | params | segA, segB |
|---|---|---|
| ellipsoid / sphere | sx sy sz | width, height segments |
| teardrop | sx sy sz pinch | width, height |
| capsule | r len | radial, capSegments |
| cone | r h | radial |
| cylinder | rTop rBottom h | radial |
| torus | R tube arc | radial, tubular |
| box | sx sy sz | — |
| disc | r | segments |

The device rejects a file when any of these fail:
- size ≤ 16 KB;
- magic;
- version;
- length;
- CRC;
- every index in range;
- segment counts 1–32;
- the reader ends exactly at the CRC.

## Behaviours (the hook vocabulary)

A behaviour drives one transform channel of one target, after the kit has animated the
frame (the same place `update(ctx)` runs).

```
behaviour = { target, channel, op, terms[] }      value = Σ amp · G(w) · [G_B(w)] · driver(t)
G(w) = k + Σ coef_mood · w_mood                   (w = the kit's smoothed mood weights, 0…1)
```

- **target**:
  - `part <name>`: every mesh with that name; mirrored copies carry side −1;
  - `socket <name>`: every socket instance, L and R for paired sockets;
  - `root`, `head` (the head pivot), `body`;
  - `eyes`: the lid channel only.
- **channel**:
  - rotation: `rx ry rz`;
  - position: `px py pz`;
  - scale: `s` (uniform), `sx sy sz`, `sxz`;
  - `lid`: eyelids, 0 open … 1 closed. Taken as `max` with the kit's own lid, so `≥0.8` shows ‿ ‿.
- **op**:
  - rotation and position: `set` = rest + value, `add` = current + value;
  - scale: `set` = rest·(1+value), `add` = current·(1+value).
- **driver**:

  | driver | value |
  |---|---|
  | `const` | 1 |
  | `sin` | sin(2π f t + phase) |
  | `abssin` | \|sin\| |
  | `possin` | max(0, sin) |
  | `square` | 1 / 0 |
  | `mouth` | the pose's mouth opening |
  | `lagx`, `lagz` | the kit's spring follow-through |
  | `pulse` | a sin bump of length `f` seconds, every `p0…p1` s (random) |
  | `spin` | the integral of amp·G over time (rad), for dials and propellers |

- **term flags**:
  - `sideSign`: × side (±1);
  - `sidePhase`: +π on the left instance;
  - `ease`: the gain follows its target at rate `ease` (1/s), like the hooks' `ease(...)`;
  - `gainB`: multiply a second gain (products such as `(1−happy)(1−0.7 sleepy)`).

Example (kitten, from `molty-behaviours.mjs`): the tail swish. The JS
`base.z + 0.38(1−happy)(1−0.7·sleepy)·sin(2π·0.5t) + 0.08·happy·sin(2π·6t)` becomes

```js
{ target: { kind: 'socket', name: 'tail' }, channel: 'rz', op: 'set',
  terms: [{ driver: 'sin', amp: 0.38, freq: 0.5, gain: { k: 1, happy: -1 }, gainB: { k: 1, sleepy: -0.7 } },
          { driver: 'sin', amp: 0.08, freq: 6, gain: { happy: 1 } }] }
```

### Coverage of the pack creatures' hooks (v1 vocabulary)

| creature | signature (hook) | v1 |
|---|---|---|
| duck, mochi 2 | none (kit only) | ✅ exact |
| kitten | tail swish, slow blink, bottlebrush tail, kneading, whisker twitch | ✅ authored (13 behaviours) |
| penguin | extra flipper flap, surprise shiver | ✅ expressible (`possin` / `sin` on part rz, sideSign) |
| frog | throat sac puffs with speech; tongue flick | ✅ sac (`mouth` → part `s`); ⚠ the tongue needs show/hide |
| hamster | pouches puff while talking, stay stuffed when happy | ✅ (`mouth` + eased happy → part `s`) |
| piglet | snout bounce on oink, tail spins when happy | ✅ (`mouth` → py, `spin`) |
| bear | paws clap when happy, thrown up when surprised | ✅ (socket rz `possin` + const gains) |
| crab | claws clap when happy | ✅ (but its legs/pincers are custom geometry, approximated) |
| bee | buzz | ✅ (high-frequency `sin`) |
| owl | head swivel | ✅ (`head` ry `sin`, gains) |
| snail | eye stalks follow | ✅ (`lagx`/`lagz`) |
| star | twinkle-spin | ✅ spin · ❌ dims at dusk (colour) |
| ladybug | shell lifts when delighted | ✅ (eased happy → rx) · its shell is custom geometry (skipped) |
| jellyfish | pulse and drift | ✅ pulse (`sin` → `s`) · dome is custom (approximated) |
| bunny | ear tips flop on every hop, nose twitch bursts | ⚠ twitch ✅ (`pulse`); flop keyed to the hop phase needs a motion-phase driver |
| axolotl, octopus, caterpillar | per-limb wave (each frond/tentacle/segment phase-shifted) | ⚠ needs a per-instance phase offset |
| robot | mood-light colour, dial spin, pogo spring stretch | ⚠ spin ✅; colour (tint) and the spring (derived from jump height) ❌ |
| dragon, shroom, cloud, whale, octopus (ink) | puffs of smoke / spores / rain / spout / ink | ❌ needs show/hide + particles |
| ghost | fades when drowsy | ❌ needs opacity |
| hedgehog | tucks into a ball | ❌ multi-part choreography |

The vocabulary would grow in this order to cover the ⚠ and ❌ rows:

1. a per-instance phase offset (by part index);
2. a `visible` channel (show when the value is over 0.5);
3. a `tint` channel (palette lerp by value);
4. a motion-phase driver;
5. a small particle emitter on a socket.

Each one is additive in the format: a new enum value under the same version.

## Device API

| call | effect |
|---|---|
| `POST /api/creature/file` (binary) | validate, write `/spiffs/creature.molty` (tmp → fsync → rename), hot reload; `{"ok":true,"bytes":N}` or `{"ok":false,"error":"crc"}` |
| `GET /api/creature/file` | the stored file, else the firmware default (`X-Molty-Default: 1`) |
| `POST /api/creature/state {"state":"creature"}` | the fluid relaxes toward the creature |
| `POST /api/creature/state {"mood":"happy"}` | mood (idle, listening, thinking, speaking, happy, sleepy, surprised); a mood alone also shows the creature |
| `GET /api/creature/state` | `mood`, `creature_info: {name, tris, error?}`, plus the fluid's timing (`prof_ms.c_update / c_raster / c_fill`) |

Measured on the 1.46 (240 MHz, 52×32 grid): the duck runs at about 28 fps in the creature
state. Of the ~24 ms of core-1 work, creature update + raster + fill take about 11 ms. The
runtime costs about 30 KB of flash, and the parser 3.8 KB.
