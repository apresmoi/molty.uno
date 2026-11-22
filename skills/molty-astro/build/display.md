# Astrology: screens

Generated from display.yaml by `molty-display build`. Do not edit: change display.yaml and build again.

Draws the sky on Molty, the zodiac wheel with aspects, planet globes, the signs, eclipses and transits. Play it with the molty-astro skill. Package `astrology`, version 1.7.1. On a Molty without screens: say it in words instead.

Show a screen with `scene.play { pkg, layout, params, ttl_s }` (`scene.queue` plays it after the one showing). `layout` is the screen's name.
Send every input and nothing else. `inputs/<screen>.schema.json` (next to this file) checks params before you send them.

| screen | shows | lasts |
|---|---|---|
| [globe](#globe) | One body as a lit globe, the Moon at tonight's phase or a planet. | 2.5 s |
| [saturn](#saturn) | Saturn as a lit globe inside its rings, tilted as they are seen tonight. | 2.5 s |
| [sign](#sign) | One zodiac sign, large. | 4 s |
| [chart](#chart) | All ten bodies on the zodiac wheel, joined by their tightest aspects. A tap on a planet or a line reads it; a tap on a sign opens it up close. | held |
| [sign-closeup](#sign-closeup) | One sign up close, its planets on its degree dial, each lit in turn while its reading shows. Molty opens it itself when a sign is tapped on the chart. | held |
| [signs](#signs) | The twelve signs together, then each in turn, then all twelve again. | 36 s |
| [aspect](#aspect) | One aspect on the degree grid, its two bodies, the angle between them and the figure it belongs to. | 5 s |
| [transit](#transit) | Mercury or Venus crossing the face of the Sun. | 6 s |
| [solar-eclipse](#solar-eclipse) | The Moon passing in front of the Sun. | 6 s |
| [occultation](#occultation) | The Moon passing in front of a planet. | 6 s |
| [appulse](#appulse) | Two planets passing close to each other. | 6 s |
| [lunar-eclipse](#lunar-eclipse) | The Moon crossing the Earth's shadow, turning red inside it. | 6 s |

## globe

One body as a lit globe, the Moon at tonight's phase or a planet. Lasts 2.5 s.

| send | type | about |
|---|---|---|
| `img` | one of: `moon` `moon-red` `sun` `mercury` `venus` `mars` `jupiter` `saturn` | Which body: moon, mercury, venus, mars or jupiter. |
| `lit` | number, 0 to 1 | The fraction of its disc that is lit. |
| `side` | number, -1 to 1 | 1 when its right side is lit, -1 when its left. The Moon: 1 waxing, -1 waning. A planet: 1 when it is east of the Sun ((its longitude - the Sun's) mod 360 is under 180), else -1. |
| `r` | distance, 0 to 2 (1 is the rim) | 0.62 for the Moon, 0.58 for a planet. |
| `spin` | number | Degrees per second it turns, for show: 0 for the Moon, 2 mercury, 3 venus, 7 mars, 29 jupiter. |

A call that works:

```json
{"name":"scene.play","args":{"pkg":"astrology","layout":"globe","params":{"img":"moon","lit":0.499,"side":-1,"r":0.62,"spin":0},"ttl_s":8}}
```

## saturn

Saturn as a lit globe inside its rings, tilted as they are seen tonight. Lasts 2.5 s.

| send | type | about |
|---|---|---|
| `lit` | number, 0 to 1 | The fraction of its disc that is lit. |
| `side` | number, -1 to 1 | 1 when Saturn is east of the Sun (lit on the right), else -1. |
| `rings` | one of: `rings-p3` `rings-n3` `rings-p9` `rings-n9` `rings-p15` `rings-n15` `rings-p21` `rings-n21` `rings-p27` `rings-n27` | The ring tilt seen from Earth: rings-p<t> for a tilt of 0 or more, rings-n<t> below 0, t the nearest of 3, 9, 15, 21, 27 degrees. |

A call that works:

```json
{"name":"scene.play","args":{"pkg":"astrology","layout":"saturn","params":{"lit":0.999,"side":1,"rings":"rings-p9"},"ttl_s":8}}
```

## sign

One zodiac sign, large. Lasts 4 s.

| send | type | about |
|---|---|---|
| `sign` | one of: `aries` `taurus` `gemini` `cancer` `leo` `virgo` `libra` `scorpio` `sagittarius` `capricorn` `aquarius` `pisces` | The sign, lower case: for tonight, the one the Moon is in. |

A call that works:

```json
{"name":"scene.play","args":{"pkg":"astrology","layout":"sign","params":{"sign":"leo"},"ttl_s":9}}
```

## chart

All ten bodies on the zodiac wheel, joined by their tightest aspects. A tap on a planet or a line reads it; a tap on a sign opens it up close. Held until another screen replaces it or its `ttl_s` ends. Readings show in the middle.

| send | type | about |
|---|---|---|
| `bodies` | list, up to 10 | The Sun, the Moon and the eight planets, in order of longitude. |
| `signs` | list, up to 12 | Each of the twelve signs: its reading, and what its close-up needs. The wheel draws the signs itself; a tap anywhere in a sign's 30 degree block opens sign-closeup on Molty, with these fields, the sign's ruler and the bodies of it. |
| `aspects` | list, up to 11 | The three tightest aspects a line can show (none for a pair closer than 0.2). With --all, every one that fits, at most 11. |
| `ruler` | list, up to 12 | Each sign's ruler, for its close-up: twelve, one a sign. |

Each element of `bodies`:

| field | type | about |
|---|---|---|
| `g` | one of: `g-sun` `g-moon` `g-mercury` `g-venus` `g-mars` `g-jupiter` `g-saturn` `g-uranus` `g-neptune` `g-pluto` | The body's symbol: g-sun, g-moon, g-mercury ... |
| `a` | angle in degrees | Its chart angle: (180 + its ecliptic longitude) mod 360, so 0 Aries sits at 9 o'clock. |
| `r` | distance, 0 to 2 (1 is the rim) | 0.64, pulled in by 0.1 for each level, when it is within 13 degrees of a body already placed on that level. |
| `c` | color, #rrggbb | Its color: sun #ffd678, moon #e6e6e0, mercury #c8bcb0, venus #fff0c4, mars #ff7a55, jupiter #ffc266, saturn #e8d890, uranus #9fe3e0, neptune #8fb4ff, pluto #c0a0d0. |
| `say` | text a tap shows, up to 160 characters | What a tap on it shows: Sun in Gemini, you shine by talking and learning. Then its tightest aspect, if it fits. |
| `id` | text, up to 40 letters | Its name, lower case (sun, moon ...): names it in the tap event, and is the group an aspect lights. |
| `of` | text, up to 40 letters | The sign it is in, lower case: that sign's close-up shows the bodies of it. |
| `da` | angle in degrees | Its place on that sign's degree dial, as in sign-closeup. |
| `d` | color, #rrggbb | Its color at 60 percent, for when it is not being read there. |
| `on` | seconds, 0 to 600 | When its reading starts in the close-up. |
| `off` | seconds, 0 to 600 | When its reading ends in the close-up. |

Each element of `signs`:

| field | type | about |
|---|---|---|
| `a` | angle in degrees | The sign's middle on the wheel: (195 + 30 i) mod 360 for sign i, Aries 0. |
| `say` | text a tap shows, up to 160 characters | Its reading, the close-up's subtitle: its nature and ruler, then its planets or, when it is empty, where its ruler is. |
| `id` | text, up to 40 letters | The sign's name, lower case: names it in the tap event. |
| `sign` | one of: `aries` `taurus` `gemini` `cancer` `leo` `virgo` `libra` `scorpio` `sagittarius` `capricorn` `aquarius` `pisces` | The sign again, lower case: the close-up's symbol. |
| `c` | color, #rrggbb | The sign's color, as in sign-closeup. |
| `name` | text, up to 11 letters | The sign's name in capitals. |
| `nx` | number, -1 to 1 | The name's centre in the close-up, as in sign-closeup. |
| `ns` | distance, 0 to 2 (1 is the rim) | The name's type size there: 0.13, or 0.11 past 8 letters. |
| `ttl` | seconds, 1 to 600 | How long the close-up plays: its reading's length plus 3 s. |

Each element of `aspects`:

| field | type | about |
|---|---|---|
| `x1` | number, -1 to 1 | The line's start: 0.085 from the first body's centre, toward the second. |
| `y1` | number, -1 to 1 | The line's start, y. |
| `x2` | number, -1 to 1 | The line's end: 0.085 short of the second body's centre. |
| `y2` | number, -1 to 1 | The line's end, y. |
| `px1` | number, -1 to 1 | The first body's centre, where its ring shows while the aspect is lit. |
| `py1` | number, -1 to 1 | The first body's centre, y. |
| `px2` | number, -1 to 1 | The second body's centre. |
| `py2` | number, -1 to 1 | The second body's centre, y. |
| `c` | color, #rrggbb | Tense (square, opposition) #ff5a4a, easy (trine, sextile) #6fa8ff, a conjunction #c8cce0. With --all, dimmed by up to 70 percent as the orb grows to 6. |
| `say` | text a tap shows, up to 160 characters | What a tap on the line shows: Mars opposes Saturn, drive meets limits. |
| `id` | text, up to 40 letters | The pair, lower case: sun-moon. Names the line in the tap event, and is its group. |
| `lights` | group names, spaces between, up to 4 | What a tap on the line lights: the aspect and its two bodies, as in: sun-moon sun moon. |

Each element of `ruler`:

| field | type | about |
|---|---|---|
| `of` | text, up to 40 letters | The sign it rules, lower case. |
| `g` | one of: `g-sun` `g-moon` `g-mercury` `g-venus` `g-mars` `g-jupiter` `g-saturn` `g-uranus` `g-neptune` `g-pluto` | The ruler's symbol: g-mars ... |
| `x` | number, -1 to 1 | Its centre in the close-up, as in sign-closeup. |
| `c` | color, #rrggbb | The ruler's color. |

A call that works (lists cut to two elements here):

```json
{"name":"scene.play","args":{"pkg":"astrology","layout":"chart","params":{"bodies":[{"g":"g-neptune","a":182.07,"r":0.64,"c":"#8fb4ff","say":"Neptune in Aries: you dream by being bold and going first. Pluto lends a hand.","id":"neptune","of":"aries","da":193.45,"d":"#566c99","on":4.19,"off":10.56},{"g":"g-saturn","a":189.23,"r":0.54,"c":"#e8d890","say":"Saturn in Aries: you learn hard lessons with courage and a fast start. Moon makes it flow easily.","id":"saturn","of":"aries","da":136.17,"d":"#8b8256","on":10.56,"off":18.08}],"signs":[{"a":195,"say":"Aries: bold, quick fire, ruled by Mars. Neptune and Saturn here: by being bold and going first, you live out your dreams and your discipline.","id":"aries","sign":"aries","c":"#ff9a7a","name":"ARIES","nx":-0.095,"ns":0.13,"ttl":22},{"a":225,"say":"Taurus: steady, sensual earth, ruled by Venus. Empty now, so its ruler, Venus in Libra, shows how it plays out for you.","id":"taurus","sign":"taurus","c":"#a8d890","name":"TAURUS","nx":-0.095,"ns":0.13,"ttl":14}],"aspects":[{"x1":0.486,"y1":0.322,"x2":0.417,"y2":-0.408,"px1":0.494,"py1":0.407,"px2":0.409,"py2":-0.492,"c":"#ff5a4a","say":"Sun squares Moon: friction between self and mood.","id":"sun-moon","lights":"sun-moon sun moon"},{"x1":0.331,"y1":-0.459,"x2":-0.455,"y2":-0.12,"px1":0.409,"py1":-0.492,"px2":-0.533,"py2":-0.087,"c":"#6fa8ff","say":"Moon flows with Saturn: mood and limits in step.","id":"moon-saturn","lights":"moon-saturn moon saturn"}],"ruler":[{"of":"aries","g":"g-mars","x":0.299,"c":"#ff7a55"},{"of":"taurus","g":"g-venus","x":0.355,"c":"#fff0c4"}]},"ttl_s":60}}
```

## sign-closeup

One sign up close, its planets on its degree dial, each lit in turn while its reading shows. Molty opens it itself when a sign is tapped on the chart. Held until another screen replaces it or its `ttl_s` ends.

| send | type | about |
|---|---|---|
| `sign` | one of: `aries` `taurus` `gemini` `cancer` `leo` `virgo` `libra` `scorpio` `sagittarius` `capricorn` `aquarius` `pisces` | The sign, lower case. Also names the centre's tap zone in the tap event. |
| `c` | color, #rrggbb | The sign's color: #ff9a7a, #a8d890, #9fc0ff for sign index (Aries 0) mod 3. |
| `name` | text, up to 11 letters | The sign's name in capitals. |
| `nx` | number, -1 to 1 | The name's centre: the name and the ruler's symbol after it (0.13 wide, 0.06 apart) make one row, centred. A letter is 6/7 of the type size wide, the last 5/7. |
| `ns` | distance, 0 to 2 (1 is the rim) | The name's type size, 0.13, or 0.11 past 8 letters. |
| `say` | text a tap shows, up to 160 characters | What a tap on the sign shows, the same reading as on the chart. |
| `ruler` | list, up to 1 | The sign's ruler, drawn after its name. Modern rulers from Aries on: Mars, Venus, Mercury, Moon, Sun, Mercury, Venus, Pluto, Jupiter, Saturn, Uranus, Neptune. |
| `bodies` | list, up to 10 | Each planet in the sign, by degree. |

Each element of `ruler`:

| field | type | about |
|---|---|---|
| `g` | one of: `g-sun` `g-moon` `g-mercury` `g-venus` `g-mars` `g-jupiter` `g-saturn` `g-uranus` `g-neptune` `g-pluto` | The ruler's symbol: g-mars ... |
| `x` | number, -1 to 1 | Its centre: the row's right end less half its width (0.065). |
| `c` | color, #rrggbb | The ruler's color, as on the chart. |

Each element of `bodies`:

| field | type | about |
|---|---|---|
| `g` | one of: `g-sun` `g-moon` `g-mercury` `g-venus` `g-mars` `g-jupiter` `g-saturn` `g-uranus` `g-neptune` `g-pluto` | The planet's symbol: g-moon ... |
| `da` | angle in degrees | Its place on the dial: 210 - 8 x its degree in the sign (0 lower left, over the top, 30 lower right), kept at least 28 apart and within 210 to -30. |
| `c` | color, #rrggbb | Its color, as on the chart. |
| `d` | color, #rrggbb | Its color at 60 percent, for when it is not being read. |
| `on` | seconds, 0 to 600 | When Molty starts reading it: about 2.6 words a second and 0.3 s a sentence, after 1.2 s and the sign's own line. |
| `off` | seconds, 0 to 600 | When Molty has read it. |
| `say` | text a tap shows, up to 160 characters | What a tap on it shows. |
| `id` | text, up to 40 letters | Its name, lower case: names it in the tap event. |

A call that works (lists cut to two elements here):

```json
{"name":"scene.play","args":{"pkg":"astrology","layout":"sign-closeup","params":{"sign":"leo","c":"#a8d890","name":"LEO","nx":-0.095,"ns":0.13,"say":"Leo: warm, proud fire, ruled by the Sun. Moon, Mars and Jupiter here: with warmth and a big heart, you live out your feelings, your drive and your luck.","ruler":[{"g":"g-sun","x":0.188,"c":"#ffd678"}],"bodies":[{"g":"g-moon","da":132.12,"c":"#e6e6e0","d":"#8a8a86","on":4.58,"off":12.48,"say":"Moon in Leo: you feel at home with warmth and a big heart. Sun adds friction that pushes you.","id":"moon"},{"g":"g-mars","da":57.51,"c":"#ff7a55","d":"#994933","on":12.48,"off":20.01,"say":"Mars in Leo: you go after things by being seen and playing. Mercury adds friction that pushes you.","id":"mars"}]},"ttl_s":60}}
```

## signs

The twelve signs together, then each in turn, then all twelve again. Lasts 36 s.

No inputs: send `"params": {}`.

A call that works:

```json
{"name":"scene.play","args":{"pkg":"astrology","layout":"signs","params":{},"ttl_s":41}}
```

## aspect

One aspect on the degree grid, its two bodies, the angle between them and the figure it belongs to. Lasts 5 s.

| send | type | about |
|---|---|---|
| `a` | angle in degrees | The chart angle of body A, the one the short way round to B starts from, counter-clockwise. |
| `b` | angle in degrees | from_angle + d, where d is B's chart angle minus A's, wrapped to 0..180 (A and B chosen so it is): the arc runs counter-clockwise from one to the other, the short way. |
| `c` | color, #rrggbb | The aspect's color: conjunction #ffd678, sextile #6fe0a0, square #ff6a55, trine #7fb0ff, opposition #ff5fa0. |
| `bodies` | list, up to 2 | The two bodies, A then B. |
| `lines` | list, up to 7 | The aspect's figure on radius 0.6 from A's angle (sextile 6 sides, square 4, trine 3), then the chord from A to B. None for a conjunction. |
| `deg` | text, up to 3 letters | The separation in whole degrees, as text (the device font has no degree sign). |
| `tx` | number, -1 to 1 | Where the number sits: at angle from_angle + d/2, radius 0.14. 0 for a conjunction. |
| `ty` | number, -1 to 1 | Where the number sits, y. 0 for a conjunction. |

Each element of `bodies`:

| field | type | about |
|---|---|---|
| `g` | one of: `g-sun` `g-moon` `g-mercury` `g-venus` `g-mars` `g-jupiter` `g-saturn` `g-uranus` `g-neptune` `g-pluto` | The body's symbol: g-mars ... |
| `a` | angle in degrees | Its chart angle. |
| `r` | distance, 0 to 2 (1 is the rim) | 0.6; B at 0.47 when the two are under 14 degrees apart. |
| `c` | color, #rrggbb | Its color, as on the chart. |

Each element of `lines`:

| field | type | about |
|---|---|---|
| `x1` | number, -1 to 1 | The segment's start. |
| `y1` | number, -1 to 1 | The segment's start, y. |
| `x2` | number, -1 to 1 | The segment's end. |
| `y2` | number, -1 to 1 | The segment's end, y. |
| `c` | color, #rrggbb | A side of the figure #565b72; the chord in the aspect's color. |
| `on` | seconds, 0 to 600 | When it appears: 0 for the figure, 1.2 for the chord. |

A call that works (lists cut to two elements here):

```json
{"name":"scene.play","args":{"pkg":"astrology","layout":"aspect","params":{"a":123.13,"b":182.07,"c":"#6fe0a0","bodies":[{"g":"g-pluto","a":123.13,"r":0.6,"c":"#c0a0d0"},{"g":"g-neptune","a":182.07,"r":0.6,"c":"#8fb4ff"}],"lines":[{"x1":-0.328,"y1":0.502,"x2":-0.599,"y2":-0.033,"c":"#565b72","on":0},{"x1":-0.599,"y1":-0.033,"x2":-0.271,"y2":-0.535,"c":"#565b72","on":0}],"deg":"59","tx":-0.124,"ty":0.064},"ttl_s":10}}
```

## transit

Mercury or Venus crossing the face of the Sun. Lasts 6 s.

| send | type | about |
|---|---|---|
| `path` | list, up to 14 | The planet against the Sun in 14 steps, sampled evenly from 15 percent before the transit starts to 15 percent after it ends. Scale: 0.55 over the Sun's angular radius. |

Each element of `path`:

| field | type | about |
|---|---|---|
| `x` | number, -2 to 2 | Offset on the sky from the still body, east to the left. |
| `y` | number, -2 to 2 | Offset on the sky from the still body, north up. |
| `on` | seconds, 0 to 600 | When this step shows: 0.3 + 5.4 i / 14 for step i. |
| `off` | seconds, 0 to 600 | When the next step takes over (the last one: 6). |

A call that works (lists cut to two elements here):

```json
{"name":"scene.play","args":{"pkg":"astrology","layout":"transit","params":{"path":[{"x":-0.66,"y":0.07,"on":0.3,"off":0.69},{"x":-0.58,"y":0.1,"on":0.69,"off":1.07}]},"ttl_s":11}}
```

## solar-eclipse

The Moon passing in front of the Sun. Lasts 6 s.

| send | type | about |
|---|---|---|
| `path` | list, up to 14 | The Moon against the Sun in 14 steps, 2.6 hours either side of the peak, seen from the point of greatest eclipse when it has one. Scale: 0.5 over the Sun's angular radius. |
| `moon_r` | distance, 0 to 2 (1 is the rim) | The Moon's radius, the Sun's being 0.5. |

Each element of `path`:

| field | type | about |
|---|---|---|
| `x` | number, -2 to 2 | Offset on the sky from the still body, east to the left. |
| `y` | number, -2 to 2 | Offset on the sky from the still body, north up. |
| `on` | seconds, 0 to 600 | When this step shows: 0.3 + 5.4 i / 14 for step i. |
| `off` | seconds, 0 to 600 | When the next step takes over (the last one: 6). |

A call that works (lists cut to two elements here):

```json
{"name":"scene.play","args":{"pkg":"astrology","layout":"solar-eclipse","params":{"path":[{"x":1.11,"y":-0.84,"on":0.3,"off":0.69},{"x":0.93,"y":-0.72,"on":0.69,"off":1.07}],"moon_r":0.457},"ttl_s":11}}
```

## occultation

The Moon passing in front of a planet. Lasts 6 s.

| send | type | about |
|---|---|---|
| `path` | list, up to 14 | The Moon against the planet in 14 steps, 2.2 hours either side of the closest moment. Scale: 0.45 over the Moon's angular radius. |
| `planet` | one of: `moon` `moon-red` `sun` `mercury` `venus` `mars` `jupiter` `saturn` | The planet it covers: venus, mars, jupiter or saturn. |

Each element of `path`:

| field | type | about |
|---|---|---|
| `x` | number, -2 to 2 | Offset on the sky from the still body, east to the left. |
| `y` | number, -2 to 2 | Offset on the sky from the still body, north up. |
| `on` | seconds, 0 to 600 | When this step shows: 0.3 + 5.4 i / 14 for step i. |
| `off` | seconds, 0 to 600 | When the next step takes over (the last one: 6). |

A call that works (lists cut to two elements here):

```json
{"name":"scene.play","args":{"pkg":"astrology","layout":"occultation","params":{"path":[{"x":2,"y":0,"on":0.3,"off":0.69},{"x":1.78,"y":-0.02,"on":0.69,"off":1.07}],"planet":"mars"},"ttl_s":11}}
```

## appulse

Two planets passing close to each other. Lasts 6 s.

| send | type | about |
|---|---|---|
| `path` | list, up to 14 | The moving planet against the still one in 14 steps, 4 days either side of the closest moment. Scale: 0.32 over the larger of their separation and 0.08 degrees. |
| `a` | one of: `moon` `moon-red` `sun` `mercury` `venus` `mars` `jupiter` `saturn` | The planet in the middle: one of mercury to saturn, at least 12 degrees from the Sun. |
| `b` | one of: `moon` `moon-red` `sun` `mercury` `venus` `mars` `jupiter` `saturn` | The planet that passes within 0.5 degrees of it. |

Each element of `path`:

| field | type | about |
|---|---|---|
| `x` | number, -2 to 2 | Offset on the sky from the still body, east to the left. |
| `y` | number, -2 to 2 | Offset on the sky from the still body, north up. |
| `on` | seconds, 0 to 600 | When this step shows: 0.3 + 5.4 i / 14 for step i. |
| `off` | seconds, 0 to 600 | When the next step takes over (the last one: 6). |

A call that works (lists cut to two elements here):

```json
{"name":"scene.play","args":{"pkg":"astrology","layout":"appulse","params":{"path":[{"x":-1.65,"y":0.05,"on":0.3,"off":0.69},{"x":-1.43,"y":0.08,"on":0.69,"off":1.07}],"a":"venus","b":"mars"},"ttl_s":11}}
```

## lunar-eclipse

The Moon crossing the Earth's shadow, turning red inside it. Lasts 6 s.

| send | type | about |
|---|---|---|
| `path` | list, up to 14 | The Moon against the centre of the Earth's shadow in 14 steps, 3.2 hours either side of the peak. Scale: 0.8 over the penumbra's radius. |
| `umbra` | distance, 0 to 2 (1 is the rim) | The radius of the Earth's full shadow. |
| `pen` | distance, 0 to 2 (1 is the rim) | The radius of the Earth's partial shadow (0.8 at this scale). |
| `moon_r` | distance, 0 to 2 (1 is the rim) | The Moon's radius at the same scale. |

Each element of `path`:

| field | type | about |
|---|---|---|
| `x` | number, -2 to 2 | Offset on the sky from the still body, east to the left. |
| `y` | number, -2 to 2 | Offset on the sky from the still body, north up. |
| `on` | seconds, 0 to 600 | When this step shows: 0.3 + 5.4 i / 14 for step i. |
| `off` | seconds, 0 to 600 | When the next step takes over (the last one: 6). |
| `img` | one of: `moon` `moon-red` `sun` `mercury` `venus` `mars` `jupiter` `saturn` | moon-red while this step is inside the umbra, else moon. |

A call that works (lists cut to two elements here):

```json
{"name":"scene.play","args":{"pkg":"astrology","layout":"lunar-eclipse","params":{"path":[{"x":1.02,"y":0.38,"on":0.3,"off":0.69,"img":"moon"},{"x":0.86,"y":0.35,"on":0.69,"off":1.07,"img":"moon"}],"umbra":0.451,"pen":0.8,"moon_r":0.167},"ttl_s":11}}
```
