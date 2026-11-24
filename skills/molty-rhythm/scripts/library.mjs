// SPDX-License-Identifier: MIT
// library.mjs: the rhythm library (../rhythms/*.json, references/library.md) loaded, checked and turned into what
// Molty's beat player takes (rhythm.play's `rhythms`). Every file is checked on every load: a bad one stops the script.
import { readdirSync, readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

export const DIR = join(dirname(fileURLToPath(import.meta.url)), '../rhythms');
export const SOUNDS = ['low', 'head', 'rim', 'clap', 'bell', 'hat', 'snare', 'click'];
/** A drum's colour on Molty by its sound (Molty's own palette), unless the file gives one. */
export const COLOR = { low: '#ff9a7a', head: '#ff9a7a', snare: '#ff9a7a', rim: '#9fc0ff', hat: '#9fc0ff', bell: '#a8d890', clap: '#ffd678', click: '#ffd678' };
export const LIMITS = { steps: 32, layers: 4, bars: 8, order: 16, label: 12, bpm: [40, 220] };

const KEYS = ['id', 'name', 'label', 'status', 'steps', 'lengths', 'swing', 'tempo', 'beats', 'count', 'layers', 'bars', 'phrase', 'source'];
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
    if (L.color !== undefined) need(/^#[0-9a-f]{6}$/.test(L.color), `layer ${id}: color #rrggbb`);
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

/** Every rhythm, in the library's order (the wheel's): the files sorted as the list below, then any others by id. */
export const ORDER = ['chacarera', 'chacarera-trunca', 'carnavalito', 'milonga', 'tango-marcato', 'habanera', 'son-clave', 'rumba-clave', 'bossa-nova', 'bell-12', 'solea', 'aksak-9'];
export function load(dir = DIR) {
  const all = readdirSync(dir).filter((f) => f.endsWith('.json')).map((f) => {
    let r;
    try { r = JSON.parse(readFileSync(join(dir, f), 'utf8')); } catch (e) { throw new Error(`rhythms/${f}: ${e.message}`); }
    const bad = check(r, `rhythms/${f}`);
    if (r?.id && `${r.id}.json` !== f) bad.push(`rhythms/${f}: id ${r.id} differs from its file name`);
    if (bad.length) throw new Error(bad.join('\n'));
    return r;
  });
  const rank = (r) => (ORDER.includes(r.id) ? ORDER.indexOf(r.id) : ORDER.length);
  return all.sort((a, b) => rank(a) - rank(b) || a.id.localeCompare(b.id));
}

/** One rhythm as Molty's beat player takes it (docs/AGENT-LINK.md "Rhythm"). */
export function wire(r) {
  const layers = Object.entries(r.layers).map(([id, L]) => ({ id, sound: L.sound, color: L.color ?? COLOR[L.sound], hits: L.hits, ...(L.accent?.length ? { accent: L.accent } : {}) }));
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
