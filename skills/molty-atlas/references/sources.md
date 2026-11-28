# Where the sky comes from

Every number and drawing in `data/catalog.json` is from open data (each source's link: [data/sources.json](../data/sources.json)); the drawings of galaxies, globular clusters and most
nebulae are models Molty makes from catalogue numbers (`atlas_model`), never photographs. The catalogue is built in the
design record (research/design-record-staging/sky-atlas/data: `make.mjs`, `export.mjs`), from these:

| source | used for | licence | credit |
|---|---|---|---|
| astronomy-engine 2.1.19, Don Cross | every position, phase, rise and set, constellation boundaries, the Sun's longitude | MIT | the copy molty-astro vendors |
| Hipparcos main catalogue (ESA 1997, I/239) | stars' places, magnitudes, colours, distances; the Pleiades | free use with acknowledgement | ESA, The Hipparcos and Tycho Catalogues, ESA SP-1200 |
| IAU Catalog of Star Names (WGSN) | the 50 bright stars' names | CC BY | IAU WGSN |
| Stellarium sky culture western_SnT | the 88 constellation figures (Sky & Telescope's, the IAU's) | CC BY-SA 2.0 (the lines; the derived figures in data/catalog.json are under it too) | Sky & Telescope, Stellarium (Paul Krizak, Jonathan E. Piskor) |
| OpenNGC, Mattia Verga | Messier, NGC and IC objects: type, place, size, angle, magnitude, Hubble type, names | CC BY-SA 4.0 (the derived rows in data/catalog.json are under it too) | Mattia Verga, OpenNGC |
| Washington Double Star catalogue (US Naval Observatory) | the 18 double stars | a US government work (no licence line in the file) | USNO |
| IMO Meteor Shower Calendar 2027, Table 5 | the 12 showers' radiants, peaks and rates (numbers only) | facts, cited | International Meteor Organization |
| Orion Nebula, Hubble 2006 mosaic | M42's dots | public domain | NASA, ESA, M. Robberto (STScI/ESA) and the Hubble Space Telescope Orion Treasury Project Team |
| Ring Nebula | M57's dots | public domain | The Hubble Heritage Team (AURA/STScI/NASA) |
| Wikipedia page views (2025-11 to 2026-10) | "best known first" | CC0 | Wikimedia Foundation |

From memory, to check: the magnitudes OpenNGC leaves blank for a few bright objects (the Pleiades 1.6, the Hyades 0.5,
the Beehive 3.7, M7 3.3, the Carina Nebula 1.0 and a few more) and the Coalsack's size (about 7 by 5 degrees).

Not used: the planet maps molty-astro draws its globes with (their source is not recorded in the repo): the atlas's
Moon and planets are plain shaded globes in their colour.
