// SPDX-License-Identifier: MIT
// Where things are for an observer, tonight: astronomy-engine (MIT), the one copy the molty-astro skill vendors (both
// skills ship in this repo's plugin; this one reads it from there, it keeps no second copy). Nothing here draws.
import { createRequire } from 'node:module';

export const A = createRequire(import.meta.url)('../../molty-astro/scripts/vendor/astronomy.min.js');
const HOUR = 3600e3, mod = (v, n) => ((v % n) + n) % n;

const obs = (p) => new A.Observer(p.lat, p.lon, 0);
/** Right ascension (hours) and declination of date: a fixed object's J2000 place precessed, a body's apparent place. */
function equ(o, date, place) {
  if (o.body) { const e = A.Equator(A.Body[o.body], date, obs(place), true, true); return { ra: e.ra, dec: e.dec, dist: e.dist }; }
  const t = A.MakeTime(date), v = A.VectorFromSphere(new A.Spherical(o.dec, o.ra, 1), t), e = A.EquatorFromVector(A.RotateVector(A.Rotation_EQJ_EQD(t), v));
  return { ra: e.ra, dec: e.dec };
}
/** Azimuth (from north through east) and altitude, degrees. */
export function pos(o, date, place) { const e = equ(o, date, place), h = A.Horizon(date, obs(place), e.ra, e.dec, 'normal'); return { az: h.azimuth, alt: h.altitude, dist: e.dist }; }
const sunAlt = (date, place) => pos({ body: 'Sun' }, date, place).alt;

/**
 * Tonight from `date`: sampled every 10 minutes over the next 24 hours. up: above the horizon now; rise, set: the next
 * crossings; high: when it is highest while the sky is dark (the Sun 12 degrees down) in the next 14 hours, else highest
 * at all; never: it does not rise; path: where it is every `step` minutes, `n` points from now (below the horizon too).
 */
export function night(o, date, place, step = 45, n = 16) {
  const t0 = date.getTime(), N = 144, alt = [], sun = [];
  for (let i = 0; i <= N; i++) { const d = new Date(t0 + (i * 24 * HOUR) / N); alt.push(pos(o, d, place).alt); sun.push(i % 3 ? sun[i - 1] : sunAlt(d, place)); }
  const at = (i) => (i * 24 * 60) / N;                 // minutes from now
  let rise = null, set = null;
  for (let i = 1; i <= N; i++) {
    if (rise == null && alt[i - 1] < 0 && alt[i] >= 0) rise = at(i - 1 + -alt[i - 1] / (alt[i] - alt[i - 1]));
    if (set == null && alt[i - 1] >= 0 && alt[i] < 0) set = at(i - 1 + alt[i - 1] / (alt[i - 1] - alt[i]));
  }
  let hi = -1, hiDark = -1;
  for (let i = 0; i <= 84; i++) { if (hi < 0 || alt[i] > alt[hi]) hi = i; if (sun[i] < -12 && alt[i] > 0 && (hiDark < 0 || alt[i] > alt[hiDark])) hiDark = i; }
  const h = hiDark >= 0 ? hiDark : hi;
  const path = [];
  for (let q = 0; q < n; q++) { const p = pos(o, new Date(t0 + q * step * 60e3), place); path.push([p.az, p.alt]); }
  return { up: alt[0] > 0, alt: alt[0], az: path[0][0], rise, set, high: at(h), highAlt: alt[h], never: Math.max(...alt) < 0, path, step };
}
/** A body's look tonight: lit fraction, the side lit (1 the right, as seen from the north), magnitude, km, ring tilt. */
export function body(o, date, place) {
  const B = A.Body[o.body], ill = A.Illumination(B, date), e = equ(o, date, place);
  const elon = (b) => (b === 'Sun' ? A.SunPosition(date).elon : A.Ecliptic(A.GeoVector(A.Body[b], date, true)).elon);
  const side = o.body === 'Moon' ? (A.MoonPhase(date) < 180 ? 1 : -1) : mod(elon(o.body) - elon('Sun'), 360) < 180 ? 1 : -1;
  return { lit: ill.phase_fraction, side, mag: ill.mag, km: e.dist * 149597870.7, rings: ill.ring_tilt ?? 0 };
}
/** A meteor shower's next peak: when the Sun reaches its longitude at the maximum. */
export const peak = (o, date) => A.SearchSunLongitude(o.sl, A.MakeTime(new Date(date.getTime() - 2 * 86400e3)), 370)?.date ?? null;
/** The IAU constellation (abbreviation) a place on the sky is in. */
export const constellation = (raDeg, dec) => A.Constellation(raDeg / 15, dec).symbol;
const DIRS = ['north', 'north-east', 'east', 'south-east', 'south', 'south-west', 'west', 'north-west'];
export const dir = (az) => DIRS[Math.round(mod(az, 360) / 45) % 8];
export const clock = (date, tz) => new Intl.DateTimeFormat('en-GB', { hour: '2-digit', minute: '2-digit', hour12: false, timeZone: tz }).format(date);
/** Minutes of the place's day now. */
export const minutes = (date, tz) => { const [h, m] = clock(date, tz).split(':').map(Number); return (h % 24) * 60 + m; };
