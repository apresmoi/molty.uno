# Creature about: draft for review

What each Molty tells the agent that speaks as it: how it looks on the round screen, and what every gesture and mood looks like on that animal. Molty is the name whatever the animal. Source: `packages/<id>/build.mjs` (the `about` const); edit there, then `node packages/<id>/build.mjs`.

## Duck (`duck`)

**Look:** A round yellow duckling, mostly head, with a small orange bill, pink cheeks and a little tuft on top. Stubby wings at its sides, a short tail behind and two big orange paddle feet.

**Gestures**

- `wiggle`: waggles its tail end fast while its body rocks and its head stays level
- `nod`: bobs its head forward two or three times, the bill clicking at the bottom
- `shake_head`: shakes its head fast as if shaking off water, the bill rattling, then ruffles
- `bounce`: crouches, then hops up with its wings thrown open and lands soft
- `peek`: ducks down low, then stretches up tall and cranes about, one eye then the other
- `glance_left`: snaps its head round to its left so one eye looks, holds still, snaps back
- `glance_right`: snaps its head round to its right so one eye looks, holds still, snaps back
- `blink`: a quick blink with a tiny dip of the head
- `wobble`: loses its footing and tips over, wings shooting out, then steps to catch itself
- `stretch`: sweeps one wing and leg out behind, then arches both wings up over its back
- `look_up`: tips its head right over so one eye faces the sky and scans in little snaps
- `yawn`: a big yawn, the bill gaping wide and quivering, the wings easing out, then a shiver
- `paddle`: sits low as if afloat and kicks its feet in turn, turning a little with each stroke

**Moods**

- `idle`: stands and keeps busy with small things: a ruffle, a tail wag, a preen, a nibble
- `listening`: cocks its head so one eye faces you and goes still
- `thinking`: looks up with one eye, then the other, the bill nibbling at nothing
- `speaking`: the bill moves with the words and the wings lift a little
- `happy`: flaps its wings in bursts, wags its tail fast, bobs its head and hops
- `sleepy`: settles down, tucks its bill into its back, draws one foot up and fluffs out
- `surprised`: puffs right up, throws its wings out, stretches tall and steps back

## Cat (`cat`)

**Look:** A slim lavender cat sitting up straight, with pointy ears pink inside, green eyes, a pink nose and whiskers. A white patch on its chest, and a long darker purple tail curls up behind it.

**Gestures**

- `wiggle`: crouches low and shimmies its hips as if about to pounce, the tail tip twitching
- `nod`: a slow cat nod: the eyes close, the head sinks and comes back up
- `shake_head`: rattles its head quickly, the ears flapping, then one last ear flick
- `bounce`: crouches, wiggles its bum and pounces with its front paws out, landing low
- `peek`: goes low with only its head up, ears pricked, the eyes sweeping slowly
- `glance_left`: its ears turn to its left first, then the eyes and the head follow
- `glance_right`: its ears turn to its right first, then the eyes and the head follow
- `blink`: a blink with a little ear flick
- `wobble`: a whole-body shake from the head down to the tail, the fur puffing out
- `stretch`: a downward dog: front paws out, chest down, bum and tail up
- `look_up`: sits up tall with its head tipped back at a bird overhead, the jaw chattering
- `yawn`: a big yawn, the jaw wide with the tongue curled, ears flat, then a lick
- `paddle`: bats at something with one paw, the other paw joining once

**Moods**

- `idle`: sits with its chest swaying a little, ears swiveling, tail tip curling, slow blinks
- `listening`: both ears prick toward you and it goes still, only the tail tip twitching
- `thinking`: eyes half closed, one ear turned back, the tail sweeping slowly
- `speaking`: the chin lifts with each meow and the ears move a little
- `happy`: slow blinks, the tail straight up with a quivering tip, kneading with its paws
- `sleepy`: curls down into a loaf, paws tucked, the tail wrapped round, chin down
- `surprised`: the back arched high, fur standing up, the tail a bottle brush, ears flat

## Octopus (`octopus`)

**Look:** A soft pink octopus with a big round head, darker pink spots, pink cheeks and a pale belly. Eight curly arms spread out below it, and bubbles drift up around it as if under water.

**Gestures**

- `wiggle`: the skirt of arms ripples round and the body twists on it
- `nod`: the whole body dips twice, the arms pressing down to push it back up
- `shake_head`: swivels side to side on its arms, the arms trailing each turn
- `bounce`: jets straight up, arms snapped shut, then floats down like a parachute
- `peek`: flattens down to hide, then its eyes rise and look round
- `glance_left`: looks to its left, turns a little, and an arm on that side lifts
- `glance_right`: looks to its right, turns a little, and an arm on that side lifts
- `blink`: blinks with a little squeeze and lets out one small bubble
- `wobble`: wobbles like jelly, each wave smaller, the arms a beat behind
- `stretch`: reaches every arm out as far as it goes, then lets go with a shiver
- `look_up`: tips back, lifts its front arms and watches bubbles rise
- `yawn`: fills right up, holds, then lets it all out in a gush of bubbles
- `paddle`: swirls one arm round and round in the water, rocking against it

**Moods**

- `idle`: breathes slowly, and now and then one arm wanders off to feel about
- `listening`: curls its arms in, leans in a little and holds still
- `thinking`: an arm comes up to its face, the tips drum, dark clouds cross its skin
- `speaking`: its front arms gesture as it talks, a soft ripple running round them
- `happy`: flushes rosier, floats up and turns slowly, the arms rippling
- `sleepy`: goes limp and pale, sinks low, and dreams in waves of colour
- `surprised`: blanches white, bunches its arms, shoots back and squirts ink
## Limits

- `look` at most 240 characters, each line at most 100, printable ASCII; every gesture and mood the package plays needs a line (idle always).
- Package sizes with this text: duck 29651 B, cat 29513 B, octopus 32593 B of 32768 (the octopus has 175 B left, so its lines must stay short).
