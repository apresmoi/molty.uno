# The alive rig

Scores how alive a Molty looks, so each change to its life (docs/alive-proposal.html, sections 9 and 10) is
measured against the one before. It plays the Molty through the firmware's own drawing code (the web module,
`device/firmware/web/built/molty.wasm`, through the website's loader), in fixed scenes with a fixed seed, and
writes pictures and numbers.

    make alive-rig                          # the cat as shipped (packages/cat/cat.json)
    make alive-rig MOLTY=path/to/molty.json # another Molty
    node tools/alive-rig/alive-rig.mjs [--molty <file>] [--out <dir>] [--scene <name>]
    make alive-rig-test                     # the numbers on made-up input

Output, in `tools/alive-rig/out/` (not committed): `<scene>.png` (the contact sheet), `numbers.json`,
`table.txt`, `cost.json`. Two runs write the same `numbers.json`, `table.txt` and sheets, byte for byte.
`cost.json` is apart because it is not: see Cost.

## The scenes

Each scene is a step script in the fidelity script's format (`device/firmware/web/fidelity.script`): the
creature, power on with seed 20260516 and no boot screen, 3 s to settle (with the scene's mood already
set), then the scene. 30 frames a second. Only the scene is scored.

| scene | steps | what today cannot do |
| --- | --- | --- |
| idle | 60 s, nobody there | |
| alone | 360 s, nobody there: it yawns and dozes off at 5 min | sped up only in its sheet (a frame every 3 s): the firmware's 5 min cannot be shortened without changing it |
| listening | mood listening, a listen, 20 s of a made up loudness track (100 ms steps, phrases and pauses), the listen ends | the listening reactions (a nod at a pause, a glance at a question) live in `link/listen_react.c`, not in the module: no pause reaction is scored |
| thinking | mood thinking, 60 s | |
| working | mood working (the agent busy), 60 s | |
| speaking | mood speaking, 15 s | no step feeds a voice to the creature yet (slice 2): no lip match |
| touched | a tap on the cat every 7 s (4 taps, 100 ms each), a shake at 35 s, 7 s more | |

## What it records each frame

- **The picture**: the creature's dots alone (`wm_cat_pixels`: the panel's pixels where its own dots are, the
  water and dust left out). For the numbers it is cut into the water's cells (59 x 36 on the 466 px panel),
  each cell's value its mean brightness, (R + G + B) / 3 over its pixels.
- **The changed share**: of the picture's pixels lit in this frame or the one before, the share whose colour
  differs between the two.
- **The bones**: each node's world matrix (`wm_bones`, the one call added to the web glue for this tool; read
  only).

## The numbers

**Loop echo.** How alike the picture is to itself a while later. For a lag L between 1 s and 30 s (at most half
the scene): d(t, L) = the distance between frame t's picture and frame t + L's, sqrt(sum over cells of the
difference squared). m(L) = the median of d(t, L) over the scene; ref = the median of d over every t and every
lag. echo(L) = 1 - m(L) / ref. 1: the typical moment comes back exactly after L; about 0: no more alike than any
two moments. Medians, so one big gesture does not drown the small loop under it (a correlation weighted by
energy did: one look up swamped the 4 s loop and put the peak at 2 s). Reported: the strongest echo, its lag (the
shortest if several tie), the standout (the strongest minus the median over all lags: a lag that stands out
scores high, noise scores about 0) and the echo at exactly 4.0 s.

**Still share.** Of the scene's frames after the first, the share whose changed share is under 1 %.

**Rhythm spread.** The changed share smoothed over 6 frames (0.2 s: the mean of this frame and the five
before). A burst starts where it rises to the scene's mean of it; the next can start only after it has fallen
under 0.7 of that mean. Spread = the standard deviation of the gaps between burst starts over their mean (null
under 3 gaps). 0: a beat like a clock.

**Body spread.** The share of the motion in each part: head (neck, head, mouth, tongue), eyes, ears, tail, body
(the rest; the root and props such as the bowl are in none). A bone's motion in a frame: take its little frame
(its origin and the tips of its three axes 0.1 world units out), seen from its anchor (the nearest node above it
that is in another part, or the root, so a part's own motion counts, not what it is carried by), and average
how far the four points moved since the frame before. A part's motion: the sum over the scene of its bones'
motion, divided by its number of bones (a 12 bone tail does not weigh more than two eyes). Share = a part's
motion over all parts'.

**Reaction delay** (touched). For each tap and the shake, the same steps are played again without that event and
what follows it. The delay is the time from the event to the end of the first frame where the creature's
picture differs from that twin (`picture_ms`, in the table), and the same for its bones (`bones_ms`). One frame
is 33 ms.

**Pops.** Frames where some bone's motion (as above) is at least 0.015 world units (about a braille dot) and at
least 4 times its motion in the frame before: a jump out of nowhere, where a move that eases in grows frame by
frame. Frames in the 0.5 s after a scripted step (a tap, a shake, a listen) are not counted.

**Cost.** The module's device mode (`wm_device`, the simulator's): each frame costed on the panel's measured
constants (`web_device.c`): the drawer's cells pass (a fixed 11 ms), the windows and pixels sent, against the
sim task's 6.2 ms, with the 30 ms frame floor. Reported: mean and max of the frame and of the drawer, in ms.
Two cautions: in device mode the firmware's clock runs with the computer's (`web_sdk.c`), so these move a
little between runs (they are kept out of the byte for byte promise); and the creature's own work per frame
(its clips, layers, dots) is not in that model, so it cannot show a 1 ms change in the creature's work. That
needs the device's own frame profile.
