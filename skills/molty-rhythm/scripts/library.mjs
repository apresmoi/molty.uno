// SPDX-License-Identifier: MIT
// library.mjs: the rhythm library (../rhythms/*.json, references/library.md) loaded, checked and turned into what
// Molty's beat player takes (rhythm.play's `rhythms`). Every file is checked on every load: a bad one stops the script.
import { readdirSync, readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

export const DIR = join(dirname(fileURLToPath(import.meta.url)), '../rhythms');
export const SOUNDS = ['low', 'head', 'rim', 'clap', 'bell', 'hat', 'snare', 'click', 'tom', 'slap', 'shaker'];
/** A drum's colour on Molty by its kind of sound, the same in every rhythm (a learner reads the sound by its colour):
 *  warm for the drums' skins, cool for wood and metal, amber and lime for hands and seeds, white for the count. Each
 *  at least 24 apart (CIE76) from every other, all lit on the black glass (references/library.md, Colours). */
export const COLOR = {
  low: '#ff6a5c',     // coral red: the bass drum, the bombo's low, a darbuka's dum
  head: '#ffa36b',    // apricot: a drum's head, the parche
  tom: '#ff86c8',     // pink: a hand drum's open tone
  slap: '#d58cff',    // violet: a hand drum's slap, a darbuka's tek
  snare: '#a3a0ff',   // periwinkle: the snare
  rim: '#5fb0ff',     // blue: the rim, the aro, sticks, the clave
  hat: '#5fe0d6',     // teal: the hi-hat, the ride, the triangle
  bell: '#86df6e',    // green: a bell, the agogo, the cowbell
  shaker: '#d9e85e',  // lime: shakers, maracas, the guacharaca
  clap: '#ffcf4a',    // amber: hand claps, palmas
  click: '#ffffff',   // white: the count, a metronome
};
/** The library's regions, in the order Molty's wheel steps through them: id, the label on Molty (capitals, up to 12),
 *  the spoken name, and a quiet tint of its own (its beads and its label on the wheel). */
export const REGIONS = [
  { id: 'argentina', label: 'ARGENTINA', name: 'Argentina', color: '#8cc4ff' },
  { id: 'south-america', label: 'LATIN AM', name: 'Latin America (beyond Argentina)', color: '#ffb38a' },
  { id: 'caribbean', label: 'CARIBBEAN', name: 'The Caribbean', color: '#4fd1c5' },
  { id: 'brazil', label: 'BRAZIL', name: 'Brazil', color: '#7ed957' },
  { id: 'mexico', label: 'MEXICO', name: 'Mexico', color: '#ff8f8f' },
  { id: 'spain', label: 'SPAIN', name: 'Spain, flamenco', color: '#ff7b6b' },
  { id: 'west-africa', label: 'WEST AFRICA', name: 'West Africa', color: '#e8b04a' },
  { id: 'middle-east', label: 'MIDDLE EAST', name: 'North Africa and the Middle East', color: '#d6a3ff' },
  { id: 'balkans', label: 'BALKANS', name: 'Turkey and the Balkans', color: '#ff9ecf' },
  { id: 'india', label: 'INDIA', name: 'India', color: '#ffb347' },
  { id: 'celtic', label: 'CELTIC', name: 'Ireland and Scotland', color: '#6ee7a8' },
  { id: 'usa', label: 'USA', name: 'The United States', color: '#a3b8ff' },
  { id: 'basics', label: 'BASICS', name: 'Basics: meters and polyrhythms', color: '#cfd3e0' },
];
export const LIMITS = { steps: 32, layers: 4, bars: 8, order: 16, label: 12, bpm: [40, 220], rhythms: 96, regions: 16 };

const KEYS = ['id', 'name', 'label', 'status', 'region', 'country', 'steps', 'lengths', 'swing', 'tempo', 'beats', 'count', 'layers', 'bars', 'phrase', 'source', 'sources'];
const isSteps = (a, n) => Array.isArray(a) && a.every((s) => Number.isInteger(s) && s >= 1 && s <= n) && new Set(a).size === a.length;

/** The problems with one rhythm file (none: []). */
export function check(r, file = r?.id) {
  const bad = [], need = (ok, m) => { if (!ok) bad.push(`${file}: ${m}`); };
  need(r && typeof r === 'object', 'not an object');
  if (!r || typeof r !== 'object') return bad;
  for (const k of Object.keys(r)) need(KEYS.includes(k), `unknown key ${k}`);
  need(/^[a-z0-9-]{1,24}$/.test(r.id ?? ''), 'id: a-z 0-9 -, up to 24');
  need(typeof r.name === 'string' && r.name.length > 0, 'name: the spoken name');
  need(/^[A-Z0-9 /-]{1,12}$/.test(r.label ?? ''), `label: capitals, up to ${LIMITS.label}`);
  need(['verified', 'unverified'].includes(r.status), 'status: verified or unverified');
  need(typeof r.source === 'string' && r.source.length > 8, 'source: where the pattern comes from (no source, no file)');
  need(REGIONS.some((g) => g.id === r.region), `region: one of ${REGIONS.map((g) => g.id).join(' ')}`);
  need(typeof r.country === 'string' && r.country.length > 1, 'country: where it comes from');
  if (r.sources !== undefined) need(Array.isArray(r.sources) && r.sources.every((x) => typeof x?.title === 'string' && /^https?:\/\//.test(x?.url ?? '') && typeof x?.shows === 'string'), 'sources: [{title, url, shows}]');
  if (r.status === 'verified' && r.sources !== undefined) need(r.sources.length >= 2, 'sources: two that agree, for verified');
  const n = r.steps;
  need(Number.isInteger(n) && n >= 2 && n <= LIMITS.steps, `steps: 2..${LIMITS.steps}`);
  if (r.lengths !== undefined) need(Array.isArray(r.lengths) && r.lengths.length === n && r.lengths.every((v) => v > 0 && v <= 8), 'lengths: one number (0..8] per step');
  if (r.swing !== undefined) need(typeof r.swing === 'number' && r.swing >= 0 && r.swing <= 0.33, 'swing: 0..0.33');
  const t = r.tempo ?? {};
  need(Number.isFinite(t.bpm) && t.bpm >= LIMITS.bpm[0] && t.bpm <= LIMITS.bpm[1], 'tempo.bpm: 40..220');
  need(Number.isFinite(t.per) && t.per > 0 && t.per <= 8, 'tempo.per: the units a beat is (0..8]');
  need(isSteps(r.beats, n) && r.beats.length > 0, 'beats: step numbers');
  const layers = Object.entries(r.layers ?? {});
  need(layers.length >= 1 && layers.length <= LIMITS.layers, `layers: 1..${LIMITS.layers}`);
  for (const [id, L] of layers) {
    need(/^[a-z][a-z0-9-]{0,15}$/.test(id), `layer ${id}: a-z 0-9 -, up to 16`);
    need(SOUNDS.includes(L.sound), `layer ${id}: sound one of ${SOUNDS.join(' ')}`);
    need(isSteps(L.hits, n) && L.hits.length > 0, `layer ${id}: hits`);
    if (L.accent !== undefined) need(isSteps(L.accent, n) && L.accent.every((s) => L.hits.includes(s)), `layer ${id}: accent must be some of its hits`);
    for (const k of Object.keys(L)) need(['name', 'sound', 'hits', 'accent'].includes(k), `layer ${id}: unknown key ${k} (a colour comes from the sound)`);
  }
  const bars = Object.entries(r.bars ?? {});
  need(bars.length <= LIMITS.bars, `bars: at most ${LIMITS.bars}`);
  for (const [name, b] of bars) {
    for (const [k, v] of Object.entries(b)) {
      if (k === 'only') need(Array.isArray(v) && v.every((l) => r.layers?.[l]), `bar ${name}: only names its layers`);
      else need(r.layers?.[k] && isSteps(v?.hits, n), `bar ${name}: ${k} is not a layer with hits`);
    }
  }
  if (r.phrase) {
    need(Array.isArray(r.phrase.order) && r.phrase.order.length >= 1 && r.phrase.order.length <= LIMITS.order && r.phrase.order.every((b) => r.bars?.[b]), `phrase.order: 1..${LIMITS.order} names of bars`);
    if (r.phrase.count_in !== undefined) need([0, 1, 2].includes(r.phrase.count_in), 'phrase.count_in: 0, 1 or 2 bars');
  }
  return bad;
}

/** Every rhythm, in the library's order (the wheel's): by region (REGIONS), inside one as the list below (the best
 *  known first), then any others by id. */
export const ORDER = ['chacarera', 'zamba', 'chamame', 'gato', 'escondido', 'cueca', 'chacarera-doble', 'chacarera-trunca', 'carnavalito',
  'vidala', 'rasguido-doble', 'milonga', 'tango-marcato', 'tango-sincopa', 'vals-criollo', 'candombe', 'cumbia', 'son-clave', 'rumba-clave', 'tresillo', 'cinquillo', 'habanera',
  'conga-tumbao', 'one-drop', 'ska', 'dembow', 'bossa-nova', 'samba', 'baiao', 'solea', 'buleria', 'seguiriya', 'fandango-huelva',
  'bell-12', 'gahu', 'ewe-axatse', 'kuku', 'soli', 'maqsum', 'baladi', 'saidi', 'malfuf', 'ayoub', 'chiftetelli', 'karsilama', 'aksak-9', 'rachenitsa', 'lesnoto',
  'paidushko', 'kopanitsa', 'postupano', 'teental', 'keherwa', 'dadra', 'rupak', 'jhaptal', 'irish-jig', 'irish-reel', 'slip-jig',
  'rock-backbeat', 'blues-shuffle', 'swing-ride', 'waltz', 'polka', 'march', 'three-against-two', 'four-against-three'];
export function load(dir = DIR) {
  const all = readdirSync(dir).filter((f) => f.endsWith('.json')).map((f) => {
    let r;
    try { r = JSON.parse(readFileSync(join(dir, f), 'utf8')); } catch (e) { throw new Error(`rhythms/${f}: ${e.message}`); }
    const bad = check(r, `rhythms/${f}`);
    if (r?.id && `${r.id}.json` !== f) bad.push(`rhythms/${f}: id ${r.id} differs from its file name`);
    if (bad.length) throw new Error(bad.join('\n'));
    return r;
  });
  const rank = (r) => (ORDER.includes(r.id) ? ORDER.indexOf(r.id) : ORDER.length), reg = (r) => REGIONS.findIndex((g) => g.id === r.region);
  return all.sort((a, b) => reg(a) - reg(b) || rank(a) - rank(b) || a.id.localeCompare(b.id));
}

/** One rhythm as Molty's beat player takes it (docs/AGENT-LINK.md "Rhythm"). */
/** The regions the library has rhythms in, in REGIONS' order. */
export const regionsOf = (lib) => REGIONS.filter((g) => lib.some((r) => r.region === g.id));
/** The whole library as rhythm.play takes it: {regions: [{label, color}], rhythms: [wire(r), each with its region's
 *  index]}. */
export function wireLibrary(lib) {
  const regions = regionsOf(lib);
  return { regions: regions.map((g) => ({ label: g.label, color: g.color })), rhythms: lib.map((r) => ({ ...wire(r), region: regions.findIndex((g) => g.id === r.region) })) };
}

export function wire(r) {
  const layers = Object.entries(r.layers).map(([id, L]) => ({ id, sound: L.sound, color: COLOR[L.sound], hits: L.hits, ...(L.accent?.length ? { accent: L.accent } : {}) }));
  const out = { id: r.id, label: r.label, steps: r.steps, per: r.tempo.per, beats: r.beats, layers };
  if (r.lengths) out.lengths = r.lengths;
  if (r.swing) out.swing = r.swing;
  if (r.phrase) {
    const names = Object.keys(r.bars);
    out.bars = names.map((name) => {
      const b = r.bars[name];
      return { name, hits: layers.map((L) => (b.only && !b.only.includes(L.id) ? [] : b[L.id]?.hits ?? L.hits)) };
    });
    out.order = r.phrase.order.map((name) => names.indexOf(name));
  }
  out.count_in = r.phrase?.count_in ?? 1;
  return out;
}
