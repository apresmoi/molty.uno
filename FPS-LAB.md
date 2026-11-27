# The frame rate lab

One firmware with run-time knobs and per-frame counters, so each candidate way to a faster Molty is measured on the
device one knob at a time. Stage 1 (built) is the knobs, the counters, measurement 1 and experiments 1 and 2. Stage 2
(designed here, not built) is the sparse cells pass, the hand lane and the baked gesture deltas.

Sources: `research/fps/plan.md` (the order), `research/fps/diagnosis.md` (where a frame goes),
`research/fps/council/out/codex.md` (the outside review and its gates).

## Stage 1: what is in the firmware

### Knobs

Every knob is back at its default on boot, and every default is today's behaviour.

| knob | values | default | what it does |
|---|---|---|---|
| `rec` | 0, 1 | 0 | a record a frame into a ring of 1,024 frames in PSRAM (90 KB) |
| `track` | -1 .. 255 | -1 | the tracked item: -1 every shape and dot the scene hands the drawer, `i` the scene's shape `i` |
| `pose` | 0, 1 | 0 | measurement 1: the creature's pose key a frame into a ring of 2,048 (43 KB) |
| `skip` | 0, 1 | 0 | experiment 1: a pose equal to the frame before's keeps that frame's creature cells, no raster |
| `cache` | 0, 1 | 0 | experiment 2: the creature cells of the last `cache_n` distinct poses kept (LRU) |
| `cache_n` | 1 .. 128 | 16 | the cache's size, 14.9 KB a pose |

HTTP (no reflash): `GET /api/lab` (knobs, defaults, summaries), `GET /api/lab?raw=N` (the newest N frame records as
CSV), `POST /api/lab` with `{"knob": value, ...}` or `{"reset": 1}`. `GET /api/diag` lists the knobs under `lab`.
While any knob is off its default, the ring log gets `lab knobs`, `lab f`, `lab t`, `lab pose` and `lab reuse` lines
every 10 s.

### What a frame record holds

The sim's side at the handoff (fluid_task), the drawer's when it is done (fluid_draw):

- the pulse it woke on, its target pulse (the last one + 2), the pulse of its first window;
- core 1: `sim` (sim_frame), `dust` (the dust's frame ahead and its build), `job` (a change's reading job), `pace`
  (its wait for the pulse), `idle` (its wait for the drawer);
- core 0: `cells`, `send`, `wait` (the last transfer);
- windows and pixels sent, cells changed, cells owed;
- what showed (creature, scene, other) and the tracked item's signature (the creature: its pose key);
- `lab`: the lab's own time in that frame.

The summary (GET, and the 10 s lines; on the lab's task or the HTTP task, never in a frame): mean, p50, p95, p99 and
max of every stage, the handoff interval (`frame`) and `lab`; handoff and first-window intervals in pulses (1, 2, 3, 4
or more); late handoffs, first windows more than 2 pulses apart, frames that sent nothing; and for the tracked item:
handoffs it moved in, distinct positions completed (all of its cells sent, none owed, a window out) a second, and its
longest hold in pulses.

### Measurement 1: does the cat's pose repeat?

The pose key (`cd_pose_key`, creature_dots.c) is a 64-bit hash of everything the raster reads: every node's world
matrix (12 floats a node; the clip, its frame, the blend between poses, breathing, heading, eyes, tail, the life layers
and the scale all end up there) and the camera (x, z, yaw). The shapes and the palette are the clip's own and never
change. The world (floor, dust, sky, weather) is not in it: it is drawn every frame anyway.

The summary over the last minute: frames (and those in a gesture), frames equal to the frame before, distinct poses
(idle, in gestures, a minute), the 8 poses seen most, and the hit rate an LRU cache of 1, 4, 16 and 64 poses would get
on that very sequence (1 is experiment 1).

Host result (the cat package, 1,500 frames: 20 s idle with its life on, nod, idle, stretch, idle): no frame's pose
equals any other frame's; every simulated cache hits 0 percent. The breathing phase and the clips run on continuous
time (the life's `nz @t` noise included), so no two frames have the same world matrices bit for bit.

### Experiments 1 and 2: the creature's cells kept

The raster now writes the creature's own cells into a layer before the world goes round them: per cell its dots, its
halo's dots, a flag where its eyes are, its colour (7 bytes a cell). The world's pass (floor, dust, sky, weather) reads
only that layer, never the dots. With `skip`, a pose equal to the last one keeps the layer; with `cache`, a pose in the
cache uses the cached layer; either way the world is made over it as ever.

- Identity: host test `lab_test.c` compares, every frame, the cells, colours and creature mask of a raster-only cat with
  a `skip` cat and a `cache` cat in step (idle life and two gestures), and with poses forced to repeat (a second clip 20
  frames behind the first drawn into the same cache: 900 hits, 0 differences; a pose drawn twice, the second kept: 900
  kept, 0 differences). The layer refactor itself was checked against the old raster bit for bit over 9,000 frames with
  sky, rain, a wet floor and its mirror (0 cells differ).
- Host cost: a hit costs 0.07 of a raster (the world's part stays); a miss with both on costs 1.00 to 1.01 of a raster
  (the key, 0.6 us here, and the copy of 14.9 KB into the cache). The default path is 0.98 of what it was.
- Since the cat's poses never repeat (above), neither experiment can save anything on the device as the cat is built
  today. The device run says so for certain (`lab pose`, `lab reuse`).

### Costs

- Internal RAM: 12 bytes (the rings' pointers and the summaries' lock). Everything else is in PSRAM.
- PSRAM: 208 bytes always (the lab's state), plus 4.2 KB in every rigged creature (its layer is 14.9 KB, the scratch it
  replaced was 10.6 KB); `rec` 90 KB, `pose` 43 KB, `cache` 14.9 KB a pose, each only while its knob is on. The lab's
  task (made at the first POST) has its stack (3.5 KB) and control block in PSRAM.
- HTTP routes: 51 of 56 (52 with DIAG_RING).
- Per frame with every knob at default: two function calls that return at once and one that compares four values.
  With `rec`: the record (a few dozen stores) and the signature (FNV over the shapes and the 2,124 dots, about 1,000
  words); with `pose`, `skip` or `cache`: the pose key (about 520 words). Host: 1.7 us a frame for a scene with 64
  shapes, 0.6 us for the key; expected well under 0.2 ms on the device, where the `lab` column measures it.

## Stage 2: designs (not built)

Each hangs off a knob, default today's behaviour, and is judged by the Stage 1 counters plus its own.

### 1. The sparse cells pass

Goal: the drawer stops visiting all 2,124 cells (`fluid_draw_cells`, fluid_draw.c, about 11 ms calm) and visits only
rows a producer says may have changed. Target: calm cells p95 at most 4.5 ms.

- Knob `sparse`: 0 the full pass (default), 1 rows from the producers, 2 check (both passes; the full one's result
  used; every cell where they disagree counted).
- Producers, each a 64-bit row mask (36 rows) in `fluid_draw_opts_t`, ORed:
  - the scene's shapes: the boxes `stale_shape` already marks (hch), old and new, a halo cell wider;
  - the scene's dots: a row whose 59 bytes differ from the frame before's (memcmp in the handoff copy, mole_fluid.c);
  - the fine dust: the rows where `ff->sig` moved (fluid_fine.c knows them as it writes them);
  - the water: a row where any cell is not `hd_still`, and for 2 frames after it calms (the sim's height step can
    mark rows whose slope passes half a dot, with hysteresis);
  - the letters, the caption box, the hold ring, overlays and the owed cells (their rows);
  - anything global (force, a palette change, a change of picture, the experimental dust kinds): all rows.
- Files: fluid_draw.c (the row loop skips clean rows; `span` and `why` cleared for them), fluid_sim.c (water rows),
  fluid_fine.c (dust rows), mole_fluid.c (dots rows, the knob). New: a host test that plays the scene scripts with
  `sparse` 2 and wants zero disagreements.
- Counters: rows visited a frame, cells visited, disagreements (mode 2), the `cells` stage.
- Risk: a producer missed leaves a cell stale for good (the worst kind of bug: a picture that looks almost right).
  Mode 2 on the device and in the host scripts is the guard; ship mode 1 only after zero disagreements.
- Gate (review): exact reference pixels across enter, interrupt, resume and erase; calm cells p95 at most 4.5 ms; for a
  60 candidate, each core's critical path p99 at most 14 ms on even and odd refreshes.

### 2. The hand lane

Goal: a scene's hand (the rhythm clock's) on every tearing pulse, 59.65 positions a second, over a background that
stays at 29.83.

- Knob `hand`: 0 off (default), 1 the lane with its governor, 2 the lane forced on (to measure its failures).
- How: the fluid task wakes on every pulse. Even pulses are today's frame, drawn without the hand. Odd pulses evaluate
  only the hand, from the beat clock at the time its rows will be scanned (the next pulse plus its rows' share of the
  period plus the measured picture delay), and send it: the union of its old and new box, cut into thin windows along
  the line, copied from the kept glass (which holds the background without the hand) with the hand composited into the
  window buffer. The cells pass is not run on an odd pulse.
- Files: new fluid/hand_lane.c (evaluate, cut windows, composite, governor); scene_player.c gets a call that gives the
  hand's shape at a time (the beat item's hand only, not the wave item); fluid_draw.c a hook that leaves the hand out of
  the glass and sends a hand-only frame; mole_fluid.c the every-pulse wake behind the knob and the picture delay
  (`frame_us` 33400 to 16700 while the lane runs).
- Governor: eligible when the scene has a hand and no change of picture runs. Enter 60 after 30 consecutive eligible
  samples whose predicted critical path (cells + send on core 0, sim + dust on core 1) is under 14 ms. Return to a
  clean 30 (even pulses only) at once on a predicted overrun or when an odd frame is not ready by its pulse; never
  judder between the two inside one second.
- Counters: lane state, entries and exits, odd and even critical paths apart, hand positions completed a second and the
  longest hold (the Stage 1 tracked item with `track` set to the hand's shape), three-pulse gaps, torn hands (a window
  written while the scan line is inside it, from the pulse's time and the row), debt (cells owed growing).
- Risk: tearing (a write racing the scan), the even frame's heavier background missing its pulse, and audio sync
  (the picture-delay estimator).
- Gate (review): at least 99 percent of eligible pulses show the correct new hand position; zero three-pulse gaps, torn
  hands or growing debt; enter after 30 samples under 14 ms, back to 30 on predicted overrun.

### 3. Baked gesture deltas

Goal: a gesture clip's creature cells made offline by the package build tool, so playing it reads run-length deltas
instead of rasterising. Measurement 1 says live poses never repeat, so this is the creature's remaining path to a
cheap frame; it only works where playback is deterministic.

- Knob `baked`: 0 the raster (default), 1 a baked clip plays from its deltas when its conditions hold, else the raster.
- Offline (packages/cat/build.mjs, the package's own builder): for each chosen gesture at a fixed camera and heading,
  play it at 30 fps with the life layers held at their gesture weights, render the creature layer per frame (the same
  7 bytes a cell as Stage 1's, from the firmware raster built natively or the JS reference), and write a keyframe every
  K frames plus run-length deltas of changed cells; a raw fallback frame when a delta grows past a raw one.
- Device: creature_dots.c reads the deltas into the live layer (the world pass as ever); a frame plays baked only when
  the clip, its frame index and the camera match what was baked, else the raster.
- Files: packages/cat/build.mjs (the baker), the package format (a new field per clip), creature_clip.c (frame index
  snapped to the baked grid while baked), creature_dots.c (the reader).
- Counters: baked frames, raster fallbacks and why, delta bytes read a frame, the reader's time, the Stage 1 creature
  stage times.
- Risk: the look (the gesture's life layers and breathing frozen to the baked grid, an EYE decision for the owner),
  depth, halo and outline shading where eyes or tail overlap the body must match the raster, and the package's size
  (estimate first: a delta of a few hundred cells is a few KB a frame).
- Gate (review): exact reference pixels across enter, interrupt, resume and erase; replay cost measured with audio and
  DMA active; worst case and hit streaks recorded; for a 60 candidate, critical path p99 at most 14 ms on both
  refreshes.

## Operator's procedures

Set the device's base URL once: `D=http://molty.local` (or `http://10.13.37.1` over the cable). Change one knob at a
time; reset between runs.

Baseline (every run starts here):

```sh
D=http://molty.local
curl -fsS -X POST "$D/api/lab" -d '{"reset":1}'
curl -fsS -X POST "$D/api/lab" -d '{"rec":1}'
# leave it 40 s in the state to measure (idle cat, a gesture, the waves scene, the clock)
curl -fsS "$D/api/lab" | jq '.frames'
curl -fsS "$D/api/lab?raw=1022" > frames.csv
curl -fsS "$D/api/log" | grep ' lab '
```

Measurement 1, the cat's poses:

```sh
curl -fsS -X POST "$D/api/lab" -d '{"reset":1}'
curl -fsS -X POST "$D/api/lab" -d '{"pose":1}'
# 2 minutes idle (no touch, no agent), then read
curl -fsS "$D/api/lab" | jq '.pose'
# gestures: one every 10 s for a minute, then read again
for g in nod stretch yawn wiggle bounce peek; do curl -fsS -X POST "$D/api/creature/state" -d "{\"act\":\"$g\"}" >/dev/null; sleep 10; done
curl -fsS "$D/api/lab" | jq '.pose'
```

Read `same`, `distinct_per_min`, `top` and `lru_hit`. A cell cache can only help if `lru_hit` for some size is well
above zero; the host says it will be 0.

Experiments 1 and 2 (only worth running if measurement 1 found repeats):

```sh
curl -fsS -X POST "$D/api/lab" -d '{"reset":1}'
curl -fsS -X POST "$D/api/lab" -d '{"rec":1,"skip":1}'                 # experiment 1
curl -fsS -X POST "$D/api/lab" -d '{"rec":1,"cache":1,"cache_n":32}'   # experiment 2 (after a reset)
# the same 2 minutes as the baseline, then
curl -fsS "$D/api/lab" | jq '.creature, .frames.sim, .frames.cells'
```

Better means: `creature.same.frames` or `creature.cached.frames` a large share of all, `saved_ms_*` positive, and
`frames.sim` p95 lower than the baseline's with the same `frames.wake_pulses`.

The tracked item in a scene (the clock's hand, the waves' outline): play the scene, `{"rec":1}`, read
`frames.tracked`; to follow one shape, set `track` to its index in `frames.tracked.shapes` (a letter a shape: s segment,
r ring, d disc, m mask, c char), e.g. `curl -fsS -X POST "$D/api/lab" -d '{"track":3}'`. Better means `done_s` near 29.8 (59.6 with the hand
lane) and `hold_pulses` 2 (1 with the lane).
