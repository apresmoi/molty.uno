// SPDX-License-Identifier: MIT
// The sky for a date, as the params of the astrology package's layouts (packages/astrology).
// All astronomy comes from astronomy-engine (MIT, vendored); nothing here draws. Ported from the
// lab prototype (molty-creature-lab/astronomy/sky.mjs): same instants, aspects, orbs and events.
import { createRequire } from 'node:module';
const A = createRequire(import.meta.url)('./vendor/astronomy.min.js');

const TAU = Math.PI * 2, DAY = 86400000, RAD = Math.PI / 180, mod360 = (d) => ((d % 360) + 360) % 360;
const r2 = (v) => +v.toFixed(2) + 0, r3 = (v) => +v.toFixed(3) + 0;
export const BODIES = ['Sun', 'Moon', 'Mercury', 'Venus', 'Mars', 'Jupiter', 'Saturn', 'Uranus', 'Neptune', 'Pluto'];
// the package's glyph colours (packages/astrology/build.mjs BODIES)
const INK = { Sun: '#ffd678', Moon: '#e6e6e0', Mercury: '#c8bcb0', Venus: '#fff0c4', Mars: '#ff7a55', Jupiter: '#ffc266', Saturn: '#e8d890', Uranus: '#9fe3e0', Neptune: '#8fb4ff', Pluto: '#c0a0d0' };
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
export function placeBodies(bodies, rBase = 0.64) { const placed = []; return [...bodies].sort((a, b) => a.lon - b.lon).map((b) => { let level = 0; while (placed.some((p) => p.level === level && sep(p.lon, b.lon) < 13)) level++; const it = { ...b, level, r: rBase - level * 0.11 }; placed.push(it); return it; }); }

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
const KIND_LIST = { sextile: 'sextiles', square: 'squares', trine: 'trines', opposition: 'oppositions' };
/** A command's params stay small: the loosest aspects are left off the wheel until it fits. */
export const MAX_PARAMS_BYTES = 900;
export function chart(date) {
  const s = sky(date), placed = placeBodies(s.bodies), inner = Math.min(...placed.map((b) => b.r)) - 0.07, aspects = findAspects(s.bodies);
  const line = (a) => { const [x1, y1] = polar(chartAngle(a.lonA), inner), [x2, y2] = polar(chartAngle(a.lonB), inner); return { x1: r2(x1), y1: r2(y1), x2: r2(x2), y2: r2(y2) }; };
  const build = (n) => { const params = { inner: r3(inner), bodies: placed.map((b) => ({ g: `g-${b.name.toLowerCase()}`, a: chartAngle(b.lon), r: r3(b.r), c: INK[b.name] })), ...Object.fromEntries(Object.values(KIND_LIST).map((k) => [k, []])) }; for (const a of aspects.filter((a) => a.angle > 0).slice(0, n)) params[KIND_LIST[a.name]].push(line(a)); return params; };
  let n = 32, params = build(n);
  while (n > 0 && JSON.stringify(params).length > MAX_PARAMS_BYTES) params = build(--n);
  return { scenes: [['chart', params]], say: `The whole sky: ten bodies on the zodiac, ${aspects.length} aspects between them.`, facts: { bodies: Object.fromEntries(s.bodies.map((b) => [b.name, `${Math.floor(mod360(b.lon) % 30)} ${signOf(b.lon)}`])), aspects: aspects.map((a) => `${a.a} ${a.name} ${a.b} (orb ${a.orb.toFixed(1)})`), drawn: n } };
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
/** n samples of a moving body as keyframes over a layout of `dur` s: {x, y, on, off}. */
const keyframes = (fn, dur = 6, n = 14) => Array.from({ length: n }, (_, i) => { const [x, y] = fn(i / (n - 1)), on = 0.3 + ((dur - 0.6) * i) / n; return { x: r2(x), y: r2(y), on: r2(on), off: r2(i === n - 1 ? dur : 0.3 + ((dur - 0.6) * (i + 1)) / n) }; });
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
