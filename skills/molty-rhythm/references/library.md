# The rhythm library

One file per rhythm in `rhythms/`, hand-written JSON. Steps are numbered 1 to `steps` round the
clock, step 1 at 12 o'clock, clockwise. Nothing in a file is invented: no source, no file.

| field | meaning |
|---|---|
| `id`, `name`, `label` | the file's name; the spoken name; the name on Molty (capitals, up to 12) |
| `status` | `verified` (two agreeing sources, or a primary source with clear corroboration) or `unverified` |
| `steps` | steps in one turn of the clock (one bar, or two for a two-bar pattern), up to 32 |
| `lengths` | optional, one number a step: uneven steps (aksak 9/8 is `[2, 2, 2, 3]`). Default all 1 |
| `swing` | optional 0 to 0.33: every second step starts this much of a step late (0.33 a shuffle) |
| `tempo` | `{bpm, per, range?}`: beats a minute, one beat being `per` units; `range` only when a source states it |
| `beats` | the steps that start a felt beat: the long marks, the count-in clicks, the buzz |
| `layers` | one per drum: `{name, sound, hits, accent?, color?}`; `sound` is `low`, `head`, `rim`, `clap`, `bell`, `hat`, `snare` or `click` |
| `bars` | optional variations: `{"B": {"only": ["parche"]}}`, or a layer with its own `hits` |
| `phrase` | optional: `{order: ["A", "A", "B", "A"], count_in?: 0..2}`: bars in an order, on repeat |
| `source` | where the pattern comes from; a tempo not from a source says "a practice default" |

`scripts/library.mjs` checks every file each time the script runs and turns it into what Molty
takes (`rhythm.play`'s `rhythms`, [commands.md](commands.md)).

## What ships

All twelve are `verified`:

| rhythm | drums | sources |
|---|---|---|
| chacarera | aro (rim) 1 4, parche (head) 3 5, accent 5 | Marti 2018 via Paredes 2025 fig. 5; Tajan (FBA-UNLP); tempo 100 to 112 from Mola's corpus |
| chacarera trunca | aro 1 4 8, parche 3 5 9 11, accents 9 11 | Diaz Capello 2024 p. 35; form in Mola and Paredes |
| carnavalito | bombo 1 3 4 5 7 8, accents 1 4 5 8 | Diaz Capello 2024 p. 39; Clang (FBA-UNLP) |
| milonga campera | bass 1 4 7 (3+3+2) | Garcia Brunelli 2008 via Martinez; tangoindepth |
| tango marcato | marcato 1 2 3 4, accents 1 3 | Peralta 2008 via Martinez |
| habanera | bass 1 4 5 7 | Toussaint 2005; tangoindepth |
| son clave 3-2 | clave 1 4 7 11 13 of 16 | Wikipedia Clave; Absil 2004 |
| rumba clave 3-2 | clave 1 4 8 11 13 of 16 | Wikipedia Clave; Absil 2004 |
| bossa nova | clave 1 4 7 11 14 of 16 | Wikipedia Clave; Toussaint 2005 |
| the standard bell | bell 1 3 5 6 8 10 12 of 12, pulse 1 4 7 10 | Wikipedia Bell pattern citing Toussaint and A. M. Jones |
| solea | palmas on 3 6 8 10 12 of 12 | Chromatone citing Juan Martin; Gongora 2024 (UPM) |
| aksak 9/8 | four uneven beats 2+2+2+3 | Toussaint 2005; Wikipedia Aksak citing Brailoiu 1951 |

The chacarera's phrase (A A B A, B the parche alone) is a practice order, not a traditional form.
Tempos without a source are practice defaults; say so if asked.

## Left out

Verified but not shipped, so the wheel holds twelve, 30 degrees apart: bulerias, reggae one drop,
the 7/8 racenica, the tresillo and cinquillo, and the gato and chacarera doble (the chacarera's own
pattern). Each can be added as a file. Found with one source only, or read from notation too blurry
to trust: zamba, vidala, cueca, chamame, tango sincopa, candombe, cascara, samba, baiao, rock
backbeat, shuffle, waltz. Named but never transcribed: escondido, malambo, bailecito, maracatu,
tangos flamencos, sevillanas. A new file needs two agreeing sources to be `verified`.
