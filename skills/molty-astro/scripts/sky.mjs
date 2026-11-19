// SPDX-License-Identifier: MIT
// The sky for a date, as the inputs of this skill's screens (../display.yaml, ../screens/).
// All astronomy comes from astronomy-engine (MIT, vendored); nothing here draws. Ported from the
// lab prototype (molty-creature-lab/astronomy/sky.mjs): same instants, aspects, orbs and events.
import { createRequire } from 'node:module';
const A = createRequire(import.meta.url)('./vendor/astronomy.min.js');

const TAU = Math.PI * 2, DAY = 86400000, RAD = Math.PI / 180, mod360 = (d) => ((d % 360) + 360) % 360;
const r2 = (v) => +v.toFixed(2) + 0, r3 = (v) => +v.toFixed(3) + 0;
export const BODIES = ['Sun', 'Moon', 'Mercury', 'Venus', 'Mars', 'Jupiter', 'Saturn', 'Uranus', 'Neptune', 'Pluto'];
// the bodies' colours (a body's color input, ../screens/chart.yaml)
const INK = { Sun: '#ffd678', Moon: '#e6e6e0', Mercury: '#c8bcb0', Venus: '#fff0c4', Mars: '#ff7a55', Jupiter: '#ffc266', Saturn: '#e8d890', Uranus: '#9fe3e0', Neptune: '#8fb4ff', Pluto: '#c0a0d0' };
// the sign glyphs' colours (../display.yaml coral, sage, sky) and the close-up's ruler glyph size (../screens/sign-closeup.yaml)
const SIGN_INK = ['#ff9a7a', '#a8d890', '#9fc0ff'], RULER = 0.13, RULER_GAP = 0.06;
export const ASPECTS = [{ angle: 0, name: 'conjunction', c: '#ffd678' }, { angle: 60, name: 'sextile', c: '#6fe0a0' }, { angle: 90, name: 'square', c: '#ff6a55' }, { angle: 120, name: 'trine', c: '#7fb0ff' }, { angle: 180, name: 'opposition', c: '#ff5fa0' }];
export const MAX_ORB = 6;
export const SIGNS = ['Aries', 'Taurus', 'Gemini', 'Cancer', 'Leo', 'Virgo', 'Libra', 'Scorpio', 'Sagittarius', 'Capricorn', 'Aquarius', 'Pisces'];

/** A bare date means that evening: 21:00 UTC, a fixed instant, since no place is assumed. */
export const instant = (d) => new Date(/^\d{4}-\d{2}-\d{2}$/.test(d) ? `${d}T21:00:00Z` : d);
export const eclipticLon = (name, date) => (name === 'Sun' ? A.SunPosition(date).elon : A.Ecliptic(A.GeoVector(A.Body[name], date, true)).elon);
export const signOf = (lon) => SIGNS[Math.floor(mod360(lon) / 30)];
export const sep = (a, b) => { const d = mod360(a - b); return d > 180 ? 360 - d : d; };
/** Screen angle (degrees) of an ecliptic longitude: 0 Aries at 9 o'clock, longitude counter-clockwise, as a chart is drawn. */
export const chartAngle = (lon) => r2(mod360(180 + lon));
const fmt = (d) => d.toLocaleDateString('en-GB', { day: 'numeric', month: 'short', year: 'numeric', timeZone: 'UTC' });
const polar = (deg, r) => [r3(r * Math.cos(deg * RAD)), r3(r * Math.sin(deg * RAD))];

export function sky(date) {
  const phase = A.MoonPhase(date), ill = A.Illumination(A.Body.Moon, date), moonLon = eclipticLon('Moon', date);
  return { sunLon: eclipticLon('Sun', date), bodies: BODIES.map((name) => ({ name, lon: eclipticLon(name, date) })), moon: { fraction: ill.phase_fraction, waxing: phase < 180, lon: moonLon, sign: signOf(moonLon) } };
}
/** Every major aspect among these bodies within maxOrb, tightest first. */
export function findAspects(list, maxOrb = MAX_ORB) {
  const out = [];
  for (let i = 0; i < list.length; i++) for (let j = i + 1; j < list.length; j++) { const s = sep(list[i].lon, list[j].lon); for (const a of ASPECTS) { const orb = Math.abs(s - a.angle); if (orb <= maxOrb) out.push({ a: list[i].name, b: list[j].name, lonA: list[i].lon, lonB: list[j].lon, name: a.name, angle: a.angle, c: a.c, sep: s, orb }); } }
  return out.sort((x, y) => x.orb - y.orb);
}
/** The tightest pair for this aspect on `date`, or the next date it comes within 1 degree. */
export function aspectOf(kind, date) {
  const now = findAspects(sky(date).bodies).find((a) => a.name === kind); if (now) return { ...now, when: date, tonight: true };
  for (let i = 1; i < 800; i++) { const d = new Date(date.getTime() + i * DAY), hit = findAspects(sky(d).bodies, 1).find((a) => a.name === kind); if (hit) return { ...hit, when: d, tonight: false }; }
  throw new Error(`no ${kind} in 800 days`);
}
/** Bodies on the chart: true longitude, pushed inward in steps when crowded. */
export function placeBodies(bodies, rBase = 0.64) { const placed = []; return [...bodies].sort((a, b) => a.lon - b.lon).map((b) => { let level = 0; while (placed.some((p) => p.level === level && sep(p.lon, b.lon) < 13)) level++; const it = { ...b, level, r: rBase - level * 0.1 }; placed.push(it); return it; }); }

// ---- scenes: each builder returns [[layout, params], ...] (the first plays, the rest queue), a short line to say and the facts.
const litSide = (lon, sunLon) => (mod360(lon - sunLon) < 180 ? 1 : -1); // east of the Sun: lit on its west, the right, north up
const RING_TILTS = [3, 9, 15, 21, 27];
const SPIN = { mercury: 2, venus: 3, mars: 7, jupiter: 29 }; // degrees per second, for show
const PLANETS = ['Mercury', 'Venus', 'Mars', 'Jupiter', 'Saturn'];
/** The moon, the five bright planets, then the moon's sign: seven small scenes. */
export function tonight(date) {
  const s = sky(date), scenes = [['globe', { img: 'moon', lit: r3(s.moon.fraction), side: s.moon.waxing ? 1 : -1, r: 0.62, spin: 0 }]];
  for (const p of PLANETS) {
    const ill = A.Illumination(A.Body[p], date), lit = r3(ill.phase_fraction), side = litSide(eclipticLon(p, date), s.sunLon);
    if (p !== 'Saturn') { scenes.push(['globe', { img: p.toLowerCase(), lit, side, r: 0.58, spin: SPIN[p.toLowerCase()] }]); continue; }
    const tilt = ill.ring_tilt, near = RING_TILTS.reduce((a, b) => (Math.abs(b - Math.abs(tilt)) < Math.abs(a - Math.abs(tilt)) ? b : a));
    scenes.push(['saturn', { lit, side, rings: `rings-${tilt < 0 ? 'n' : 'p'}${near}` }]);
  }
  scenes.push(['sign', { sign: s.moon.sign.toLowerCase() }]);
  const pct = Math.round(s.moon.fraction * 100);
  return { scenes, say: `Tonight's moon is ${pct} percent lit, ${s.moon.waxing ? 'waxing' : 'waning'}, in ${s.moon.sign}. Then Mercury, Venus, Mars, Jupiter and Saturn.`, facts: { moon: { lit: pct, waxing: s.moon.waxing, sign: s.moon.sign }, planets: Object.fromEntries(PLANETS.map((p) => { const lon = eclipticLon(p, date); return [p, `${Math.floor(mod360(lon) % 30)} ${signOf(lon)}`]; })) } };
}
// The chart's aspects: the tightest few, each a hairline between its two bodies' symbols (../screens/chart.yaml):
// tense red, easy blue, a conjunction white; LINE_GAP outside each symbol's centre, none for a pair too close for a line
const DRAWN = 3, TONE = { square: 'tense', opposition: 'tense', trine: 'easy', sextile: 'easy', conjunction: 'joined' };
const TONE_INK = { tense: '#ff5a4a', easy: '#6fa8ff', joined: '#c8cce0' }, LINE_GAP = 0.085;
// every aspect instead (chart(date, { all: true })): the tightest brightest (ORB_FADE of the colour lost by MAX_ORB), as many as
// the device's 128 items hold (the chart's other items are 58, an aspect's 6)
const ORB_FADE = 0.7, MOST = Math.floor((128 - 58) / 6);
// What a tap reads (a spot) and what the close-up says: a planet's part of you, played out in its sign's way.
const DOES = { Sun: 'you shine', Moon: 'you feel at home', Mercury: 'your mind works best', Venus: 'you love', Mars: 'you go after things', Jupiter: 'luck finds you', Saturn: 'you learn hard lessons', Uranus: 'you break old rules', Neptune: 'you dream', Pluto: 'you change deep down' };
const PART = { Sun: 'your core self', Moon: 'your feelings', Mercury: 'your mind', Venus: 'your loves', Mars: 'your drive', Jupiter: 'your luck', Saturn: 'your discipline', Uranus: 'your rebel side', Neptune: 'your dreams', Pluto: 'your power' };
// each sign: its nature, its ruler (modern), and two ways it plays out (alternating, so a crowded sign does not repeat itself)
const NATURE = [
  ['bold, quick', 'Mars', 'by being bold and going first', 'with courage and a fast start'],
  ['steady, sensual', 'Venus', 'through comfort and patience', 'by building things that last'],
  ['curious, chatty', 'Mercury', 'by talking and learning', 'through ideas and quick wits'],
  ['caring, tender', 'Moon', 'by caring for your people', 'through home and feeling safe'],
  ['warm, proud', 'Sun', 'with warmth and a big heart', 'by being seen and playing'],
  ['careful, helpful', 'Mercury', 'by getting the details right', 'through being useful'],
  ['fair, graceful', 'Venus', 'through fairness and beauty', 'by sharing and making peace'],
  ['deep, intense', 'Pluto', 'with deep, all or nothing focus', 'through trust and hidden things'],
  ['free, far-roaming', 'Jupiter', 'through adventure and big ideas', 'by searching for meaning'],
  ['patient, ambitious', 'Saturn', 'with patience and quiet ambition', 'by climbing step by step'],
  ['original, free-thinking', 'Uranus', 'by doing it your own way', 'through friends and new ideas'],
  ['dreamy, gentle', 'Neptune', 'through dreams and kindness', 'by feeling what others feel'],
];
const ELEMENTS = ['fire', 'earth', 'air', 'water'];
const TOUCH = { conjunction: 'sits right beside it, so the two blend', sextile: 'lends a hand', trine: 'makes it flow easily', square: 'adds friction that pushes you', opposition: 'pulls the other way, so look for balance' };
const MAX_SAY = 160; // a spot's reading (docs/PACKAGES.md)
const signIndex = (lon) => Math.floor(mod360(lon) / 30), deg = (lon) => mod360(lon) % 30;
/** The bodies in sign i, by degree. */
const inSign = (bodies, i) => bodies.filter((b) => signIndex(b.lon) === i).sort((a, b) => deg(a.lon) - deg(b.lon));
const and = (xs) => (xs.length > 1 ? `${xs.slice(0, -1).join(', ')} and ${xs.at(-1)}` : xs[0]);
/** The first of these lines that fits a spot. */
const fit = (...lines) => lines.find((l) => l.length <= MAX_SAY) ?? lines.at(-1).slice(0, MAX_SAY);
function bodyReading(b, bodies, aspects) {
  const i = signIndex(b.lon), rank = inSign(bodies, i).findIndex((x) => x.name === b.name), main = `${b.name} in ${SIGNS[i]}: ${DOES[b.name]} ${NATURE[i][2 + (rank % 2)]}.`;
  const x = aspects.find((a) => a.a === b.name || a.b === b.name);
  return x ? fit(`${main} ${x.a === b.name ? x.b : x.a} ${TOUCH[x.name]}.`, main) : main;
}
const lord = (i) => `${['Sun', 'Moon'].includes(NATURE[i][1]) ? 'the ' : ''}${NATURE[i][1]}`, signHead = (i) => `${SIGNS[i]}: ${NATURE[i][0]} ${ELEMENTS[i % 4]}, ruled by ${lord(i)}.`;
function signReading(i, bodies) {
  const here = inSign(bodies, i), head = signHead(i);
  if (!here.length) return fit(`${head} Empty now, so its ruler, ${lord(i)} in ${signOf(bodies.find((b) => b.name === NATURE[i][1]).lon)}, shows how it plays out for you.`, `${head} Empty now; ${lord(i)} speaks for it.`);
  const names = and(here.map((b) => b.name)), parts = and(here.map((b) => PART[b.name]));
  return fit(`${head} ${names} here: ${NATURE[i][2]}, you live out ${parts}.`, `${head} ${names} here, all ${NATURE[i][2]}.`, `${head} ${here.length} planets here.`);
}
const aspectReading = (a) => `${a.a} ${PAIR[a.name][0]} ${a.b}: ${PAIR[a.name][1](WORD[a.a], WORD[a.b])}.`;
const WORD = { Sun: 'self', Moon: 'mood', Mercury: 'mind', Venus: 'love', Mars: 'drive', Jupiter: 'luck', Saturn: 'limits', Uranus: 'change', Neptune: 'dreams', Pluto: 'power' };
const PAIR = { conjunction: ['sits with', (a, b) => `${a} and ${b} as one`], sextile: ['helps', (a, b) => `${a} helping ${b}`], square: ['squares', (a, b) => `friction between ${a} and ${b}`], trine: ['flows with', (a, b) => `${a} and ${b} in step`], opposition: ['opposes', (a, b) => `${a} meets ${b}`] };
const scaleInk = (c, f) => '#' + [1, 3, 5].map((k) => Math.round(parseInt(c.slice(k, k + 2), 16) * f).toString(16).padStart(2, '0')).join('');
/** The chart: every body on the zodiac, and between them the tightest aspects as lines (all: every one that fits). */
export function chart(date, { all = false } = {}) {
  const s = sky(date), placed = placeBodies(s.bodies), aspects = findAspects(s.bodies), DEG = Math.PI / 180;
  const at = (name) => { const b = placed.find((p) => p.name === name), a = chartAngle(b.lon) * DEG; return [b.r * Math.cos(a), b.r * Math.sin(a)]; };
  // a line from LINE_GAP outside one symbol's centre to LINE_GAP outside the other's, or null for a pair too close for one
  const line = (a) => {
    const P = at(a.a), Q = at(a.b), dx = Q[0] - P[0], dy = Q[1] - P[1], l = Math.hypot(dx, dy), ux = dx / l, uy = dy / l, id = `${a.a}-${a.b}`.toLowerCase();
    if (l < 2 * LINE_GAP + 0.03) return null;
    const c0 = TONE_INK[TONE[a.name]], c = all ? scaleInk(c0, 1 - ORB_FADE * Math.min(1, a.orb / MAX_ORB)) : c0;
    return { x1: r3(P[0] + ux * LINE_GAP), y1: r3(P[1] + uy * LINE_GAP), x2: r3(Q[0] - ux * LINE_GAP), y2: r3(Q[1] - uy * LINE_GAP), px1: r3(P[0]), py1: r3(P[1]), px2: r3(Q[0]), py2: r3(Q[1]), c, say: aspectReading(a), id, lights: `${id} ${a.a.toLowerCase()} ${a.b.toLowerCase()}` };
  };
  const lines = [], drawn = [];
  for (const a of aspects) {
    if (lines.length >= (all ? MOST : DRAWN)) break;
    const l = line(a);
    if (l) { lines.push(l); drawn.push(a); }
  }
  // a tap on a sign opens it up close on Molty itself (the layout's spots: open): the chart carries what that needs. Each
  // sign: its close-up's own fields and how long it plays; `ruler`: each sign's ruler glyph (of: the sign); each body: the
  // sign it is in (of) and its place and moment on that sign's dial (da, d, on, off)
  const ups = SIGNS.map((_, i) => closeupOf(s, aspects, i)), dial = Object.fromEntries(ups.flatMap((c) => c.params.bodies.map((b) => [b.id, { of: c.params.sign, da: b.da, d: b.d, on: b.on, off: b.off }])));
  const params = { bodies: placed.map((b) => ({ g: `g-${b.name.toLowerCase()}`, a: chartAngle(b.lon), r: r3(b.r), c: INK[b.name], say: bodyReading(b, s.bodies, aspects), id: b.name.toLowerCase(), ...dial[b.name.toLowerCase()] })), aspects: lines };
  params.signs = SIGNS.map((name, i) => { const { sign, c, name: label, nx, ns, say } = ups[i].params; return { a: (195 + 30 * i) % 360, say, id: sign, sign, c, name: label, nx, ns, ttl: ups[i].ttl }; });
  params.ruler = ups.map((c) => ({ of: c.params.sign, ...c.params.ruler[0] }));
  return { scenes: [['chart', params]], say: `The whole sky: ten bodies on the zodiac, joined by the ${drawn.length} tightest of their ${aspects.length} aspects. Tap a planet, a sign or a line to read it.`, facts: { bodies: Object.fromEntries(s.bodies.map((b) => [b.name, `${Math.floor(mod360(b.lon) % 30)} ${signOf(b.lon)}`])), aspects: aspects.map((a) => `${a.a} ${a.name} ${a.b} (orb ${a.orb.toFixed(1)})`), drawn: drawn.map((a) => `${a.a} ${a.name} ${a.b}`), tap: 'a tap on a body or an aspect line shows its reading; a tap on a sign opens it up close on Molty itself (the tap event says so: opened), and a tap beside it there comes back; the tap event names the spot; chart --interactive reads the sign aloud' } };
}
// The close-up reads aloud (a say over the scene) while each planet lights in turn. Molty's voice cues are not
// visible to a layout, so the lighting follows an estimate of the voice: about 2.6 words a second, a beat at each
// sentence end, after a lead-in while the first words are synthesised.
const SPEECH = { lead: 1.2, wps: 2.6, stop: 0.3, hold: 3 };
const speak = (line) => line.split(/\s+/).length / SPEECH.wps + (line.match(/[.!?](\s|$)/g)?.length ?? 0) * SPEECH.stop;
/** The sign's degree dial: 0 at the lower left, over the top, 30 at the lower right (the subtitle keeps the bottom). */
const DIAL = { a0: 210, span: 240, gap: 28 };
const dim = (c) => '#' + [1, 3, 5].map((k) => Math.round(parseInt(c.slice(k, k + 2), 16) * 0.6).toString(16).padStart(2, '0')).join('');
/** Dial angles for these degrees (ascending), at least `gap` apart and on the dial. */
function spread(degs) {
  const gap = Math.min(DIAL.gap, DIAL.span / Math.max(1, degs.length - 1)), a = degs.map((d) => DIAL.a0 - (d * DIAL.span) / 30), end = DIAL.a0 - DIAL.span;
  for (let k = 1; k < a.length; k++) a[k] = Math.min(a[k], a[k - 1] - gap);
  for (let k = a.length - 1; k >= 0 && a[k] < end + (a.length - 1 - k) * gap; k--) a[k] = end + (a.length - 1 - k) * gap;
  return a.map(r2);
}
/** The device font's width of a text item (5 x 7 letters, 1 apart), in glass units at `size`. */
const textWidth = (str, size) => ((str.length * 6 - 1) * size) / 7;
/** Sign i up close for this sky: the close-up's params (its planets on its degree dial, `da`, lit one by one as Molty reads
 *  them), how long it plays and what is read aloud. */
function closeupOf(s, aspects, i) {
  const id = SIGNS[i].toLowerCase(), here = inSign(s.bodies, i), angles = spread(here.map((b) => deg(b.lon)));
  const intro = here.length ? signHead(i) : signReading(i, s.bodies), reads = here.map((b) => bodyReading(b, s.bodies, aspects));
  let t = SPEECH.lead + speak(intro);
  const bodies = here.map((b, k) => { const on = r2(t); t += speak(reads[k]); return { g: `g-${b.name.toLowerCase()}`, da: angles[k], c: INK[b.name], d: dim(INK[b.name]), on, off: r2(t), say: reads[k], id: b.name.toLowerCase() }; });
  // the name and the ruler's glyph after it, centred as one row (a long name in smaller type)
  const name = SIGNS[i].toUpperCase(), ruler = NATURE[i][1], ns = name.length > 8 ? 0.11 : 0.13, w = textWidth(name, ns), half = (w + RULER_GAP + RULER) / 2;
  const params = { sign: id, c: SIGN_INK[i % 3], name, nx: r3(w / 2 - half), ns, say: signReading(i, s.bodies), ruler: [{ g: `g-${ruler.toLowerCase()}`, x: r3(half - RULER / 2), c: INK[ruler] }], bodies };
  return { params, ttl: Math.ceil(t + SPEECH.hold), read: [intro, ...reads.map((r) => r.replace(` in ${SIGNS[i]}:`, ':'))].join(' ') };
}
/** A sign up close: its glyph, its planets on its degree dial lit one by one as Molty reads them, its ruler. */
export function closeup(date, id) {
  const i = SIGNS.findIndex((n) => n.toLowerCase() === id);
  if (i < 0) throw new Error(`sign must be one of ${SIGNS.join(', ').toLowerCase()}`);
  const s = sky(date), c = closeupOf(s, findAspects(s.bodies), i);
  return { scenes: [['sign-closeup', c.params, c.ttl]], read: c.read };
}
const FIG = { sextile: 6, square: 4, trine: 3, opposition: 2, conjunction: 1 };
const GRID = '#565b72';
export function aspect(kind, date) {
  if (!FIG[kind]) throw new Error(`aspect must be one of ${Object.keys(FIG).join(', ')}`);
  const x = aspectOf(kind, date), R = 0.6, sA = chartAngle(x.lonA), sB = chartAngle(x.lonB);
  let d = sB - sA; while (d > 180) d -= 360; while (d < -180) d += 360;
  const n = FIG[kind], seg = (p, q, c, on) => ({ x1: p[0], y1: p[1], x2: q[0], y2: q[1], c, on });
  const corner = (i) => polar(sA + (i * 360) / n, R), lines = n >= 3 ? Array.from({ length: n }, (_, i) => seg(corner(i), corner(i + 1), GRID, 0)) : [];
  if (x.angle > 0) lines.push(seg(polar(sA, R), polar(sB, R), x.c, 1.2));
  const [tx, ty] = x.angle > 0 ? polar(sA + d / 2, 0.14) : [0, 0];
  const bodies = [{ g: `g-${x.a.toLowerCase()}`, a: sA, r: R, c: INK[x.a] }, { g: `g-${x.b.toLowerCase()}`, a: sB, r: Math.abs(d) < 14 ? 0.47 : R, c: INK[x.b] }];
  const params = { a: sA, b: r2(sA + d), c: x.c, bodies, lines, deg: String(Math.max(1, Math.round(x.sep))), tx, ty };
  const name = kind[0].toUpperCase() + kind.slice(1);
  return { scenes: [['aspect', params]], say: `${name}: ${x.a} and ${x.b}, ${Math.round(x.sep)} degrees apart.${x.tonight ? '' : ` Next on ${fmt(x.when)}.`}`, facts: { kind, a: x.a, b: x.b, sep: r2(x.sep), orb: r2(x.orb), when: x.when.toISOString(), tonight: x.tonight } };
}

// ---- events: real geometry, as an offset on the sky from the centre body (north up, east left)
const geoEq = (name, date) => A.EquatorFromVector(A.GeoVector(A.Body[name], date, true));
const eqOf = (name, date, obs) => A.Equator(A.Body[name], date, obs, true, true);
const offset = (body, centre, date, obs) => { const b = obs ? eqOf(body, date, obs) : geoEq(body, date), c = obs ? eqOf(centre, date, obs) : geoEq(centre, date); let dra = (b.ra - c.ra) * 15; if (dra > 180) dra -= 360; if (dra < -180) dra += 360; return [-dra * Math.cos(c.dec * RAD), b.dec - c.dec]; };
const radiusDeg = (km, au) => Math.atan(km / (au * 149597870.7)) / RAD;
const R_KM = { Sun: 695700, Moon: 1737.4 };
const clamp2 = (v) => Math.max(-2, Math.min(2, v)); // off the glass either way; the device takes -2..2
/** n samples of a moving body as keyframes over a layout of `dur` s: {x, y, on, off}. */
const keyframes = (fn, dur = 6, n = 14) => Array.from({ length: n }, (_, i) => { const [x, y] = fn(i / (n - 1)), on = 0.3 + ((dur - 0.6) * i) / n; return { x: r2(clamp2(x)), y: r2(clamp2(y)), on: r2(on), off: r2(i === n - 1 ? dur : 0.3 + ((dur - 0.6) * (i + 1)) / n) }; });
const across = (body, centre, t0, t1, scale, obs) => (u) => offset(body, centre, new Date(t0 + (t1 - t0) * u), obs).map((v) => v * scale);
const angle = (x, y, d) => A.AngleBetween(A.GeoVector(A.Body[x], d, true), A.GeoVector(A.Body[y], d, true));
const closest = (x, y, d, stepMs, n) => { let best = d, bs = angle(x, y, d); for (let k = -n; k <= n; k++) { const dd = new Date(d.getTime() + k * stepMs), v = angle(x, y, dd); if (v < bs) { bs = v; best = dd; } } return [best, bs]; };

const EVENT_BUILDERS = {
  transit(date) {
    const { p, e } = ['Mercury', 'Venus'].map((p) => ({ p, e: A.SearchTransit(A.Body[p], date) })).sort((a, b) => a.e.peak.date - b.e.peak.date)[0];
    const S = 0.55 / radiusDeg(R_KM.Sun, A.GeoVector(A.Body.Sun, e.peak.date, true).Length()), t0 = e.start.date.getTime(), t1 = e.finish.date.getTime(), pad = (t1 - t0) * 0.15;
    return { params: { path: keyframes(across(p, 'Sun', t0 - pad, t1 + pad, S)) }, say: `Next transit: ${p} crosses the Sun on ${fmt(e.peak.date)}.`, facts: { kind: 'transit', planet: p, when: e.peak.date.toISOString() } };
  },
  occultation(date) {
    for (let i = 0; i < 12 * 1500; i++) {
      const d = new Date(date.getTime() + i * 2 * 3600000);
      for (const p of ['Venus', 'Mars', 'Jupiter', 'Saturn']) {
        if (angle(p, 'Moon', d) > 0.9) continue;
        const [when, s] = closest(p, 'Moon', d, 120000, 60); if (s >= 0.22) continue;
        const k = 0.45 / radiusDeg(R_KM.Moon, A.GeoVector(A.Body.Moon, when, true).Length()), t = when.getTime();
        return { params: { planet: p.toLowerCase(), path: keyframes(across('Moon', p, t - 2.2 * 3600000, t + 2.2 * 3600000, k)) }, say: `The Moon covers ${p} on ${fmt(when)}.`, facts: { kind: 'occultation', planet: p, when: when.toISOString() } };
      }
    }
    throw new Error('no occultation');
  },
  appulse(date) {
    const P = ['Mercury', 'Venus', 'Mars', 'Jupiter', 'Saturn'];
    for (let i = 0; i < 365 * 8; i++) {
      const d = new Date(date.getTime() + i * DAY);
      for (let a = 0; a < P.length; a++) for (let b = a + 1; b < P.length; b++) {
        if (angle(P[a], P[b], d) > 1.2) continue;
        const [when, s] = closest(P[a], P[b], d, 3600000, 36); if (s >= 0.5 || A.AngleFromSun(A.Body[P[a]], when) <= 12) continue;
        const t = when.getTime();
        return { params: { a: P[a].toLowerCase(), b: P[b].toLowerCase(), path: keyframes(across(P[b], P[a], t - 4 * DAY, t + 4 * DAY, 0.32 / Math.max(s, 0.08))) }, say: `${P[a]} meets ${P[b]}, ${s.toFixed(1)} degrees apart, on ${fmt(when)}.`, facts: { kind: 'appulse', a: P[a], b: P[b], sep: r3(s), when: when.toISOString() } };
      }
    }
    throw new Error('no appulse');
  },
  'solar-eclipse'(date) {
    const e = A.SearchGlobalSolarEclipse(date), t = e.peak.date.getTime(), obs = e.latitude === undefined ? undefined : new A.Observer(e.latitude, e.longitude, 0);
    const dist = (b) => A.GeoVector(A.Body[b], e.peak.date, true).Length(), S = 0.5 / radiusDeg(R_KM.Sun, dist('Sun'));
    return { params: { moon_r: r3(radiusDeg(R_KM.Moon, dist('Moon')) * S), path: keyframes(across('Moon', 'Sun', t - 2.6 * 3600000, t + 2.6 * 3600000, S, obs)) }, say: `Next solar eclipse: ${e.kind}, on ${fmt(e.peak.date)}.`, facts: { kind: 'solar-eclipse', type: e.kind, when: e.peak.date.toISOString() } };
  },
  'lunar-eclipse'(date) {
    let e = A.SearchLunarEclipse(date); while (e.kind !== 'total') e = A.NextLunarEclipse(e.peak);
    const t = e.peak.date.getTime(), au = A.GeoVector(A.Body.Moon, e.peak.date, true).Length(), su = A.GeoVector(A.Body.Sun, e.peak.date, true).Length();
    const par = Math.asin(6378.14 / (au * 149597870.7)) / RAD, parS = Math.asin(6378.14 / (su * 149597870.7)) / RAD, rs = radiusDeg(R_KM.Sun, su), umbra = 1.02 * (par + parS - rs), pen = 1.02 * (par + parS + rs), S = 0.8 / pen;
    const anti = (d) => { const q = geoEq('Sun', d); return { ra: (q.ra + 12) % 24, dec: -q.dec }; };
    const off = (u) => { const d = new Date(t + (u - 0.5) * 2 * 3.2 * 3600000), m = geoEq('Moon', d), c = anti(d); let dra = (m.ra - c.ra) * 15; if (dra > 180) dra -= 360; if (dra < -180) dra += 360; return [-dra * Math.cos(c.dec * RAD) * S, (m.dec - c.dec) * S]; };
    // inside the Earth's shadow the moon turns red
    const path = keyframes(off).map((k) => ({ ...k, img: Math.hypot(k.x, k.y) < umbra * S ? 'moon-red' : 'moon' }));
    return { params: { umbra: r3(umbra * S), pen: r3(pen * S), moon_r: r3(radiusDeg(R_KM.Moon, au) * S), path }, say: `Next total lunar eclipse: ${fmt(e.peak.date)}.`, facts: { kind: 'lunar-eclipse', when: e.peak.date.toISOString(), minutes_total: Math.round(e.sd_total) } };
  },
};

/** Each event kind as a one-scene builder, like the others. */
export const EVENTS = Object.fromEntries(Object.entries(EVENT_BUILDERS).map(([k, f]) => [k, (date) => { const { params, ...rest } = f(date); return { scenes: [[k, params]], ...rest }; }]));
