// SPDX-License-Identifier: MIT
// molty-display check and build (docs/DISPLAY.md). check: the schema, then the device rules, then the
// links in SKILL.md; every problem names its file, its line and the fix. build: the molty-pkg/1
// package the device installs, what it costs, and the agent's docs (build/display.md, an input schema
// per screen).
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { basename, dirname, join, relative, resolve } from 'node:path';
import { LIMITS, MAX_PKG_BYTES, ParamError, bind, checkPackage } from '../format.mjs';
import { buildAssets } from './assets.mjs';
import { TYPES, compile, sentAs, wireParams } from './compile.mjs';
import { loadSkill, where, whereAt } from './load.mjs';
import { validate } from './schema.mjs';

export const SCHEMA = JSON.parse(readFileSync(new URL('../../../schema/display-2.json', import.meta.url), 'utf8'));
const kb = (n) => `${(n / 1024).toFixed(1)} KB`, size = (v) => Buffer.byteLength(JSON.stringify(v));
const fieldsOf = (d) => (typeof d.of === 'object' ? d.of : {});

/** What the agent may send for one input or list field, as JSON Schema. names: every asset, by folder. */
function sends(d, files) {
  const T = TYPES[d.type], about = { description: d.about }, names = Object.keys(files).filter((n) => !d.of || typeof d.of !== 'string' || files[n].file.startsWith(`${d.of.replace(/\/$/, '')}/`));
  if (T.v1 === 'number') return { type: 'number', ...((d.min ?? T.range?.[0]) !== undefined ? { minimum: d.min ?? T.range[0] } : {}), ...((d.max ?? T.range?.[1]) !== undefined ? { maximum: d.max ?? T.range[1] } : {}), ...about };
  if (d.type === 'list') return { type: 'array', maxItems: d.max ?? T.most, items: { type: 'object', required: Object.entries(fieldsOf(d)).map(([f, fd]) => sentAs(f, fd)), additionalProperties: false, properties: Object.fromEntries(Object.entries(fieldsOf(d)).map(([f, fd]) => [sentAs(f, fd), sends(fd, files)])) }, ...about };
  if (d.type === 'asset') return { enum: names, ...about };
  if (d.type === 'color') return { type: 'string', pattern: '^#[0-9a-f]{6}$', ...about };
  if (d.type === 'groups') return { type: 'string', pattern: `^[A-Za-z0-9_-]{1,24}( [A-Za-z0-9_-]{1,24}){0,${LIMITS.lights - 1}}$`, ...about };
  return { type: 'string', ...(d.type === 'reading' ? { minLength: 1 } : {}), maxLength: d.max ?? T.most, pattern: '^(?![$@])[\\x20-\\x7e]*$', ...about };
}
/** A screen's params, as the JSON Schema an agent checks them with before scene.play. */
export const inputSchema = (id, screen, def, files) => ({
  title: `${id} ${screen}: the params of scene.play`,
  type: 'object',
  required: Object.entries(def.inputs ?? {}).map(([n, d]) => sentAs(n, d)),
  additionalProperties: false,
  properties: Object.fromEntries(Object.entries(def.inputs ?? {}).map(([n, d]) => [sentAs(n, d), sends(d, files)])),
});
/** The screen's example params (as sent), or null when an input has no example. */
export function exampleParams(def) {
  const inputs = def.inputs ?? {};
  return Object.values(inputs).every((d) => 'example' in d) ? wireParams(inputs, Object.fromEntries(Object.entries(inputs).map(([n, d]) => [n, d.example]))) : null;
}
const ttl = (def) => (typeof def.duration === 'number' && def.duration > 0 ? Math.ceil(def.duration) + 5 : 60);

/**
 * Everything wrong with the skill folder: { skill, errors, warnings, pkg, bytes, files, screens }.
 * errors and warnings are [{ at: "file:line", message }]; pkg only when the schema held.
 */
export function check(dir) {
  const skill = loadSkill(dir), errors = [...skill.problems], warnings = [], doc = skill.doc;
  const bad = (at, message) => errors.push({ at, message }), warn = (at, message) => warnings.push({ at, message });
  if (!doc || typeof doc !== 'object') return { skill, errors, warnings };

  // 1. the schema
  const shape = (schema, node) => {
    for (const e of validate(schema, node, SCHEMA)) {
      const v = e.path.reduce((n, k) => n?.[k], node), label = e.path.reduce((s, k) => (typeof k === 'number' ? `${s}[${k}]` : s ? `${s}.${k}` : k), '');
      bad(whereAt(node, 'key' in e ? [...e.path, e.key] : e.path), v && typeof v === 'object' && '[object Object]' in v ? `${label} is a binding without quotes: write "{{ name }}"` : `${label || 'the file'} ${e.message}`);
    }
  };
  shape(SCHEMA, doc);
  for (const { def, file } of Object.values(skill.screens)) if (file !== skill.file) shape(SCHEMA.$defs.screen, def);
  if (errors.length) return { skill, errors, warnings };

  // 2. the device rules
  const { assets, files } = buildAssets(dir, doc.assets ?? [], (n, m) => bad(where(doc.assets, n), m));
  if (Object.keys(assets).length > LIMITS.assets) bad(where(doc.assets), `${Object.keys(assets).length} assets: the device holds ${LIMITS.assets}`);
  const { layouts, screens } = compile(skill, files, bad, warn);
  const pkg = { v: 'molty-pkg/1', id: doc.package.id, name: doc.package.name, version: doc.package.version, kind: 'scenes', ...(doc.assets ? { assets } : {}), layouts };
  const bytes = Buffer.from(JSON.stringify(pkg) + '\n');
  if (bytes.length > MAX_PKG_BYTES) bad(`${skill.file}:1`, `the package is ${kb(bytes.length)}: the device takes ${kb(MAX_PKG_BYTES)}. Its largest assets: ${Object.entries(assets).sort((a, b) => size(b[1]) - size(a[1])).slice(0, 3).map(([n, a]) => `${n} ${kb(size(a))}`).join(', ')}`);
  // the v1 format's own check, last: whatever the rules above let through is named by the item it reached
  if (!errors.length) for (const p of checkPackage(pkg, doc.package.id)) {
    const m = /^layout (\S+?)(?: item (\d+))?: (.*)$/.exec(p);
    bad(m ? (m[2] ? screens[m[1]].sources[+m[2]] : where(skill.screens[m[1]].def)) || `${skill.screens[m[1]].file}:1` : `${skill.file}:1`, `${m ? m[3] : p} (the device's own check of the built package)`);
  }
  // the examples and the abouts, once the screens themselves hold
  for (const [name, { def, file }] of errors.length ? [] : Object.entries(skill.screens)) {
    const inputs = def.inputs ?? {}, schema = inputSchema(pkg.id, name, def, files);
    for (const [n, d] of Object.entries(inputs)) {
      for (const [what, f] of [[`input ${n}`, d], ...Object.entries(fieldsOf(d)).map(([fn, fd]) => [`field ${fn} of ${n}`, fd])]) if (f.type === 'asset' && !sends(f, files).enum.length) bad(where(inputs, n), `${what}: no asset is in ${f.of ?? 'assets/'}`);
      if ('example' in d) for (const e of validate(schema.properties[sentAs(n, d)], wireParams(inputs, { [n]: d.example })[sentAs(n, d)])) bad(where(d, 'example') || where(inputs, n), `input ${n}: its example${e.path.map((k) => (typeof k === 'number' ? `[${k}]` : `.${k}`)).join('')} ${e.message}`);
    }
    const params = exampleParams(def);
    if (!params) warn(where(def, 'inputs') || `${file}:1`, `screen ${name}: give every input an example, and build/display.md shows a call that works`);
    else try { bind(pkg, name, params); } catch (e) { if (!(e instanceof ParamError)) throw e; bad(where(def, 'inputs') || `${file}:1`, `screen ${name}: the device would refuse its examples: ${e.message}`); }
    for (const [node, key] of [[def, 'about'], ...Object.values(inputs).flatMap((d) => [[d, 'about'], ...Object.values(fieldsOf(d)).map((f) => [f, 'about'])])]) if (node[key] === 'TODO') warn(where(node, key), 'about is still TODO: say what it is in one sentence');
  }
  if (doc.package.about === 'TODO') warn(where(doc.package, 'about'), 'about is still TODO: say what the package adds in one line');

  // 3. the links in SKILL.md
  const md = join(dir, 'SKILL.md');
  if (!existsSync(md)) warn('SKILL.md:1', 'no SKILL.md yet: write one that says when to show each screen, linking it as [chart](display.yaml#chart)');
  else {
    const text = readFileSync(md, 'utf8'), linked = new Set();
    for (const m of text.matchAll(/\]\((?:\.\/)?display\.(?:ya?ml|json)#([^)\s]*)\)/g)) {
      linked.add(m[1]);
      if (!skill.screens[m[1]]) bad(`SKILL.md:${text.slice(0, m.index).split('\n').length}`, `links to screen ${m[1]}, which ${basename(skill.file)} does not have (screens: ${Object.keys(skill.screens).join(', ')})`);
    }
    const lost = Object.keys(skill.screens).filter((s) => !linked.has(s));
    if (lost.length) warn('SKILL.md:1', `never mentions ${lost.length === 1 ? 'the screen' : 'the screens'} ${lost.join(', ')}: link each as [name](${skill.file}#name) where it says when to show it`);
  }
  return { skill, errors, warnings, pkg, bytes, files, screens };
}

// ---- build/display.md
const cell = (s) => String(s).replace(/\s+/g, ' ').replace(/\|/g, '\\|');
function typeWords(d, files) {
  const T = TYPES[d.type], s = sends(d, files), range = s.minimum !== undefined && s.maximum !== undefined ? `, ${s.minimum} to ${s.maximum}` : '';
  switch (d.type) {
    case 'angle': return `angle in degrees${d.min !== undefined || d.max !== undefined ? range : ''}`;
    case 'distance': return `distance${range} (1 is the rim)`;
    case 'amount': case 'number': return `number${range}`;
    case 'seconds': return `seconds${range}`;
    case 'text': return `text, up to ${d.max ?? T.most} letters`;
    case 'reading': return `text a tap shows, up to ${d.max ?? T.most} characters`;
    case 'color': return 'color, #rrggbb';
    case 'asset': return `one of: ${s.enum.map((n) => `\`${n}\``).join(' ')}`;
    case 'groups': return `group names, spaces between, up to ${LIMITS.lights}`;
    default: return `list, up to ${d.max ?? T.most}`;
  }
}
function displayMd(doc, skill, files) {
  const p = doc.package, lasts = (def) => (typeof def.duration === 'number' && def.duration > 0 ? `${def.duration} s` : 'held'), out = [];
  out.push(`# ${p.name}: screens`, '', `Generated from ${skill.file} by \`molty-display build\`. Do not edit: change ${skill.file} and build again.`, '',
    `${p.about} Package \`${p.id}\`, version ${p.version}.${p.fallback ? ` On a Molty without screens: ${p.fallback === 'speech' ? 'say it in words instead' : 'do nothing'}.` : ''}`, '',
    'Show a screen with `scene.play { pkg, layout, params, ttl_s }` (`scene.queue` plays it after the one showing). `layout` is the screen\'s name.',
    'Send every input and nothing else. `inputs/<screen>.schema.json` (next to this file) checks params before you send them.', '',
    '| screen | shows | lasts |', '|---|---|---|', ...Object.entries(skill.screens).map(([n, { def }]) => `| [${n}](#${n}) | ${cell(def.about)} | ${lasts(def)} |`));
  for (const [name, { def }] of Object.entries(skill.screens)) {
    const inputs = Object.entries(def.inputs ?? {}), params = exampleParams(def), spot = def.subtitles === 'middle' ? 'in the middle' : typeof def.subtitles === 'number' ? `at y ${def.subtitles}` : '';
    out.push('', `## ${name}`, '', `${def.about} ${lasts(def) === 'held' ? 'Held until another screen replaces it or its `ttl_s` ends.' : `Lasts ${lasts(def)}.`}${spot ? ` Readings show ${spot}.` : ''}`, '');
    if (!inputs.length) out.push('No inputs: send `"params": {}`.');
    else out.push('| send | type | about |', '|---|---|---|', ...inputs.map(([n, d]) => `| \`${sentAs(n, d)}\` | ${typeWords(d, files)} | ${cell(d.about)} |`));
    for (const [n, d] of inputs.filter(([, d]) => d.type === 'list')) out.push('', `Each element of \`${sentAs(n, d)}\`:`, '', '| field | type | about |', '|---|---|---|', ...Object.entries(fieldsOf(d)).map(([f, fd]) => `| \`${sentAs(f, fd)}\` | ${typeWords(fd, files)} | ${cell(fd.about)} |`));
    if (!params) continue;
    const cut = Object.values(params).some((v) => Array.isArray(v) && v.length > 2), short = Object.fromEntries(Object.entries(params).map(([k, v]) => [k, Array.isArray(v) ? v.slice(0, 2) : v]));
    out.push('', `A call that works${cut ? ' (lists cut to two elements here)' : ''}:`, '', '```json', JSON.stringify({ name: 'scene.play', args: { pkg: p.id, layout: name, params: short, ttl_s: ttl(def) } }), '```');
  }
  return out.join('\n') + '\n';
}

/** Where a skill's package goes: packages/<id>/<id>.json for a skill under the repo's skills/, else its own build/. */
export function packagePath(dir, id) {
  const up = resolve(dir, '..');
  return basename(up) === 'skills' && existsSync(join(up, '..', 'packages')) ? join(up, '..', 'packages', id, `${id}.json`) : join(resolve(dir), 'build', `${id}.json`);
}

/**
 * check(), then write the package (to `out`, default packagePath), build/display.md and build/inputs/<screen>.schema.json.
 * Returns check()'s result plus { out, report } (the cost report's lines); nothing is written when there are errors.
 */
export function build(dir, out) {
  const r = check(dir);
  if (r.errors.length) return r;
  const { skill, pkg, bytes, files, screens } = r, doc = skill.doc, made = join(dir, 'build'), rel = (f) => (relative(process.cwd(), f).startsWith('..') ? f : relative(process.cwd(), f));
  out = resolve(out ?? packagePath(dir, pkg.id));
  mkdirSync(dirname(out), { recursive: true });
  writeFileSync(out, bytes);
  // the catalog's line (packages/index.json reads it) sits beside a package that lands in packages/
  if (basename(dirname(dirname(out))) === 'packages') writeFileSync(join(dirname(out), 'description.txt'), `${doc.package.about}\n`);
  rmSync(join(made, 'inputs'), { recursive: true, force: true });
  mkdirSync(join(made, 'inputs'), { recursive: true });
  writeFileSync(join(made, 'display.md'), displayMd(doc, skill, files));
  for (const [name, { def }] of Object.entries(skill.screens)) writeFileSync(join(made, 'inputs', `${name}.schema.json`), JSON.stringify(inputSchema(pkg.id, name, def, files), null, 2) + '\n');

  const kinds = { symbol: 'symbols', picture: 'pictures', map: 'maps' }, assets = pkg.assets ?? {};
  const cost = Object.entries(kinds).map(([as, word]) => [word, Object.keys(assets).filter((n) => files[n].as === as)]).filter(([, names]) => names.length).map(([word, names]) => `${word} ${names.length}, ${kb(names.reduce((n, a) => n + size(assets[a]), 0))}`);
  const [top, t] = Object.entries(screens).sort((a, b) => b[1].fullest - a[1].fullest)[0];
  const report = [
    `assets    ${cost.join('; ') || 'none'}`,
    `screens   ${Object.keys(screens).length}, ${kb(size(pkg.layouts))}; fullest: ${top}, ${t.items} items written, ${t.fullest} of ${LIMITS.scene} on screen${t.filled ? ` with ${t.filled}` : ''}`,
    `package   ${rel(out)}  ${kb(bytes.length)} of ${kb(MAX_PKG_BYTES)}  ok`,
    `docs      ${rel(join(made, 'display.md'))}, ${rel(join(made, 'inputs'))}/*.schema.json`,
  ];
  return { ...r, out, report };
}
