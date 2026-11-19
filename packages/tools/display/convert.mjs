// SPDX-License-Identifier: MIT
// molty-display convert (docs/DISPLAY.md "Moving from v1"): a molty-pkg/1 scenes package written out as
// a display v2 skill folder. Mechanical: every asset a PNG, every item its shape with the properties in
// the item's own order, each run of `each` a for block, so building the folder gives the package back.
// It cannot know what a param means: names stay, and every `about` is TODO for a person to fill in.
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { LIMITS, checkPackage } from '../format.mjs';
import { assetPng } from './assets.mjs';
import { COMMON, READS, SHAPES, TOKENS } from './compile.mjs';

const REPO = fileURLToPath(new URL('../../../', import.meta.url));
const KIND = { ring: 'circle', arc: 'arc', ticks: 'ticks', line: 'line', curve: 'line', disc: 'dot', ball: 'globe', glyph: 'symbol', text: 'label', spot: 'tap_zone' };
const HOLDS = { x: 'coord', y: 'coord', on: 'time', off: 'time' };   // v1 field -> what it holds, where a shape's table does not say
const SPLIT_AT = 5;   // screens: from here on, one file each

// ---- YAML, as much as these files need
const scalar = (v) => (typeof v !== 'string' ? String(v) : /^[A-Za-z_][A-Za-z0-9_ ./-]*$/.test(v) && !/^(true|false|null|yes|no|on|off|y|n)$/i.test(v) && !v.endsWith(' ') ? v : JSON.stringify(v));
const flow = (v) => (Array.isArray(v) ? `[${v.map(flow).join(', ')}]` : v && typeof v === 'object' ? (Object.keys(v).length ? `{ ${Object.entries(v).map(([k, x]) => `${k}: ${flow(x)}`).join(', ')} }` : '{}') : scalar(v));

/** The v2 name of a v1 name: itself when it is snake_case, else a snake_case stand-in (sent_as keeps the original). */
function namer() {
  const taken = new Set();
  return (v1) => {
    let n = /^[a-z][a-z0-9_]{0,23}$/.test(v1) ? v1 : `${/^[a-z]/i.test(v1) ? '' : 'v_'}${v1.toLowerCase().replace(/[^a-z0-9_]/g, '_')}`.slice(0, 22);
    while (taken.has(n)) n += '_';
    taken.add(n);
    return n;
  };
}

/** One layout as a v2 screen: { about, duration, subtitles?, inputs?, draw } with every key in the order it is written. */
function screenOf(l) {
  const name = namer(), inputs = {}, fields = {}, uses = {};   // uses: "$p" or "$list @f" -> the kinds of property that read it
  for (const p of Object.keys(l.params ?? {})) inputs[p] = name(p);
  const field = (lst, f) => ((fields[lst] ??= { name: namer(), of: {} }).of[f] ??= fields[lst].name(f));
  const val = (v, holds, it) => {
    if (typeof v !== 'string' || !/^[$@]/.test(v)) return v;
    (uses[v[0] === '$' ? v : `${it.each} ${v}`] ??= []).push(holds);
    return v[0] === '$' ? `{{ ${inputs[v.slice(1)]} }}` : `{{ item.${field(it.each, v.slice(1))} }}`;
  };
  const token = (cat, v) => Object.keys(TOKENS[cat]).find((t) => TOKENS[cat][t] === v) ?? v;
  function shapeOf(it) {
    const keys = Object.keys(it).filter((k) => k !== 'k' && k !== 'each'), spoke = it.k === 'ticks' && it.n === 1 && keys[keys.indexOf('n') + 1] === 'a0';
    const kind = spoke ? 'spoke' : KIND[it.k], props = {}, table = { ...SHAPES[kind][1], ...COMMON };
    const prop = (f) => Object.keys(table).find((p) => table[p][0] === f), at = ([x, y]) => ({ x: val(x, 'coord', it), y: val(y, 'coord', it) });
    for (const f of keys) {
      const v = it[f];
      if (f === 'x' || f === 'y') (props.at ??= {})[f] = val(v, 'coord', it);
      else if (f === 'polar') props.at = { angle: val(v[0], 'angle', it), distance: val(v[1], 'distance', it) };
      else if (f === 'on' || f === 'off') (props.show ??= {})[f === 'on' ? 'from' : 'until'] = val(v, 'time', it);
      else if (f === 'when') props.only_when = 'highlighted';
      else if (f === 'pts' && it.k === 'spot') props.along = { from: at(v[0]), to: at(v[1]) };
      else if (f === 'pts' && v.length === 2) [props.from, props.to] = v.map(at);
      else if (f === 'pts') props.points = v.map(at);
      else if (f === 'n' && spoke) continue;   // the spoke's angle carries it
      else if (f === 'side' && typeof v === 'number') props.lit_side = v < 0 ? 'left' : 'right';
      else if (f === 'color' && typeof v === 'string' && v[0] === '#') props.color = token('colors', v);
      else if (f === 'w' && typeof v === 'number') props.width = token('lines', v);
      else props[prop(f)] = val(v, table[prop(f)][1], it);
    }
    if (it.k === 'curve') props.smooth = true;
    return { [kind]: props };
  }
  // each run of items repeated over one list is a for block; `each` last in the item is `for` written first
  const draw = [], last = (it) => Object.keys(it).at(-1) === 'each';
  for (let i = 0; i < l.items.length;) {
    const it = l.items[i];
    let j = i + 1;
    if (it.each === undefined) { draw.push(shapeOf(it)); i = j; continue; }
    while (j < l.items.length && l.items[j].each === it.each && last(l.items[j]) === last(it)) j++;
    const block = { for: `item in ${inputs[it.each.slice(1)]}`, draw: l.items.slice(i, j).map(shapeOf) };
    draw.push(last(it) ? block : { draw: block.draw, for: block.for });
    i = j;
  }
  // a text param's type: the one a property that reads it asks for first, if every other one takes it too
  const typed = (v1, key) => (v1 === 'number' ? 'number' : (uses[key] ?? []).map((h) => READS[h][0]).find((t) => uses[key].every((h) => READS[h].includes(t))) ?? 'text');
  const decl = (v1, key, as, to) => ({ type: typed(v1, key), about: 'TODO', ...(as !== to ? { sent_as: to } : {}) });
  // a list's most elements: the device's 32, or fewer when the screen would not fit at its fullest
  const per = (lst) => l.items.filter((it) => it.each === lst).length, lists = Object.keys(l.params ?? {}).filter((p) => l.params[p] === 'list').map((p) => `$${p}`);
  const still = l.items.filter((it) => it.each === undefined).length, grouped = (lst) => new Set(l.items.filter((it) => it.each === lst && typeof it.group === 'string' && it.group[0] === '@').map((it) => it.group)).size;
  let most = LIMITS.list;
  while (most > 1 && (still + lists.reduce((n, p) => n + most * per(p), 0) > LIMITS.scene || lists.reduce((n, p) => n + most * grouped(p), 0) > LIMITS.groups)) most--;
  const labels = l.items.filter((it) => it.k === 'text' && typeof it.str === 'string' && it.str[0] === '$').map((it) => it.str), room = LIMITS.letters - l.items.filter((it) => it.k === 'text' && !/^[$@]/.test(it.str)).reduce((n, it) => n + it.str.length, 0);
  const made = {};
  for (const [p, t] of Object.entries(l.params ?? {})) {
    made[inputs[p]] = decl(t, `$${p}`, inputs[p], p);
    if (t === 'list') Object.assign(made[inputs[p]], { type: 'list', ...(most < LIMITS.list ? { max: most } : {}), of: Object.fromEntries(Object.entries(fields[`$${p}`]?.of ?? {}).map(([f, n]) => [n, decl(uses[`$${p} @${f}`]?.every((h) => READS[h].includes('number')) ? 'number' : 'text', `$${p} @${f}`, n, f)])) });
    else if (labels.includes(`$${p}`) && labels.length * LIMITS.text > room) made[inputs[p]].max = Math.max(1, Math.floor(room / labels.length));
  }
  return {
    about: 'TODO', duration: l.dur > 0 ? l.dur : 'hold',
    ...(l.caption_y === undefined ? {} : { subtitles: l.caption_y === 0 ? 'middle' : l.caption_y }),
    ...(l.params ? { inputs: made } : {}), draw, guessed: most < LIMITS.list,
  };
}

/** A screen's lines of YAML, indented by `pad`. */
function screenYaml(s, pad = '') {
  const out = [`about: ${s.about}`, `duration: ${s.duration}`];
  if ('subtitles' in s) out.push(`subtitles: ${s.subtitles}`);
  if (s.inputs && !Object.keys(s.inputs).length) out.push('inputs: {}');
  else if (s.inputs) {
    out.push('inputs:');
    if (s.guessed) out.push('  # TODO: each max below was set so the screen fits the device at its fullest; say what the skill really sends');
    for (const [n, d] of Object.entries(s.inputs)) {
      if (d.type !== 'list') { out.push(`  ${n}: ${flow(d)}`); continue; }
      const { of, ...rest } = d;
      out.push(`  ${n}:`, ...Object.entries(rest).map(([k, v]) => `    ${k}: ${flow(v)}`), '    of:', ...Object.entries(of).map(([f, fd]) => `      ${f}: ${flow(fd)}`));
    }
  }
  const entry = (e, ind) => {
    if (!('for' in e)) return Object.entries(e).map(([kind, props]) => `${ind}- ${kind}: ${flow(props)}`);
    const body = ['draw:', ...e.draw.flatMap((x) => entry(x, '  '))], head = `for: ${e.for}`, lines = Object.keys(e)[0] === 'for' ? [head, ...body] : [...body, head];
    return lines.map((line, i) => `${ind}${i ? '  ' : '- '}${line}`);
  };
  out.push('draw:', ...s.draw.flatMap((e) => entry(e, '  ')));
  return out.map((line) => pad + line);
}

/** Writes the package at `file` as a skill folder at `to`; returns what it did, line by line. */
export function convert(file, to) {
  const pkg = JSON.parse(readFileSync(file, 'utf8')), problems = checkPackage(pkg);
  if (pkg.kind !== 'scenes') throw new Error(`${file} is a ${pkg.kind} package: only scenes packages are written as display.yaml (a creature stays a package of its own)`);
  if (problems.length) throw new Error(`${file} is not a valid molty-pkg/1 package: ${problems[0]}`);
  if (existsSync(join(to, 'display.yaml'))) throw new Error(`${to} already has a display.yaml: convert into an empty folder`);
  const put = (rel, body) => { mkdirSync(dirname(join(to, rel)), { recursive: true }); writeFileSync(join(to, rel), body); };
  // assets: a mask is a symbol; an image is a map when a globe wears it or it is twice as wide as tall, else a picture
  const worn = new Set(Object.values(pkg.layouts).flatMap((l) => l.items).filter((it) => it.k === 'ball').map((it) => it.img));
  const as = (n, a) => (a.kind === 'mask' ? 'symbol' : worn.has(n) || a.w === 2 * a.h ? 'map' : 'picture'), groups = [];
  for (const [n, a] of Object.entries(pkg.assets ?? {})) {
    put(`assets/${n}.png`, assetPng(a));
    if (groups.at(-1)?.as === as(n, a)) groups.at(-1).names.push(n); else groups.push({ as: as(n, a), names: [n] });
  }
  const screens = Object.fromEntries(Object.entries(pkg.layouts).map(([n, l]) => [n, screenOf(l)])), split = Object.keys(screens).length >= SPLIT_AT;
  // editors read the schema from this line; a folder outside the repo has no steady path to it
  const schema = (from, name) => (resolve(to).startsWith(REPO) ? [`# yaml-language-server: $schema=${relative(join(to, from), join(REPO, 'schema', name))}`] : []);
  const out = [...schema('.', 'display-2.json'), 'molty: display/2', 'package:', `  id: ${scalar(pkg.id)}`, `  name: ${scalar(pkg.name)}`, `  version: ${pkg.version}`, '  about: TODO'];
  if (pkg.assets) out.push(groups.length ? 'assets:' : 'assets: []', ...groups.map((g) => `  - { ${g.names.length > 1 ? `files: ${scalar(`assets/{${g.names.join(',')}}.png`)}` : `file: assets/${g.names[0]}.png`}, as: ${g.as} }`));
  out.push('screens:');
  for (const [n, s] of Object.entries(screens)) {
    if (!split) { out.push(`  ${n}:`, ...screenYaml(s, '    ')); continue; }
    out.push(`  ${n}: screens/${n}.yaml`);
    put(`screens/${n}.yaml`, [...schema('screens', 'display-2-screen.json'), ...screenYaml(s)].join('\n') + '\n');
  }
  put('display.yaml', out.join('\n') + '\n');
  return [`${to}: display.yaml${split ? `, screens/ (${Object.keys(screens).length})` : ` (${Object.keys(screens).length} screens)`}, assets/ (${Object.keys(pkg.assets ?? {}).length} PNG)`,
    'next: write SKILL.md, fill in every "about: TODO" (molty-display check lists them), then molty-display build'];
}
