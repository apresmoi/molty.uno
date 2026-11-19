// SPDX-License-Identifier: MIT
// Display v2 screens into molty-pkg/1 layouts (docs/DISPLAY.md). Names, units, tokens, parts and data
// tables are resolved here, so the device gets the same compact items as ever. Also the device rules a
// schema cannot know: every binding reads a declared input of a fitting type, the limits once lists
// are at their fullest, and what still needs firmware (refused, with the way round it).
// A v1 item's fields come out in the order its properties are written, so a converted package builds
// back to the same bytes.
import { LIMITS } from '../format.mjs';
import { where } from './load.mjs';

/** Molty's look: the tokens every skill has. */
export const TOKENS = { colors: { ink: '#c8cce0', grid: '#565b72', dim: '#3a3e4e', white: '#ffffff', erase: '#000000' }, lines: { hairline: 0.012, thin: 0.03, bold: 0.06 }, sizes: {} };
/** Input types: what the device takes each as (v1), a number's range, and the most letters or elements (`max` may lower it). */
export const TYPES = {
  number: { v1: 'number' }, angle: { v1: 'number', range: [-720, 720] }, distance: { v1: 'number', range: [0, 2] }, amount: { v1: 'number', range: [0, 1] }, seconds: { v1: 'number', range: [0, 600] },
  text: { v1: 'text', most: LIMITS.text }, reading: { v1: 'text', most: LIMITS.say }, color: { v1: 'text' }, asset: { v1: 'text' }, groups: { v1: 'text' }, list: { v1: 'list', most: LIMITS.list },
};
/** v2 shape -> [v1 kind, { property: [v1 field, what it holds] }]. */
export const SHAPES = {
  circle: ['ring', { radius: ['r', 'distance'], width: ['w', 'width'] }],
  arc: ['arc', { radius: ['r', 'distance'], start: ['a0', 'angle'], end: ['a1', 'angle'], width: ['w', 'width'] }],
  ticks: ['ticks', { count: ['n', 'count'], radius: ['r', 'distance'], length: ['len', 'length'], first_angle: ['a0', 'angle'], width: ['w', 'width'] }],
  spoke: ['ticks', { angle: ['a0', 'angle'], radius: ['r', 'distance'], length: ['len', 'length'], width: ['w', 'width'] }],
  line: ['line', { width: ['w', 'width'] }],
  dot: ['disc', { radius: ['r', 'distance'] }],
  globe: ['ball', { radius: ['r', 'distance'], map: ['img', 'map'], lit: ['lit', 'amount'], lit_side: ['side', 'side'], spin: ['spin', 'speed'] }],
  symbol: ['glyph', { asset: ['asset', 'asset'], size: ['size', 'distance'], rotate: ['rot', 'angle'] }],
  label: ['text', { text: ['str', 'text'], size: ['size', 'distance'] }],
  // a new way to shape a zone or a new thing a tap does is one more row here, or one more case in shape() when it is
  // not a plain field (sector, opens), and its property in schema/display-2.json's tap_zone
  tap_zone: ['spot', { radius: ['r', 'distance'], reads: ['say', 'reading'], id: ['id', 'id'], highlights: ['lights', 'groups'], grow: ['grow', 'number'] }],
};
export const COMMON = { color: ['color', 'color'], group: ['group', 'group'] };
/** Properties that are not one plain field: shape() turns each into its v1 form. */
export const SHAPED = ['at', 'show', 'only_when', 'move', 'from', 'to', 'points', 'smooth', 'along', 'sector', 'opens'];
/** What a property holds -> the input types a binding there may read. */
export const READS = {
  distance: ['distance', 'number'], length: ['distance', 'number'], width: ['distance', 'number'], coord: ['number', 'distance'], angle: ['angle', 'number'], time: ['seconds', 'number'], amount: ['amount', 'number'],
  speed: ['number'], count: ['number'], number: ['number'], side: ['number'], color: ['color'], asset: ['asset'], map: ['asset'], text: ['text'], reading: ['reading', 'text'], id: ['text', 'asset'], group: ['text', 'asset'], groups: ['groups', 'text'], screen: ['text'],
};
const WORDS = ['text', 'reading', 'id', 'group', 'groups', 'screen'];
const REFUSED = {
  move: 'move needs firmware that slides a shape along keyframes, which is not out yet: repeat the shape with for and show: { from, until } for now',
  default: 'default needs firmware that fills in a missing input, which is not out yet: the agent sends every input, so put the usual value in example',
  path: 'type path goes with move, which needs firmware that is not out yet: use a list with x, y, from and until for now',
  polar: 'a point read from inputs must be { x, y }: an { angle, distance } point from inputs needs firmware that is not out yet, so work out x and y in the skill',
};
const BIND = /^\{\{ ?([a-z][a-z0-9_]*)(?:\.([a-z][a-z0-9_]*))? ?\}\}$/, FOR = /^([a-z][a-z0-9_]*) in ([a-z][a-z0-9_]*)$/;
const RAD = Math.PI / 180, n4 = (v) => +v.toFixed(4) + 0, A = (t) => (/^[aeio]/.test(t) ? `an ${t}` : `a ${t}`), list = (names) => names.join(', ') || 'none';

/** The name the agent sends for an input or a list field. */
export const sentAs = (name, decl) => decl.sent_as ?? name;
/** Example or preview values, written with the names display.yaml uses, as the params the agent sends. */
export function wireParams(inputs, values) {
  const row = (fields, e) => Object.fromEntries(Object.entries(e).map(([k, v]) => [fields?.[k] ? sentAs(k, fields[k]) : k, v]));
  return Object.fromEntries(Object.entries(values).map(([k, v]) => { const d = inputs?.[k]; return [d ? sentAs(k, d) : k, d?.type === 'list' && Array.isArray(v) && typeof d.of === 'object' ? v.map((e) => row(d.of, e)) : v]; }));
}

/**
 * skill (loadSkill) + its built assets (buildAssets' files) -> { layouts, screens: { name: { sources, items, fullest, lists } } }.
 * sources[i] is where item i was written. Problems go to bad(at, message), remarks to warn(at, message).
 */
export function compile(skill, files, bad, warn) {
  const doc = skill.doc, data = doc.data ?? {}, parts = doc.parts ?? {}, tokens = {}, layouts = {}, screens = {}, source = new WeakMap();
  for (const cat of Object.keys(TOKENS)) {
    tokens[cat] = { ...TOKENS[cat], ...doc.tokens?.[cat] };
    for (const t of Object.keys(doc.tokens?.[cat] ?? {})) if (Object.values(TOKENS).some((b) => t in b)) bad(where(doc.tokens[cat], t), `tokens.${cat}.${t} reuses a built-in token's name: pick another`);
  }

  for (const [screen, { def }] of Object.entries(skill.screens)) {
    const inputs = def.inputs ?? {}, used = new Set(), read = new Set(), fail = (at, m) => void bad(at, m);
    const token = (cat, v, at, what) => (v in tokens[cat] ? tokens[cat][v] : fail(at, `${what}: no ${cat.slice(0, -1)} token named ${v} (tokens: ${list(Object.keys(tokens[cat]))})`));
    function literal(v, kind, at, what) {
      const num = () => (typeof v === 'number' ? v : fail(at, `${what} must be a number`));
      switch (kind) {
        case 'distance': case 'length': return typeof v === 'string' ? token('sizes', v, at, what) : num();
        case 'width': return typeof v === 'string' ? token('lines', v, at, what) : num();
        case 'color': return typeof v !== 'string' ? fail(at, `${what} must be a color token or #rrggbb`) : /^#[0-9a-f]{6}$/.test(v) ? v : token('colors', v, at, what);
        case 'asset': return files[v] ? v : fail(at, `${what}: no asset named ${JSON.stringify(v)} (assets: ${list(Object.keys(files))})`);
        case 'map': return !files[v] ? fail(at, `${what}: no asset named ${JSON.stringify(v)} (maps: ${list(Object.keys(files).filter((n) => files[n].as === 'map'))})`) : files[v].as === 'symbol' ? fail(at, `${what}: ${v} is a symbol, and a globe wears a map (as: map)`) : v;
        case 'side': return v === 'right' ? 1 : v === 'left' ? -1 : fail(at, `${what} must be right or left`);
        case 'screen': return skill.screens[v] ? v : fail(at, `${what}: no screen named ${JSON.stringify(v)} (screens: ${list(Object.keys(skill.screens))})`);
        case 'text': case 'reading': case 'id': case 'group': case 'groups':
          return typeof v !== 'string' ? fail(at, `${what} must be text: put it in quotes`) : /^[$@]/.test(v) ? fail(at, `${what} ${JSON.stringify(v)} starts with ${v[0]}, which the device reads as a binding: start the text with something else`) : v;
        default: return num();
      }
    }
    /** What "{{ name }}" or "{{ name.field }}" stands for here: a literal (a data row's, a part's), or { ref } to an input. */
    function lookup([raw, name, field], ctx, at, what) {
      const no = (m) => fail(at, `${what} ${JSON.stringify(raw)}: ${m}`);
      if (field !== undefined) {
        if (name in ctx.rows) return field in ctx.rows[name] ? ctx.rows[name][field] : no(`${name} has no field ${field} (fields: ${list(Object.keys(ctx.rows[name]))})`);
        if (ctx.loop?.v !== name) return no(`{{ ${name}.${field} }} only works inside "for: ${name} in ..."`);
        const f = ctx.loop.decl.of?.[field];
        if (!f) return no(`${ctx.loop.src} has no field ${field} (fields: ${list(Object.keys(ctx.loop.decl.of ?? {}))})`);
        read.add(`${ctx.loop.src}.${field}`);
        return { ref: `@${sentAs(field, f)}`, type: f.type, name: `${name}.${field}` };
      }
      if (ctx.vars) return name in ctx.vars ? ctx.vars[name] : no(`the part ${ctx.part} takes no ${name} (with: ${list(Object.keys(ctx.vars))}): a part reads only what use passes it`);
      const d = inputs[name];
      if (!d) return no(`no input named ${name} (inputs: ${list(Object.keys(inputs))})`);
      used.add(name);
      return { ref: `$${sentAs(name, d)}`, type: d.type, name, decl: d };
    }
    /** A property's value as the v1 field holds it: a literal in device units, "$param" or "@field". */
    function value(v, kind, ctx, at, what) {
      if (typeof v === 'string' && v.includes('{{')) {
        const m = BIND.exec(v);
        if (!m) return fail(at, WORDS.includes(kind) ? `${what} ${JSON.stringify(v)} mixes words with an input: the device builds no text, so send the finished words as one input` : `${what} ${JSON.stringify(v)}: a binding fills the whole value ("{{ name }}"), with no formula round it: work it out in the skill`);
        v = lookup(m, ctx, at, what);
      }
      if (v === undefined) return undefined;
      if (!v.ref) return literal(v, kind, at, what);
      if (!READS[kind].includes(v.type)) return fail(at, `${what}: ${v.name} is ${A(v.type)}, and ${what} takes ${READS[kind].map(A).join(' or ')}${v.type === 'list' ? ' (repeat over a list with for)' : ''}`);
      return v.ref;
    }
    /** A point of a line or of a zone's segment, as [x, y]. */
    function point(p, ctx, at, what) {
      if (p === 'center') return [0, 0];
      if (!('angle' in p)) return [value(p.x ?? 0, 'coord', ctx, at, `${what}.x`), value(p.y ?? 0, 'coord', ctx, at, `${what}.y`)];
      const a = value(p.angle, 'angle', ctx, at, `${what}.angle`), d = value(p.distance, 'distance', ctx, at, `${what}.distance`);
      if (typeof a === 'number' && typeof d === 'number') return [n4(d * Math.cos(a * RAD)), n4(d * Math.sin(a * RAD))];
      if (a !== undefined && d !== undefined) fail(at, `${what}: ${REFUSED.polar}`);
      return [0, 0];
    }
    /**
     * The screen a tap opens finds each of its inputs by the name it is sent under: in the tapped element, else among
     * this screen's inputs (the device's rule). Here: every one is there, of the same kind, and counts as used.
     */
    function passes(target, ctx, at, what) {
      const sent = (decls) => new Map(Object.entries(decls ?? {}).map(([n, d]) => [sentAs(n, d), [n, d]])), mine = sent(inputs), row = sent(ctx.loop?.decl.of);
      for (const [name, [, want]] of sent(skill.screens[target].def.inputs)) {
        const [n, d] = (want.type !== 'list' && row.get(name)) || mine.get(name) || [];
        if (!d) { fail(at, `${what}: ${target} needs ${name}, and ${ctx.loop ? `neither an element of ${ctx.loop.src} nor this screen` : 'this screen'} sends one. Add ${want.type === 'list' || !ctx.loop ? `the input ${name} here` : `the field ${name} to ${ctx.loop.src}`} (an opened screen takes its inputs by name)`); continue; }
        if (TYPES[d.type]?.v1 !== TYPES[want.type]?.v1) { fail(at, `${what}: ${target} takes ${name} as ${A(want.type)}, and here it is ${A(d.type)}`); continue; }
        if (row.get(name)?.[1] === d) { read.add(`${ctx.loop.src}.${n}`); continue; }
        used.add(n);
        if (d.type !== 'list') continue;
        // a list goes along whole (the elements `of` the tapped zone): its elements need the fields the opened screen reads
        const have = sent(d.of);
        for (const [f] of sent(want.of)) if (have.has(f)) read.add(`${n}.${have.get(f)[0]}`); else fail(at, `${what}: ${target} reads ${f} from each element of ${name}, and ${n} here has no such field (fields: ${list([...have.keys()])})`);
        if (have.has('of')) read.add(`${n}.${have.get('of')[0]}`);
      }
    }
    function shape(kind, props, ctx, at) {
      const [k, fields] = SHAPES[kind], it = kind === 'spoke' && !('radius' in props) ? { k, r: 0 } : { k };
      if (kind === 'tap_zone' && 'at' in props && 'along' in props) fail(at, 'tap_zone has both at and along: a zone is round (at) or a band (along)');
      for (const [p, v] of Object.entries(props)) {
        const here = where(props, p) || at, what = `${kind}.${p}`;
        if (p === 'at') {
          if (v === 'center') continue;
          if ('angle' in v) it.polar = [value(v.angle, 'angle', ctx, here, `${what}.angle`), value(v.distance, 'distance', ctx, here, `${what}.distance`)];
          else for (const [c, n] of Object.entries(v)) it[c] = value(n, 'coord', ctx, here, `${what}.${c}`);
        } else if (p === 'show') for (const [q, t] of Object.entries(v)) it[q === 'from' ? 'on' : 'off'] = value(t, 'time', ctx, here, `${what}.${q}`);
        else if (p === 'only_when') it.when = 'lit';
        else if (p === 'move') fail(here, REFUSED.move);
        else if (p === 'smooth') { if (v) it.k = 'curve'; }
        else if (p === 'from' || p === 'to') it.pts ??= [point(props.from, ctx, here, `${kind}.from`), point(props.to, ctx, here, `${kind}.to`)];
        else if (p === 'points') it.pts = v.map((q, i) => point(q, ctx, here, `${what}[${i}]`));
        else if (p === 'along') it.pts = [point(v.from, ctx, here, `${what}.from`), point(v.to, ctx, here, `${what}.to`)];
        else if (p === 'sector' && 'width' in v) for (const [q, n] of Object.entries(v)) it[{ width: 'arc', inner: 'r0', outer: 'r1' }[q]] = value(n, q === 'width' ? 'angle' : 'distance', ctx, here, `${what}.${q}`);
        else if (p === 'sector') {   // from one angle to another: the zone is the wedge, its own circle in the wedge's middle
          const inner = v.inner ?? 0;
          if ('at' in props || 'along' in props) fail(here, `${what}: a sector with from and to places the zone itself, so drop ${'at' in props ? 'at' : 'along'} (or say { width, inner, outer }: a wedge around where the zone is)`);
          else if (v.to <= v.from || v.to - v.from > 360 || v.outer <= inner) fail(here, `${what}: it runs from a smaller angle to a larger one (at most a full turn), and from inner out to outer`);
          else Object.assign(it, { polar: [n4((v.from + v.to) / 2), n4((inner + v.outer) / 2)], ...('radius' in props ? {} : { r: n4((v.outer - inner) / 2) }), arc: n4(v.to - v.from), ...('inner' in v ? { r0: inner } : {}), r1: v.outer });
        } else if (p === 'opens') {
          const o = typeof v === 'string' ? { screen: v } : v;
          it.open = value(o.screen, 'screen', ctx, here, what);
          if ('seconds' in o) it.ttl = value(o.seconds, 'time', ctx, here, `${what}.seconds`);
          if (skill.screens[it.open]) passes(it.open, ctx, here, what);
        }
        else {
          const [f, holds] = fields[p] ?? COMMON[p];
          if (kind === 'spoke' && p === 'angle') it.n = 1;
          it[f] = value(v, holds, ctx, here, what);
        }
      }
      if (kind === 'tap_zone' && !('r' in it)) fail(at, 'tap_zone needs radius (how far from at a tap still counts), or a sector with from and to');
      source.set(it, at);
      return it;
    }
    /** The v1 items of a draw list. ctx: rows (data elements in reach), loop (the list input being repeated), vars (a part's values). */
    function draw(entries, ctx) {
      const out = [];
      for (const e of entries) {
        if ('use' in e) {
          const part = parts[e.use], at = where(e, 'use');
          if (!part) { fail(at, `no part named ${e.use} (parts: ${list(Object.keys(parts))})`); continue; }
          if (ctx.chain.includes(e.use)) { fail(at, `the part ${e.use} uses itself`); continue; }
          const takes = part.with ?? {}, vars = { ...takes };
          for (const [n, v] of Object.entries(e.with ?? {})) {
            const here = where(e.with, n), m = typeof v === 'string' && v.includes('{{') && BIND.exec(v);
            if (!(n in takes)) fail(here, `the part ${e.use} takes no ${n} (with: ${list(Object.keys(takes))})`);
            else if (m === null) fail(here, `with.${n} ${JSON.stringify(v)}: a binding fills the whole value ("{{ name }}")`);
            else vars[n] = m ? lookup(m, ctx, here, `with.${n}`) : v;
          }
          out.push(...draw(part.draw, { vars, rows: {}, loop: ctx.loop && { ...ctx.loop, v: null }, part: e.use, chain: [...ctx.chain, e.use] }));
        } else if ('for' in e) {
          const [, v, src] = FOR.exec(e.for), at = where(e, 'for'), passed = ctx.vars?.[src], decl = passed?.ref ? passed.decl : ctx.vars ? undefined : inputs[src];
          if (decl) {
            if (decl.type !== 'list') { fail(at, `${src} is ${A(decl.type)}, not a list: for repeats over a list input or a data table`); continue; }
            if (ctx.loop) { fail(at, `a for over the input ${src} sits inside another: the device repeats one list at a time`); continue; }
            if (!passed) used.add(src);
            // each goes last, or right after k when draw is written before for: the order a converted v1 item had
            const each = passed?.ref ?? `$${sentAs(src, decl)}`, early = Object.keys(e)[0] === 'draw';
            for (const it of draw(e.draw, { ...ctx, loop: { v, src: passed?.name ?? src, decl } })) { const rep = early ? { k: it.k, each, ...it } : { ...it, each }; source.set(rep, source.get(it)); out.push(rep); }
          } else if (data[src] && !(ctx.vars && src in ctx.vars)) {
            // a data table is written out here, layer by layer: the first shape for every element, then the second
            for (const inner of e.draw) for (const row of data[src]) out.push(...draw([inner], { ...ctx, rows: { ...ctx.rows, [v]: row } }));
          } else fail(at, `${src} is neither a list input nor a data table (lists: ${list(Object.keys(inputs).filter((n) => inputs[n].type === 'list'))}; tables: ${list(Object.keys(data))})`);
        } else {
          const [kind] = Object.keys(e);
          out.push(shape(kind, e[kind], ctx, where(e, kind)));
        }
      }
      return out;
    }

    // ---- the inputs, then the screen
    const at = where(def) || `${skill.file}:1`, wires = new Set(), lists = {};
    for (const [name, d] of Object.entries(inputs)) {
      const here = where(inputs, name), cap = TYPES[d.type]?.most;
      if (d.type === 'path') fail(here, `input ${name}: ${REFUSED.path}`);
      if ('default' in d) fail(where(d, 'default') || here, `input ${name}: ${REFUSED.default}`);
      if (name in data) fail(here, `input ${name} has the name of a data table: rename one, so "for: x in ${name}" says which it means`);
      if (wires.has(sentAs(name, d))) fail(here, `input ${name} is sent as ${sentAs(name, d)}, like another input`);
      wires.add(sentAs(name, d));
      if (d.type === 'list' ? typeof d.of !== 'object' : typeof d.of === 'object') fail(here, d.type === 'list' ? `input ${name} is a list: say what each element holds with of: { field: { type, about } }` : `input ${name}: of with fields goes with type list`);
      if (d.max !== undefined && cap !== undefined && !(Number.isInteger(d.max) && d.max >= 1 && d.max <= cap)) fail(where(d, 'max') || here, `input ${name}: max must be a whole number from 1 to ${cap} for ${A(d.type)}`);
      const sent = Object.entries(typeof d.of === 'object' ? d.of : {}).map(([f, fd]) => sentAs(f, fd));
      if (new Set(sent).size !== sent.length) fail(here, `input ${name}: two fields are sent under one name`);
      if (d.type === 'list') lists[`$${sentAs(name, d)}`] = { name, max: d.max ?? LIMITS.list };
    }
    const items = draw(def.draw ?? [], { rows: {}, loop: null, vars: null, part: null, chain: [] });
    for (const [name, d] of Object.entries(inputs)) {
      if (!used.has(name)) fail(where(inputs, name), `input ${name} is never used: bind it ("{{ ${name} }}"${d.type === 'list' ? `, or "for: x in ${name}"` : ''}) or remove it (the device refuses an input nothing reads)`);
      else for (const f of Object.keys(typeof d.of === 'object' ? d.of : {})) if (!read.has(`${name}.${f}`)) warn(where(d.of, f), `field ${f} of ${name} is never read: the agent would send it for nothing`);
    }
    // the screen at its fullest: every list at its max
    const times = (it) => (it.each ? lists[it.each]?.max ?? LIMITS.list : 1), fullest = items.reduce((n, it) => n + times(it), 0);
    const filled = Object.values(lists).filter((l) => items.some((it) => lists[it.each] === l)).map((l) => `${l.max} ${l.name}`).join(', ');
    const span = (v, it) => (typeof v !== 'string' ? 0 : v[0] === '$' ? Object.entries(inputs).find(([n, d]) => `$${sentAs(n, d)}` === v)?.[1].max : v[0] === '@' ? Object.entries(inputs[lists[it.each]?.name]?.of ?? {}).find(([n, d]) => `@${sentAs(n, d)}` === v)?.[1].max : v.length) ?? LIMITS.text;
    const letters = items.filter((it) => it.k === 'text').reduce((n, it) => n + span(it.str, it) * times(it), 0);
    const groups = new Map(items.filter((it) => typeof it.group === 'string').map((it) => [/^[$@]/.test(it.group) ? `${it.each} ${it.group}` : it.group, it.group[0] === '@' ? times(it) : 1]));
    const named = [...groups.values()].reduce((a, b) => a + b, 0);
    if (items.length > LIMITS.items) fail(at, `screen ${screen} writes out ${items.length} items: the device takes ${LIMITS.items} a screen (a part or a data table counts every shape it writes)`);
    if (fullest > LIMITS.scene) fail(at, `screen ${screen} can reach ${fullest} items (${items.length} written${filled ? `, with ${filled}` : ''}): the device shows at most ${LIMITS.scene} at once. Give its lists a smaller max, or draw less for each element`);
    if (letters > LIMITS.letters) fail(at, `screen ${screen} can reach ${letters} letters in its labels: the device shows at most ${LIMITS.letters} at once. Give its text inputs a smaller max, or write less`);
    if (named > LIMITS.groups) fail(at, `screen ${screen} can name ${named} groups: the device lights at most ${LIMITS.groups}. Give its lists a smaller max`);
    layouts[screen] = {
      dur: def.duration === 'hold' || def.duration === undefined ? 0 : def.duration,
      ...(def.subtitles === undefined || def.subtitles === 'bottom' ? {} : { caption_y: def.subtitles === 'middle' ? 0 : def.subtitles }),
      ...('inputs' in def ? { params: Object.fromEntries(Object.entries(inputs).map(([n, d]) => [sentAs(n, d), TYPES[d.type]?.v1 ?? 'list'])) } : {}),
      items,
    };
    screens[screen] = { sources: items.map((it) => source.get(it)), items: items.length, fullest, filled };
  }
  return { layouts, screens };
}
