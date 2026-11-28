// SPDX-License-Identifier: MIT
// The atlas's sky maths against known values (run by device/firmware/test/host/atlas_build.sh): where a star is from the
// textbook formulas (sidereal time, the horizon), when Orion is highest from its transit, the Moon's phase from the
// molty-astro skill's own maths, a meteor shower's peak from the IMO's calendar, and the wire's shape.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import * as sky from './sky.mjs';
import { sky as astroSky } from '../../molty-astro/scripts/sky.mjs';
import { CAT, byId, load, object } from './wire.mjs';

const R = Math.PI / 180;
/** Altitude and azimuth from the textbook: Greenwich sidereal time (Meeus 12.4), the hour angle, J2000 place (no
 *  precession, no refraction: within half a degree in 2026 above 15 degrees). */
function textbook(raDeg, dec, date, lat, lon) {
  const jd = date.getTime() / 86400000 + 2440587.5, T = (jd - 2451545) / 36525;
  const gmst = 280.46061837 + 360.98564736629 * (jd - 2451545) + 0.000387933 * T * T;
  const H = (gmst + lon - raDeg) * R, d = dec * R, p = lat * R;
  const alt = Math.asin(Math.sin(p) * Math.sin(d) + Math.cos(p) * Math.cos(d) * Math.cos(H));
  const az = Math.atan2(-Math.cos(d) * Math.sin(H), Math.sin(d) * Math.cos(p) - Math.cos(d) * Math.sin(p) * Math.cos(H));
  return { alt: alt / R, az: ((az / R) % 360 + 360) % 360 };
}
const PLACES = { berlin: { lat: 52.5, lon: 13.4, tz: 'Europe/Berlin' }, sydney: { lat: -33.9, lon: 151.2, tz: 'Australia/Sydney' }, quito: { lat: -0.2, lon: -78.5, tz: 'America/Guayaquil' } };

test('a star is where the textbook puts it', () => {
  const cases = [['star-betelgeuse', 'berlin', '2026-11-27T21:00:00Z'], ['star-vega', 'berlin', '2026-11-27T17:00:00Z'], ['m45', 'berlin', '2026-11-27T21:00:00Z'],
    ['star-antares', 'sydney', '2026-06-01T12:00:00Z'], ['star-acrux', 'sydney', '2026-06-01T12:00:00Z'], ['star-sirius', 'quito', '2026-01-15T04:00:00Z']];
  for (const [id, at, when] of cases) {
    const o = byId.get(id), d = new Date(when), p = PLACES[at], got = sky.pos(o, d, p), want = textbook(o.ra, o.dec, d, p.lat, p.lon);
    assert.ok(want.alt > 15, `${id} ${at}: up (${want.alt.toFixed(1)})`);
    // apart on the sky (an azimuth near the zenith moves a lot for a little): under 0.6 degrees
    const apart = Math.acos(Math.min(1, Math.sin(got.alt * R) * Math.sin(want.alt * R) + Math.cos(got.alt * R) * Math.cos(want.alt * R) * Math.cos((got.az - want.az) * R))) / R;
    assert.ok(apart < 0.6, `${id} ${at}: ${got.alt.toFixed(2)} up at ${got.az.toFixed(2)} vs ${want.alt.toFixed(2)} at ${want.az.toFixed(2)}`);
  }
});

test('Orion is highest at its transit, tonight in Berlin', () => {
  const o = byId.get('con-ori'), now = new Date('2026-11-27T21:00:00Z'), n = sky.night(o, now, PLACES.berlin);
  // the transit: the hour angle 0, found from the textbook's sidereal time
  let best = 0, at = 0;
  for (let m = 0; m < 14 * 60; m += 2) { const a = textbook(o.ra, o.dec, new Date(now.getTime() + m * 60e3), 52.5, 13.4).alt; if (a > best) { best = a; at = m; } }
  assert.ok(Math.abs(n.high - at) <= 15, `highest ${n.high.toFixed(0)} min from now, transit at ${at}`);
  assert.ok(Math.abs(n.highAlt - best) < 1, `highest at ${n.highAlt.toFixed(1)}, transit altitude ${best.toFixed(1)}`);
  assert.equal(sky.clock(new Date(now.getTime() + n.high * 60e3), 'Europe/Berlin'), '01:10');
  assert.ok(n.up && !n.never && n.set != null && n.rise != null);
});

test('the Moon lit as the molty-astro skill has it; the Geminids peak on December 14', () => {
  for (const when of ['2026-11-24T21:00:00Z', '2026-11-30T21:00:00Z', '2027-03-03T12:00:00Z']) {
    const d = new Date(when), mine = sky.body({ body: 'Moon' }, d, PLACES.berlin).lit, astro = astroSky(d).moon.fraction;
    assert.ok(Math.abs(mine - astro) < 1e-6, `${when}: ${mine} vs ${astro}`);
  }
  const peak = sky.peak(byId.get('shower-geminids'), new Date('2026-11-27T21:00:00Z'));
  assert.equal(peak.toISOString().slice(0, 10), '2026-12-14');
});

test('what the wire carries fits the device', () => {
  const now = new Date('2026-11-27T21:00:00Z');
  for (const k of CAT.kinds) {
    const a = load(k.id, now, PLACES.berlin, 'fame', true), bytes = Buffer.byteLength(JSON.stringify(a));
    assert.ok(bytes <= 32768, `${k.id}: ${bytes} bytes over the relay's 32 KB`);
    assert.ok(a.objects.length <= 96 && a.kinds.length === 10);
    for (const o of a.objects) {
      assert.ok(o.name.length <= 21 && o.facts.length <= 22, `${o.id}: ${o.name} / ${o.facts}`);
      assert.ok(o.w && Buffer.from(o.w.p, 'base64').length === 32, `${o.id}: 16 path points`);
    }
  }
  assert.equal(object(byId.get('m31'), now, null).w, undefined, 'no place: no path');
});
