# The rhythm library

One file per rhythm in `rhythms/`, hand-written JSON. Steps are numbered 1 to `steps` round the
clock, step 1 at 12 o'clock, clockwise. Nothing in a file is invented: no source, no file.

| field | meaning |
|---|---|
| `id`, `name`, `label` | the file's name; the spoken name; the name on Molty (capitals, up to 12) |
| `status` | `verified` (two agreeing sources, or a primary source with clear corroboration) or `unverified` |
| `region`, `country` | the region on Molty's wheel (`scripts/library.mjs` `REGIONS`) and where the rhythm comes from |
| `steps` | steps in one turn of the clock (one bar, or two for a two-bar pattern), up to 32 |
| `lengths` | optional, one number a step: uneven steps (aksak 9/8 is `[2, 2, 2, 3]`). Default all 1 |
| `swing` | optional 0 to 0.33: every second step starts this much of a step late (0.33 a shuffle) |
| `tempo` | `{bpm, per, range?}`: beats a minute, one beat being `per` units; `range` only when a source states it |
| `beats` | the steps that start a felt beat: the long marks, the count-in clicks, the buzz |
| `layers` | one per drum: `{name, sound, hits, accent?}`; `sound` is `low`, `head`, `tom`, `slap`, `snare`, `rim`, `hat`, `bell`, `shaker`, `clap` or `click`; its colour comes from its sound (below) |
| `bars` | optional variations: `{"B": {"only": ["parche"]}}`, or a layer with its own `hits` |
| `phrase` | optional: `{order: ["A", "A", "B", "A"], count_in?: 0..2}`: bars in an order, on repeat |
| `source` | where the pattern comes from, in one line; a tempo not from a source says "a practice default" |
| `sources` | `[{title, url, shows}]`: each source and what exactly it shows; two that agree for `verified` |

`scripts/library.mjs` checks every file each time the script runs and turns it into what Molty
takes (`rhythm.play`'s `rhythms`, [commands.md](commands.md)).

## What ships

All 67 are `verified`: each pattern below is given the same way by two independent sources (the
`sources` of its file say exactly what each shows, with its link). They are in 12 regions, the order Molty's wheel
steps through them. Tempos without a source are practice defaults; say so if asked. The
chacarera's phrase (A A B A, B the parche alone) is a practice order, not a traditional form. Gato,
escondido and chacarera doble share the chacarera's bombo (the sources say so); rasguido doble
shares the milonga's 3+3+2.

### Argentina (15)

| rhythm | from | grid | drums | sources |
|---|---|---|---|---|
| Chacarera | Argentina | 6 steps | aro (rim) 1 4; parche (head) 3 5, accent 5 | Marti 2018 via Paredes 2025 fig. 5; Tajan (FBA-UNLP) points 2 to 4; tempo Mola (FBA-UNLP) corpus |
| Zamba | Argentina | 12 steps | aro (rim) 1 7; parche (head) 4 5 9 | Mola, Bombo leguero: una propuesta metodologica para la ensenanza de s; Paredes, tesina de Licenciatura en Musica Popular (UNCuyo 2025), fig.  |
| Chamamé | Argentina | 6 steps | bajo (low) 1 3; rasguido (rim) 2 5 | Gimenez, El acordeon del Litoral argentino. Un analisis de los distint; Romero Garcia, Chacarera, zamba y chamame: tres generos ... y su inter |
| Gato | Argentina | 6 steps | aro (rim) 1 4; parche (head) 3 5, accent 5 | Pablo Cruz, El bombo leguero en el folklore argentino, Vol. I (method ; Albarino and Balut, Proyecto Bombo: fundamentacion (Laboratorio de Son; Mola, Bombo leguero: una propuesta metodologica para la ensenanza de s |
| Escondido | Argentina | 6 steps | aro (rim) 1 4; parche (head) 3 5, accent 5 | Albarino and Balut, Proyecto Bombo: fundamentacion (Laboratorio de Son; Mola, Bombo leguero: una propuesta metodologica para la ensenanza de s |
| Cueca | Argentina | 6 steps | bajo (low) 1 3 5, accent 5; chasquido (rim) 4 | Octavio Sanchez, La cueca cuyana contemporanea. Identidades sonora y s; Fabian Duran B., Repertorio musical criollo latinoamericano: usos y pr |
| Chacarera doble | Argentina | 6 steps | aro (rim) 1 4; parche (head) 3 5, accent 5 | Paredes, tesina de Licenciatura en Musica Popular (UNCuyo 2025); Mola, Analisis del aspecto ritmico en la melodia y en el acompanamient |
| Chacarera trunca | Argentina | 12 steps | aro (rim) 1 4 8; parche (head) 3 5 9 11, accent 9 11 | Diaz Capello 2024 p. 35 ex. 1; form in Mola (FBA-UNLP) and Paredes 2025 p. 29; tempo a practice default |
| Carnavalito | Argentina | 8 steps | bombo (head) 1 3 4 5 7 8, accent 1 4 5 8 | Diaz Capello 2024 p. 39 ex. 2; Clang (FBA-UNLP) on the huayno figure; tempo a practice default |
| Vidala | Argentina | 6 steps | aro (rim) 4; parche (head) 1 5 | Mola, Bombo leguero: una propuesta metodologica para la ensenanza de s; Paredes, tesina de Licenciatura en Musica Popular (UNCuyo 2025), fig.  |
| Rasguido doble | Argentina | 8 steps | bajo (low) 1 4 7 | Adriani & Schinca, Rasguido doble: nuevas texturas y sonoridades (FBA-; Rasguido doble, Spanish Wikipedia, section 'Caracteristicas del rasgui |
| Milonga campera | Argentina | 8 steps | bass (low) 1 4 7 | Garcia Brunelli 2008 p. 21 via Martinez (UNCuyo) 2.1.9; tangoindepth 2021 (3+3+2); tempo a practice default |
| Tango marcato | Argentina | 4 steps | marcato (low) 1 2 3 4, accent 1 3 | Peralta 2008 p. 56 via Martinez (UNCuyo) 2.1.3 (marcato en 2); tempo a practice default |
| Tango sincopa | Argentina | 8 steps | bass (low) 1 2 4 5, accent 1 | Camilo Martinez, tesis on tango drumming (UNCuyo), 2.1.8.1 Sincopa a t; Iris Marcipar, DMA on tango double bass (University of Georgia), fig.  |
| Vals criollo | Argentina | 3 steps | bajo (low) 1; rasguido (rim) 2 3 | Silvina Arguello, El vals en la musica criolla y en el tango, Revista ; El vals peruano: devenir historico y formas de toque en la guitarra ac |

### Latin America (beyond Argentina) (2)

| rhythm | from | grid | drums | sources |
|---|---|---|---|---|
| Candombe | Uruguay | 16 steps | chico (head) 2 3 4 6 7 8 10 11 12 14 15 16, accent 2 6 10 14; madera (rim) 1 4 7 11 13 | Wikipedia (es), Tambores de candombe: Chico; Paredes, tesina (UNCuyo 2025), fig. 15 and 18 after Luis Ferreira; Martin Rocamora, PhD thesis page on candombe (FING, UdelaR); Anibal Gonzalez, Patron de acompanamiento de candombe (agswingmusica 2 |
| Cumbia | Colombia | 4 steps | tambora (low) 1 3; llamador (head) 2 4; guache (shaker) 1 2 3 4 | Burgos Palomino, J. A. (2017), Berroche, trabajo de grado, Universidad; Orphiq, What Is Cumbia? Origins, Rhythm, and Regional Styles |

### The Caribbean (9)

| rhythm | from | grid | drums | sources |
|---|---|---|---|---|
| Son clave | Cuba | 16 steps | clave (rim) 1 4 7 11 13 | Wikipedia Clave (rhythm); Absil 2004 table 1; 3-2; tempo a practice default |
| Rumba clave | Cuba | 16 steps | clave (rim) 1 4 8 11 13 | Wikipedia Clave (rhythm); Absil 2004 table 1; 3-2; tempo a practice default |
| Tresillo | Cuba | 8 steps | tresillo (low) 1 4 7, accent 1 | Toussaint, The Euclidean Algorithm Generates Traditional Musical Rhyth; Wikipedia, Tresillo (rhythm), citing Penalosa, The Clave Matrix (2010)) |
| Cinquillo | Cuba | 8 steps | cinquillo (rim) 1 3 4 6 7, accent 1 | Toussaint, The Euclidean Algorithm Generates Traditional Musical Rhyth; Wikipedia, Cinquillo |
| Habanera | Cuba | 8 steps | bass (low) 1 4 5 7 | Toussaint 2005 fig. 1a (tresillo); tangoindepth 2021 (habanera 3+1+2+2); tempo a practice default |
| Conga tumbao | Cuba | 8 steps | heel (head) 1 5; tip (click) 2 4 6; slap (slap) 3; open (tom) 7 8 | Wikipedia, Tumbao (conga section, citing Mauleon 1993 p.63-64); Thomann Online Expert, Congas: First Steps |
| One drop | Jamaica | 8 steps | hat (hat) 1 2 3 4 5 6 7 8; kick (low) 5, accent 5; rim (rim) 5, accent 5 | Wikipedia, One drop rhythm (Strong, Drums For Dummies 2011 p.163); MusicRadar, How to program a typical one drop reggae beat; Soundbrenner, One drop |
| Ska | Jamaica | 8 steps | skank (click) 2 4 6 8 | Wikipedia, Ska; Soundbrenner, Ska rhythm |
| Dembow | Puerto Rico | 16 steps | kick (low) 1 5 9 13; snare (snare) 4 7 12 15 | MusicRadar, Beat Building: how to make a reggaeton beat; Orphiq, Dembow Rhythm Explained; UJAM, How to make a dembow groove |

### Brazil (3)

| rhythm | from | grid | drums | sources |
|---|---|---|---|---|
| Bossa nova | Brazil | 16 steps | clave (rim) 1 4 7 11 14 | Wikipedia Clave (rhythm); Toussaint 2005 (E(5,16) from its third onset); tempo a practice default |
| Samba | Brazil | 4 steps | segunda (head) 1; primeira (low) 3, accent 3 | Wikipedia, Surdo (Rio-style Carnival samba, citing Fogo Azul NYC); Hudson Music, The Brazilian Groove Book (sample), Part 1 Samba Layers; Brazilian Percussion blog, Heavy samba percussion instruments |
| Baiao | Brazil | 8 steps | zabumba (low) 1 4, accent 1; triangle (hat) 1 2 3 4 5 6 7 8 | Galvao, C. (2018), Complexizacao do baiao na bateria, ANPPOM XXVIII; John Patitucci, Electric Bass Complete: Baiao (LinkedIn Learning); Cortes, A. (2014), Como se toca o baiao, Per Musi 29 p.195-208 |

### Spain, flamenco (4)

| rhythm | from | grid | drums | sources |
|---|---|---|---|---|
| Solea | Spain | 12 steps | palmas (clap) 3 6 8 10 12 | Chromatone citing Juan Martin; Gongora 2024 (UPM) alegrias 3-6-8-10-12; tempo range a single source (alegrias) |
| Buleria | Spain | 12 steps | palmas (clap) 3 7 8 10 12 | El Compas Flamenco: A Phylogenetic Analysis (Bridges 2004); Bulerias, Wikipedia |
| Seguiriya | Spain | 12 steps | palmas (clap) 1 3 5 8 11 | El Compas Flamenco: A Phylogenetic Analysis (Diaz-Banez, Farigu, Gomez; Palo (flamenco), Wikipedia); Compas exercises (canteytoque) |
| Fandango de Huelva | Spain | 12 steps | palmas (clap) 1 4 7 10 | El Compas Flamenco: A Phylogenetic Analysis (Bridges 2004); Chromatone, Flamenco compas; El compas y la sonoridad flamenca (Berlanga, UGR) |

### West Africa (5)

| rhythm | from | grid | drums | sources |
|---|---|---|---|---|
| The standard bell | Ghana | 12 steps | bell (bell) 1 3 5 6 8 10 12; pulse (low) 1 4 7 10 | Wikipedia Bell pattern citing Toussaint 2005 and A. M. Jones; tempo a practice default |
| Gahu | Ghana | 16 steps | bell (bell) 1 4 7 9 12 15; axatse (shaker) 1 4 5 7 9 12 13 15; sogo (head) 1 5 7 9 13 15, accent 5 13 | Mark Powers, GAHU: Part One (2006); Andy Schloss, MUS 320 study guide / Quiz 1, University of Victoria |
| Ewe bell and rattle | Ghana | 12 steps | bell (bell) 1 3 5 6 8 10 12; pa (shaker) 1 3 5 6 8 10 12; ti (click) 4 7 9 11 | Wikipedia: Ewe drumming, Standard bell pattern and accompanying axatse; Jeff Pressing, Rhythmic design in the support drums of Agbadza, Africa |
| Kuku | Guinea | 32 steps | dununba (low) 4 13 15 17 20; sangban (head) 1 4 7 9 12 15 17 20 23 25 28 31, accent 1 7 9 15 17 23 25 31; kenkeni (tom) 1 2 5 6 9 10 13 14 17 18 21 22 25 26 29 30; bell (bell) 1 3 4 6 7 9 11 12 14 15 17 19 20 22 23 25 27 28 30 31 | KUKU (reynders-bonhagen.nl rhythm pages); Malcolm Smith, Kuku (thesmith.org.uk) |
| Soli | Guinea | 12 steps | dununba (low) 1 3 9 10 12; sangban (head) 1 5 7 10, accent 1 10; kenkeni (tom) 5 6 11 12; bell (bell) 2 3 5 6 8 9 11 12 | SOLI, taught by Mamady Keita 1997, transcribed by Rodger Osgood (reynd; Malcolm Smith, Soli (thesmith.org.uk) |

### North Africa and the Middle East (7)

| rhythm | from | grid | drums | sources |
|---|---|---|---|---|
| Maqsum | Egypt | 8 steps | dum (low) 1 5, accent 1; tek (slap) 2 4 7 | Oriental dance rhythm diagrams and descriptions (Baba Yaga Music); Middle Eastern Rhythms FAQ (Jas, khafif) |
| Baladi | Egypt | 8 steps | dum (low) 1 2 5, accent 1; tek (slap) 4 7 | Oriental dance rhythm diagrams and descriptions (Baba Yaga Music); Middle Eastern Rhythms FAQ (Jas, khafif) |
| Saidi | Egypt | 8 steps | dum (low) 1 4 5, accent 1; tek (slap) 2 7 | Oriental dance rhythm diagrams and descriptions (Baba Yaga Music); Middle Eastern Rhythms FAQ (Jas, khafif) |
| Malfuf | Egypt | 8 steps | dum (low) 1, accent 1; tek (slap) 4 7 | Oriental dance rhythm diagrams and descriptions (Baba Yaga Music); Middle Eastern Rhythms FAQ (Jas, khafif) |
| Ayoub | Egypt | 8 steps | dum (low) 1 5, accent 1; tek (slap) 7 | Oriental dance rhythm diagrams and descriptions (Baba Yaga Music); Middle Eastern Rhythms FAQ (Jas, khafif) |
| Chiftetelli | Turkey | 16 steps | dum (low) 1 9 11, accent 1; tek (slap) 4 7 13 | Oriental dance rhythm diagrams and descriptions (Baba Yaga Music); Middle Eastern Rhythms FAQ (Jas, khafif) |
| Karsilama | Turkey | 9 steps | dum (low) 1 5, accent 1; tek (slap) 3 7 8 9 | Oriental dance rhythm diagrams and descriptions (Baba Yaga Music); Middle Eastern Rhythms FAQ (Jas, khafif) |

### Turkey and the Balkans (6)

| rhythm | from | grid | drums | sources |
|---|---|---|---|---|
| Aksak, nine eight | Turkey and the Balkans | 4 steps (2+2+2+3) | drum (head) 1 2 3 4 | Toussaint 2005 (E(4,9)); Wikipedia Aksak citing Brailoiu 1951 (2+2+2+3); tempo a practice default |
| Rachenitsa, seven eight | Bulgaria | 3 steps (2+2+3) | drum (head) 1 2 3 | Wikipedia: Bulgarian dances; Chromatone: Balkan rhythms; Fiddling Around: Balkan rhythms in Celtic music |
| Lesnoto, seven eight | Bulgaria | 3 steps (3+2+2) | drum (head) 1 2 3 | Wikipedia: Bulgarian dances; Chromatone: Balkan rhythms; Fiddling Around: Balkan rhythms in Celtic music |
| Paidushko, five eight | Bulgaria | 2 steps (2+3) | drum (head) 1 2 | Wikipedia: Bulgarian dances; Chromatone: Balkan rhythms |
| Kopanitsa, eleven eight | Bulgaria | 5 steps (2+2+3+2+2) | drum (head) 1 2 3 4 5 | Wikipedia: Bulgarian dances; Chromatone: Balkan rhythms; Fiddling Around: Balkan rhythms in Celtic music |
| Postupano, thirteen eight | Bulgaria | 6 steps (2+2+2+3+2+2) | drum (head) 1 2 3 4 5 6 | Wikipedia: Bulgarian dances; Chromatone: Balkan rhythms; Fiddling Around: Balkan rhythms in Celtic music |

### India (5)

| rhythm | from | grid | drums | sources |
|---|---|---|---|---|
| Teental, sixteen beats | India | 16 steps | theka (head) 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16, accent 1; clap (clap) 1 5 13 | Wikipedia: Tala (music), Common Hindustani talas); DigiTabla: Tintal |
| Keherwa, eight beats | India | 8 steps | theka (head) 1 2 3 4 5 6 7 8, accent 1; clap (clap) 1 | Wikipedia: Tala (music), Common Hindustani talas); Sangtar: How to learn, read and play taals |
| Dadra, six beats | India | 6 steps | theka (head) 1 2 3 4 5 6, accent 1; clap (clap) 1 | Wikipedia: Tala (music), Common Hindustani talas); DigiTabla: Dadra Tal |
| Rupak, seven beats | India | 7 steps | theka (head) 1 2 3 4 5 6 7, accent 1; clap (clap) 4 6 | Wikipedia: Rupak Tala; DigiTabla: Rupak Tal |
| Jhaptal, ten beats | India | 10 steps | theka (head) 1 2 3 4 5 6 7 8 9 10, accent 1; clap (clap) 1 3 8 | Wikipedia: Jhaptal; DigiTabla: Jhaptal |

### Ireland and Scotland (3)

| rhythm | from | grid | drums | sources |
|---|---|---|---|---|
| Irish jig, six eight | Ireland | 6 steps | drum (head) 1 2 3 4 5 6, accent 1 4 | Bodhran workshop outline (C. S. MacCath); Irish session tunes handout (MIT) |
| Irish reel, four four | Ireland | 4 steps | drum (head) 1 2 3 4, accent 1 | Bodhran workshop outline (C. S. MacCath); Irish session tunes handout (MIT) |
| Slip jig, nine eight | Ireland | 9 steps | drum (head) 1 4 7, accent 1 | Irish session tunes handout (MIT); Wikipedia: Jig |

### The United States (3)

| rhythm | from | grid | drums | sources |
|---|---|---|---|---|
| Rock backbeat | United States | 8 steps | hat (hat) 1 2 3 4 5 6 7 8; kick (low) 1 5; snare (snare) 3 7, accent 3 7 | Gear4Music: basic drum beats; Rockschool Drums Method: Example 1, basic eighth-note beat; Wikipedia: Backbeat |
| Blues shuffle | United States | 8 steps, swing 0.33 | hat (hat) 1 2 3 4 5 6 7 8; kick (low) 1 5; snare (snare) 3 7, accent 3 7 | Soundbrenner: blues shuffle; Musicians Institute: shuffle; The Drum Ninja: blues drums |
| Jazz swing ride pattern | United States | 8 steps, swing 0.33 | ride (hat) 1 3 4 5 7 8, accent 3 7; foot (click) 3 7 | BYU Percussion: jazz style; Paul Wertico: jazz ride cymbal; Drumeo: a drummer's guide to jazz |

### Basics: meters and polyrhythms (5)

| rhythm | from | grid | drums | sources |
|---|---|---|---|---|
| Waltz, three four | Europe | 3 steps | oom (low) 1, accent 1; pah (rim) 2 3 | Wikipedia: Oom-pah; Soundbrenner: waltz rhythm |
| Polka, two four | Europe | 2 steps | oom (low) 1, accent 1; pah (rim) 2 | Soundbrenner: polka rhythm; Wikipedia: Polka |
| March, two four | Europe | 4 steps | bass (low) 1 3, accent 1 | Soundbrenner: march rhythm; Wikipedia: March (music)) |
| Three against two | Worldwide | 6 steps | two (low) 1 4, accent 1; three (click) 1 3 5 | Soundbrenner: polyrhythm basics; Wikipedia: Polyrhythm |
| Four against three | Worldwide | 12 steps | three (low) 1 5 9, accent 1; four (click) 1 4 7 10 | Soundbrenner: polyrhythm basics; Wikipedia: Polyrhythm |

## Not shipped

Found with one source only, sources that disagree, or notation only in images: malambo, bailecito,
huella, triunfo, chamarrita, polca correntina, milonga ciudadana (the same cell as the milonga),
huayno (the scale differs between sources), the candombe's piano drum, cáscara, partido alto,
ijexá, maracatu, xote, merengue, plena, bomba, joropo, festejo, landó, marinera, son jarocho,
huapango, saya, rocksteady, tangos flamencos, sevillanas, the gahu five-stroke and fume-fume bells,
kpegisu, the soli bell, the kpanlogo drums, djole, dundunba, masmoudi kabir, wahda, samai thaqil,
funk, second line, Motown, boogaloo, hornpipe, the strathspey and the 13/8 as 3+4+4+2. Left out
as the same as one that ships: the bembé bell and agbekor's bell (the standard bell), the kpanlogo
bell (the son clave), alegrías (soleá's accents), masmoudi saghir (baladi). A new file needs two
agreeing sources to be `verified`.

## Colours

A drum's colour comes from its kind of sound, the same in every rhythm, so a learner reads the sound
by its colour (`scripts/library.mjs` `COLOR`): warm for skins, cool for wood and metal, amber and
lime for hands and seeds. Every two are at least 24 apart (CIE76) and all are lit on the black glass;
the hand is white, a main beat's mark light grey, the others' darker; the play and stop marks and the
tempo face's notch are Molty's green, the choosing face's notch warm gold; each region has a quiet
tint of its own (its beads, its name above the rhythm's).

| sound | colour | for |
|---|---|---|
| low | `#ff6a5c` coral red | the bass drum, the bombo's low, a darbuka's dum |
| head | `#ffa36b` apricot | a drum's head, the parche |
| tom | `#ff86c8` pink | a hand drum's open tone |
| slap | `#d58cff` violet | a hand drum's slap, a darbuka's tek |
| snare | `#a3a0ff` periwinkle | the snare |
| rim | `#5fb0ff` blue | the rim, the aro, sticks, the clave |
| hat | `#5fe0d6` teal | the hi-hat, the ride, the triangle |
| bell | `#86df6e` green | a bell, the agogo, the cowbell |
| shaker | `#d9e85e` lime | shakers, maracas, the guacharaca |
| clap | `#ffcf4a` amber | hand claps, palmas |
| click | `#ffffff` white | the count, a metronome |
