// SPDX-License-Identifier: MIT
// The catalogue (../data/catalog.json) and tonight's sky as atlas.load and atlas.show args (references/commands.md):
// per object a name, two numbers, what shows it, its drawing and, with a place, its path in the sky tonight.
import { readFileSync } from 'node:fs';
import * as sky from './sky.mjs';

export const CAT = JSON.parse(readFileSync(new URL('../data/catalog.json', import.meta.url), 'utf8'));
export const byId = new Map(CAT.objects.map((o) => [o.id, o]));
const FIT = 0.56, PATH_STEP = 45, PATH_N = 16;

/** A kind's objects in the wheel's order: best known first, or with a place and `up`, the ones up now first (highest first). */
export function list(kind, order, now, place) {
  const all = CAT.objects.filter((o) => o.kind === kind).sort((a, b) => a.rank - b.rank);
  if (order !== 'up' || !place) return all;
  const alt = new Map(all.map((o) => [o.id, sky.pos(o, now, place).alt]));
  return [...all.filter((o) => alt.get(o.id) > 0).sort((a, b) => alt.get(b.id) - alt.get(a.id)), ...all.filter((o) => !(alt.get(o.id) > 0))];
}
export const glassName = (o) => o.glass ?? o.name;

/** The card's two numbers, plain: brightness, then how far or how big (full Moons, hands at arm's length). */
export function facts(o, b, now, tz) {
  const m = (v) => `MAG ${v.toFixed(1)}`;
  if (o.kind === 'planet') return o.body === 'Moon' ? `${Math.round(b.lit * 100)}% LIT  ${Math.round(b.km / 1000)}K KM` : `${m(b.mag)}  ${b.km > 1e9 ? (b.km / 1e9).toFixed(1) + 'BN' : Math.round(b.km / 1e6) + 'M'} KM`;
  if (o.kind === 'constellation') { const h = Math.max(1, Math.round(o.span / 20)); return `${h} HAND${h > 1 ? 'S' : ''} WIDE`; }
  if (o.kind === 'star') return `${m(o.mag)}  ${o.ly ? o.ly + ' LY' : ''}`.trim();
  if (o.kind === 'double') return `MAG ${o.mag.toFixed(1)} + ${o.mag2.toFixed(1)}`;
  if (o.kind === 'shower') { const p = sky.peak(o, now); return `${p ? p.toLocaleDateString('en-GB', { month: 'short', day: 'numeric', timeZone: tz ?? 'UTC' }).toUpperCase() + '  ' : ''}${o.zhr} AN HOUR`; }
  const size = o.size ? (o.size >= 15 ? `${(o.size / 30).toFixed(o.size >= 60 ? 0 : 1)} MOON${o.size >= 45 ? 'S' : ''}` : `${o.size.toFixed(o.size < 3 ? 1 : 0)} ARCMIN`) : '';
  return [o.mag != null ? m(o.mag) : '', size].filter(Boolean).join('  ');
}

/** Where a place on the sky falls in a constellation's drawing (make.mjs's tangent plane: north up, east left). */
function inFigure(con, raDeg, dec) {
  const R = Math.PI / 180, a0 = con.ra * R, d0 = con.dec * R, a = raDeg * R, d = dec * R;
  const c = Math.sin(d0) * Math.sin(d) + Math.cos(d0) * Math.cos(d) * Math.cos(a - a0);
  const x = (Math.cos(d) * Math.sin(a - a0)) / c, y = (Math.cos(d0) * Math.sin(d) - Math.sin(d0) * Math.cos(d) * Math.cos(a - a0)) / c;
  const s = FIT / con.reach;
  return [+((-x / R) * s).toFixed(3), +((y / R) * s).toFixed(3)];
}
/** The drawing as Molty takes it. b: a body's look tonight (the Moon, a planet). */
export function drawing(o, b) {
  const d = o.draw;
  if (d.ref) { const con = byId.get(d.ref); return { k: 'stars', p: con.draw.p, l: con.draw.l, focus: d.focus }; }
  if (d.k === 'radiant') {
    const con = byId.get(`con-${sky.constellation(o.ra, o.dec).toLowerCase()}`);
    return { k: 'radiant', p: con.draw.p, l: con.draw.l, at: inFigure(con, o.ra, o.dec) };
  }
  if (d.k === 'globe') return { k: 'globe', tint: d.tint, lit: +b.lit.toFixed(3), side: b.side, ...(o.body === 'Saturn' ? { rings: +b.rings.toFixed(1) } : {}) };
  const { credit, ...wire } = d;
  void credit;
  // the full Moon at the drawing's scale: a picture or a model is its catalogue size across, real stars a degree each way
  const reach = d.k === 'stars' ? (o.kind === 'open' ? 60 : 0) : o.size ? o.size / 2 : 0;
  return reach ? { ...wire, moon: +((15 / reach) * FIT).toFixed(3) } : wire;
}
/** Tonight's path as two bytes a point (azimuth / 2, altitude + 90), times in minutes from now (-1: none). */
function tonight(o, now, place) {
  const n = sky.night(o, now, place, PATH_STEP, PATH_N);
  const bytes = Buffer.from(n.path.flatMap(([az, alt]) => [Math.round((((az % 360) + 360) % 360) / 2) % 180, Math.max(0, Math.min(180, Math.round(alt) + 90))]));
  const t = (v) => (v == null ? -1 : Math.round(v));
  return { p: bytes.toString('base64'), step: PATH_STEP, t: [t(n.rise), t(n.high), t(n.set)], ha: Math.round(n.highAlt), ...(n.never ? { never: true } : {}) };
}
export function object(o, now, place) {
  const b = o.body ? sky.body(o, now, place ?? { lat: 0, lon: 0 }) : null;
  return { id: o.id, name: glassName(o), facts: facts(o, b, now, place?.tz), see: o.see, d: drawing(o, b), ...(place ? { w: tonight(o, now, place) } : {}) };
}
/** atlas.load for one kind (its objects in the wheel's order); with kinds, the ten kinds first (a new atlas). */
export function load(kind, now, place, order, withKinds) {
  const objs = list(kind, order, now, place);
  const args = { kind, objects: objs.map((o) => object(o, now, place)) };
  if (withKinds) {
    args.kinds = CAT.kinds.map((k) => ({ id: k.id, name: k.short, tint: k.tint, count: k.count, ...(place ? { up: CAT.objects.filter((o) => o.kind === k.id && sky.pos(o, now, place).alt > 0).length } : {}) }));
    args.clock = place ? sky.minutes(now, place.tz) : -1;
  }
  return args;
}
