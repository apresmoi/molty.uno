#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// atlas.mjs: the night sky's best known objects on Molty, and where each is for your human tonight.
//   atlas.mjs place --name Berlin --lat 52.52 --lon 13.40 --tz Europe/Berlin | place --forget | place
//   atlas.mjs open [<kind>] | show <object> [--where] | tonight [--see eye|binoculars] | find <words>
//   atlas.mjs install | status          options: --at <ISO time>, --order fame|up, --dry-run
// Reads the pairing the molty skill stored (~/.config/molty/env + token; MOLTY_CONFIG_DIR, MOLTY_RELAY, MOLTY_DEVICE
// override); keeps the place next to it (atlas-place.json), a town rounded to 0.1 degree, never sent anywhere: Molty
// gets directions, heights and times worked out from it. Installs the atlas package first when Molty lacks it (Molty asks
// your human with one tap), sends every kind's objects (atlas.load), the screen (scene.play) and where to start
// (atlas.show). Prints `SAY: <line>`, `FACTS: <json>` and, with no place kept, `ASK: <question>`. Never prints the token.
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';
import * as sky from './sky.mjs';
import { CAT, byId, glassName, list, load } from './wire.mjs';

const PKG = 'atlas';
const RAW = 'https://raw.githubusercontent.com/apresmoi/molty.uno/main/';
const INDEX = process.env.MOLTY_PKG_INDEX ?? `${RAW}packages/index.json`;
const DEFAULT_RELAY = 'https://molty-relay.alicenet.workers.dev'; // the molty skill's default relay (MOLTY_RELAY overrides)
const VALUED = ['--name', '--lat', '--lon', '--tz', '--see', '--at', '--order', '--ttl'];
const argv = process.argv.slice(2), flag = (k) => argv.includes(k), opt = (k, d) => { const i = argv.indexOf(k); return i < 0 ? d : argv[i + 1]; };
const [what, ...words] = argv.filter((a, i) => !a.startsWith('--') && !VALUED.includes(argv[i - 1]));
const dry = flag('--dry-run'), now = opt('--at') ? new Date(opt('--at')) : new Date();
const DIR = process.env.MOLTY_CONFIG_DIR ?? join(homedir(), '.config/molty'), PLACE = join(DIR, 'atlas-place.json');

// ---- the place: with you only, a town, never finer than 0.1 degree (about 10 km) ----
const place = existsSync(PLACE) ? JSON.parse(readFileSync(PLACE, 'utf8')) : null;
const ASK = 'Where are you? A town is enough.';

// ---- the relay (as molty-astro's astro.mjs) ----
function config() {
  const env = {};
  if (existsSync(join(DIR, 'env'))) for (const l of readFileSync(join(DIR, 'env'), 'utf8').split('\n')) { const m = /^\s*(?:export\s+)?([A-Z_]+)=["']?([^"'\s]*)/.exec(l); if (m) env[m[1]] = m[2]; }
  const relay = (process.env.MOLTY_RELAY ?? env.MOLTY_RELAY ?? DEFAULT_RELAY).replace(/\/$/, ''), device = process.env.MOLTY_DEVICE ?? env.MOLTY_DEVICE;
  const token = existsSync(join(DIR, 'token')) ? readFileSync(join(DIR, 'token'), 'utf8').trim() : '';
  if (!device || !token) throw new Error('not paired: pair with the molty skill first (scripts/pair.sh <DEVICE> <CODE>)');
  return { relay, device, token };
}
const cfg = () => (cfg.v ??= config());
async function rest(path, init = {}) {
  const { relay, token } = cfg();
  return (await fetch(relay + path, { ...init, headers: { authorization: `Bearer ${token}`, 'content-type': 'application/json', 'user-agent': 'molty-atlas' } })).json();
}
/** One relay command; follows a pending job to its end. Returns the result or throws {code}. */
async function send(name, args, wait = false) {
  if (dry) { console.log(JSON.stringify({ name, args })); return name === 'pkg.list' ? { packages: [] } : {}; }
  let out = await rest(`/v1/devices/${cfg().device}/cmd`, { method: 'POST', body: JSON.stringify({ name, args, ...(wait ? { wait } : {}) }) });
  for (let i = 0; i < 40 && out.ok && ['pending', 'queued', 'running'].includes(out.result?.status); i++) {
    const job = await rest(`/v1/jobs/${out.result.job}?wait_s=8`);
    out = job.status === 'done' ? { ok: true, result: job.result ?? {} } : job.status === 'failed' ? { ok: false, error: job.error, message: job.detail } : { ok: true, result: job };
  }
  if (!out.ok) throw Object.assign(new Error(`${name}: ${out.error}${out.message ? ` (${out.message})` : ''}`), { code: out.error });
  return out.result;
}
async function entry() {
  const index = /^https?:/.test(INDEX) ? await (await fetch(INDEX)).json() : JSON.parse(readFileSync(INDEX, 'utf8'));
  const e = index[PKG];
  if (!e) throw new Error(`no ${PKG} in ${INDEX}`);
  const root = /^https?:/.test(INDEX) ? new URL('..', INDEX).href : RAW;
  return { id: PKG, url: root + e.path, sha256: e.sha256, size: e.size, name: e.name, version: e.version };
}
async function install({ version, ...args }) {
  console.error(`Molty is asking your human to install ${args.name}: waiting for their tap (the question stays up 1 min)`);
  const r = await send('pkg.install', args, true);
  if (r.status === 'pending') throw Object.assign(new Error('pkg.install: still waiting for the tap on Molty; run atlas.mjs status later'), { code: 'pending' });
  console.error(`installed ${r.installed ?? PKG} ${r.version ?? version}`);
}
async function ensure() {
  const want = await entry(), have = (await send('pkg.list', {})).packages?.find((p) => p.id === PKG);
  if (have?.version !== want.version) await install(want);
}

// ---- the sky ----
const kindOf = (s) => CAT.kinds.find((k) => [k.id, k.name.toLowerCase(), k.short.toLowerCase()].includes(String(s).toLowerCase()));
function find(s) {
  const t = String(s).toLowerCase().replace(/[^a-z0-9 ]/g, '').trim(), flat = t.replace(/ /g, '');
  const score = (o) => { const n = o.name.toLowerCase().replace(/[^a-z0-9 ]/g, ''); return o.id === t ? 4 : n === t || o.plain?.toLowerCase() === t ? 3 : /^m\d+$/.test(flat) && n.startsWith(flat + ' ') || n === flat ? 3 : n.includes(t) ? 1 : 0; };
  const best = CAT.objects.map((o) => [score(o), o]).filter(([v]) => v).sort((a, b) => b[0] - a[0] || a[1].rank - b[1].rank)[0];
  if (!best) throw Object.assign(new Error(`nothing called ${s}`), { code: 'not_found' });
  return best[1];
}
/** One line, two sentences at most: the name, where to look, when. */
function say(o) {
  const name = o.plain ?? o.name, tz = place?.tz;
  if (o.kind === 'shower') { const p = sky.peak(o, now); return `The ${o.name} peak on ${p.toLocaleDateString('en-GB', { month: 'long', day: 'numeric', timeZone: tz ?? 'UTC' })}, up to ${o.zhr} an hour.`; }
  if (!place) return `${name}. Tell me roughly where you are and I'll show where to look.`;
  const n = sky.night(o, now, place), at = (m) => sky.clock(new Date(now.getTime() + m * 60e3), tz);
  if (n.never) return `${name} never rises from ${place.name}.`;
  if (!n.up) return `${name} is below the horizon. It rises at ${at(n.rise)}.`;
  const w = n.high > 0 && n.highAlt > n.alt + 1 ? `Highest at ${at(n.high)}` : n.set != null ? `It sets at ${at(n.set)}` : 'Up all night';
  return `${name}: ${sky.dir(n.az)}, ${Math.round(n.alt)} degrees up. ${w}.`;
}
/** Every kind's objects, the screen, where to start. */
async function open(kind, o, face) {
  const order = place && opt('--order') === 'up' ? 'up' : 'fame';
  if (!dry) await ensure();                        // (a dry run only prints what it would send)
  const first = kind ?? CAT.kinds[0];
  await send('atlas.load', load(first.id, now, place, order, true));
  for (const k of CAT.kinds) if (k.id !== first.id) await send('atlas.load', load(k.id, now, place, order, false));
  try { await send('scene.play', { pkg: PKG, layout: 'atlas', params: {}, ttl_s: Math.min(600, +opt('--ttl', 600)) }); }
  catch (e) { if (e.code !== 'not_installed') throw e; await install(await entry()); await send('scene.play', { pkg: PKG, layout: 'atlas', params: {}, ttl_s: 600 }); }
  const index = o ? list(o.kind, order, now, place).findIndex((x) => x.id === o.id) : 0;
  await send('atlas.show', o ? { kind: o.kind, level: 'objects', index, face: face === 'where' && place ? 'where' : 'drawing' } : { kind: first.id, level: kind ? 'objects' : 'kinds', index: 0 });
}
const facts = (o) => ({ id: o.id, name: o.name, kind: o.kind, mag: o.mag ?? null, ly: o.ly ?? null, size_arcmin: o.size ?? null, see: o.see, place: place?.name ?? null, ...(place ? (({ az, alt, up }) => ({ direction: sky.dir(az), height: Math.round(alt), up }))(sky.night(o, now, place)) : {}) });

try {
  if (what === 'place') {
    if (flag('--forget')) { rmSync(PLACE, { force: true }); console.log('SAY: Forgotten. I will ask next time.'); process.exit(0); }
    if (opt('--lat') == null) { console.log(place ? JSON.stringify(place) : 'no place kept'); process.exit(0); }
    const r1 = (v) => Math.round(v * 10) / 10, lat = +opt('--lat'), lon = +opt('--lon');
    if (!(Math.abs(lat) <= 90 && Math.abs(lon) <= 180)) throw new Error('--lat -90..90 and --lon -180..180');
    const p = { name: opt('--name', 'here'), lat: r1(lat), lon: r1(lon), tz: opt('--tz', Intl.DateTimeFormat().resolvedOptions().timeZone) };
    mkdirSync(DIR, { recursive: true }); writeFileSync(PLACE, JSON.stringify(p) + '\n', { mode: 0o600 });
    console.log(`SAY: ${p.name}, got it.`); process.exit(0);
  }
  if (what === 'find') { const o = find(words.join(' ')); console.log(`${o.id}\t${o.name}\t${o.kind}`); process.exit(0); }
  if (what === 'install') { await install(await entry()); process.exit(0); }
  if (what === 'status') { const have = (await send('pkg.list', {})).packages?.find((p) => p.id === PKG); console.log(have ? `${PKG} ${have.version}` : `no ${PKG} on Molty`); process.exit(0); }
  if (what === 'open') {
    const k = words.length ? kindOf(words.join(' ')) : null;
    if (words.length && !k) throw new Error(`no kind ${words.join(' ')}: ${CAT.kinds.map((x) => x.id).join(', ')}`);
    await open(k, null);
    console.log(`SAY: ${k ? k.name : 'The night sky.'}`);
    if (!place) console.log(`ASK: ${ASK}`);
    process.exit(0);
  }
  if (what === 'show') {
    const o = find(words.join(' '));
    await open(CAT.kinds.find((k) => k.id === o.kind), o, flag('--where') ? 'where' : 'drawing');
    console.log(`SAY: ${say(o)}`);
    console.log(`FACTS: ${JSON.stringify(facts(o))}`);
    if (!place) console.log(`ASK: ${ASK}`);
    process.exit(0);
  }
  if (what === 'tonight') {
    if (!place) { console.log(`ASK: ${ASK}`); process.exit(3); }
    const see = opt('--see'), ok = (o) => !see || o.see === see || (see === 'binoculars' && o.see === 'eye');
    const up = CAT.objects.filter((o) => o.kind !== 'shower' && ok(o) && sky.pos(o, now, place).alt > 20).sort((a, b) => a.rank - b.rank || a.kind.localeCompare(b.kind)).slice(0, 12);
    for (const o of up) { const p = sky.pos(o, now, place); console.log(`${o.id.padEnd(16)} ${glassName(o).padEnd(22)} ${sky.dir(p.az).padEnd(11)} ${Math.round(p.alt)} up`); }
    if (!up.length) { console.log('SAY: Not much is up yet.'); process.exit(0); }
    await open(CAT.kinds.find((k) => k.id === up[0].kind), up[0], 'where');
    console.log(`SAY: ${say(up[0])}`);
    console.log(`FACTS: ${JSON.stringify(facts(up[0]))}`);
    process.exit(0);
  }
  console.error('usage: atlas.mjs place | open [kind] | show <object> [--where] | tonight [--see binoculars] | find <name> | install | status');
  process.exit(2);
} catch (e) {
  console.error(`molty-atlas: ${e.code === 'unsupported' || e.code === 'unknown_command' ? 'unsupported (this Molty has no atlas yet: update it)' : e.message}`);
  process.exit(1);
}
void byId;
