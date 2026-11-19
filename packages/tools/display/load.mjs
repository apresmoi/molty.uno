// SPDX-License-Identifier: MIT
// A display v2 skill folder (docs/DISPLAY.md) read into memory: display.yaml (or display.json), the
// screens it keeps in screens/<name>.yaml, and where each value was written, so a check can name the
// file and line to fix.
import { existsSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import yaml from '../vendor/js-yaml.mjs';

// every parsed mapping and list -> { file, line, at: { key or index: line } }
const ORIGIN = new WeakMap();

/** YAML (or JSON) text as data, remembering where each mapping, list and key sits. Throws { message, line }. */
export function parse(text, file) {
  const open = [];
  try {
    return yaml.load(text, { listener(ev, st) {
      if (ev === 'open') return void open.push({ line: st.line + 1, kids: [] });
      const n = open.pop();
      n.value = st.result;
      open.at(-1)?.kids.push(n);
      if (st.result === null || typeof st.result !== 'object') return;
      const at = {};
      if (st.kind === 'mapping') for (let i = 0; i + 1 < n.kids.length; i += 2) at[n.kids[i].value] = n.kids[i].line;
      else n.kids.forEach((k, i) => { at[i] = k.line; });
      ORIGIN.set(st.result, { file, line: n.line, at });
    } });
  } catch (e) {
    throw Object.assign(new Error(e.reason ?? e.message), { line: (e.mark?.line ?? 0) + 1 });
  }
}

/** "file:line" of `node` (a parsed mapping or list), or of its entry `key` when given. */
export function where(node, key) {
  const o = ORIGIN.get(node);
  return o ? `${o.file}:${(key !== undefined && o.at[key]) || o.line}` : '';
}
/** where() for a path of keys below `node`: as deep as the path stays inside parsed data. */
export function whereAt(node, path) {
  let at = where(node);
  for (const k of path) {
    at = where(node, k) || at;
    node = node?.[k];
    if (node === null || typeof node !== 'object') break;
  }
  return at;
}

/**
 * The folder's display file and screens: { dir, file, doc, screens: { name: { def, file } }, problems }.
 * A screen written as a path is read from that file. problems: [{ at, message }] for what could not be read.
 */
export function loadSkill(dir) {
  const problems = [], file = ['display.yaml', 'display.yml', 'display.json'].find((f) => existsSync(join(dir, f)));
  const read = (f) => {
    try { return parse(readFileSync(join(dir, f), 'utf8'), f); } catch (e) { problems.push({ at: `${f}:${e.line ?? 1}`, message: e.message }); return null; }
  };
  if (!file) return { dir, file: 'display.yaml', doc: null, screens: {}, problems: [{ at: 'display.yaml:1', message: `no display.yaml in ${dir}` }] };
  const doc = read(file), screens = {};
  for (const [name, s] of Object.entries(doc?.screens ?? {})) {
    if (typeof s !== 'string') { screens[name] = { def: s, file }; continue; }
    if (!existsSync(join(dir, s))) { problems.push({ at: where(doc.screens, name), message: `screen ${name}: ${s} does not exist` }); continue; }
    const def = read(s);
    if (def) screens[name] = { def, file: s };
  }
  return { dir, file, doc, screens, problems };
}
