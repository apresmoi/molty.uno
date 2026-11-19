// SPDX-License-Identifier: MIT
// The JSON Schema keywords schema/display-2.json uses, checked with messages a person can act on.
// Small on purpose: KEYWORDS is the whole list (display.test.mjs holds the schema to it), and
// `errorMessage` (the convention editors' JSON language servers read too) says a rule in plain words.

/** Every keyword validate() applies, and the annotations it skips. */
export const KEYWORDS = ['$ref', 'type', 'const', 'enum', 'pattern', 'minLength', 'maxLength', 'minimum', 'maximum', 'exclusiveMinimum', 'required', 'properties', 'additionalProperties', 'propertyNames', 'minProperties', 'maxProperties', 'items', 'minItems', 'maxItems', 'anyOf', 'oneOf', 'errorMessage'];
export const ANNOTATIONS = ['$schema', '$id', '$defs', 'title', 'description', 'examples'];

const typeOf = (v) => (v === null ? 'null' : Array.isArray(v) ? 'array' : Number.isInteger(v) ? 'integer' : typeof v);
const isType = (v, t) => typeOf(v) === t || (t === 'number' && typeOf(v) === 'integer');
const A = (t) => (/^[aeio]/.test(t) ? `an ${t}` : `a ${t}`);
/** The closest of `known` to a misspelt `key` (edit distance at most 2), or undefined. */
function nearest(key, known) {
  const dist = (a, b) => { const d = Array.from({ length: a.length + 1 }, (_, i) => [i]); for (let j = 1; j <= b.length; j++) { d[0][j] = j; for (let i = 1; i <= a.length; i++) d[i][j] = Math.min(d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + (a[i - 1] !== b[j - 1])); } return d[a.length][b.length]; };
  return known.map((k) => [dist(key, k), k]).filter(([d]) => d <= 2).sort((a, b) => a[0] - b[0])[0]?.[1];
}

/**
 * Every way `value` breaks `schema`: [{ path: [keys], message }], empty when it holds.
 * A node's errorMessage replaces what its own value-level keywords say (not a missing or unknown key).
 */
export function validate(schema, value, root = schema, path = []) {
  const deref = (s) => { while (s.$ref) s = s.$ref.slice(2).split('/').reduce((n, k) => n[k], root); return s; };
  const s = deref(schema), out = [], own = (kw, message) => out.push({ path, kw, message });
  const sub = (child, v = value, p = path) => validate(child, v, root, p);
  const t = typeOf(value), obj = t === 'object';
  if (s.type && ![s.type].flat().some((x) => isType(value, x))) own('type', `must be ${[s.type].flat().map(A).join(' or ')}`);
  if ('const' in s && value !== s.const) own('const', `must be ${JSON.stringify(s.const)}`);
  if (s.enum && !s.enum.includes(value)) own('enum', `must be one of: ${s.enum.join(', ')}`);
  if (t === 'string') {
    if (s.pattern && !new RegExp(s.pattern).test(value)) own('pattern', `must match ${s.pattern}`);
    if (s.minLength !== undefined && value.length < s.minLength) own('minLength', s.minLength === 1 ? 'must not be empty' : `must be at least ${s.minLength} characters`);
    if (s.maxLength !== undefined && value.length > s.maxLength) own('maxLength', `must be at most ${s.maxLength} characters (it is ${value.length})`);
  }
  if (t === 'number' || t === 'integer') {
    if (s.minimum !== undefined && value < s.minimum) own('minimum', `must be at least ${s.minimum}`);
    if (s.maximum !== undefined && value > s.maximum) own('maximum', `must be at most ${s.maximum}`);
    if (s.exclusiveMinimum !== undefined && value <= s.exclusiveMinimum) own('exclusiveMinimum', `must be more than ${s.exclusiveMinimum}`);
  }
  if (obj) {
    const keys = Object.keys(value), known = Object.keys(s.properties ?? {});
    for (const k of s.required ?? []) if (!(k in value)) own('required', `needs ${k}`);
    for (const k of keys) {
      if (s.properties?.[k]) out.push(...sub(s.properties[k], value[k], [...path, k]));
      else if (s.additionalProperties === false) { const near = nearest(k, known); own('additionalProperties', `has no "${k}"${near ? ` (did you mean ${near}?)` : `: it takes ${known.filter((x) => x !== '$schema').join(', ')}`}`); }
      else if (s.additionalProperties) out.push(...sub(s.additionalProperties, value[k], [...path, k]));
      if (s.propertyNames) for (const e of sub(s.propertyNames, k, [...path, k])) out.push({ ...e, message: `the name ${e.message}` });
    }
    if (s.minProperties !== undefined && keys.length < s.minProperties) own('minProperties', s.minProperties === 1 ? 'must not be empty' : `needs at least ${s.minProperties} entries`);
    if (s.maxProperties !== undefined && keys.length > s.maxProperties) own('maxProperties', `holds at most ${s.maxProperties} (it has ${keys.length})`);
  }
  if (t === 'array') {
    if (s.items) value.forEach((v, i) => out.push(...sub(s.items, v, [...path, i])));
    if (s.minItems !== undefined && value.length < s.minItems) own('minItems', s.minItems === 1 ? 'must not be empty' : `needs at least ${s.minItems} entries`);
    if (s.maxItems !== undefined && value.length > s.maxItems) own('maxItems', `holds at most ${s.maxItems} (it has ${value.length})`);
  }
  for (const kw of ['anyOf', 'oneOf']) {
    if (!s[kw] || out.some((e) => e.path === path && (e.kw === 'required' || e.kw === 'additionalProperties'))) continue;
    const tries = s[kw].map((b) => sub(b)), passed = tries.filter((e) => !e.length).length;
    if (kw === 'anyOf' ? passed : passed === 1) continue;
    // the one form the value could have been (by its type) explains itself; otherwise the node's own words
    const fits = s[kw].map(deref).map((b, i) => [b, tries[i]]).filter(([b]) => ('const' in b || b.enum ? !obj && t !== 'array' : !b.type || [b.type].flat().some((x) => isType(value, x))));
    if (!passed && fits.length === 1) out.push(...fits[0][1]);
    else own(kw, passed ? 'matches more than one form' : 'matches none of the forms it can take');
  }
  if (!s.errorMessage) return out;
  const mine = (e) => e.path === path && e.kw !== 'required' && e.kw !== 'additionalProperties';
  return out.some(mine) ? [...out.filter((e) => !mine(e)), { path, kw: 'errorMessage', message: s.errorMessage }] : out;
}
