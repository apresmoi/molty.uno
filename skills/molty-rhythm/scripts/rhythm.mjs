#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// rhythm.mjs: a rhythm on Molty, as a clock that plays it (the rhythm package and Molty's beat player).
//   rhythm.mjs play <id> | --region <region> [--bpm n] [--mute drum,drum] [--buzz] [--still] [--ttl s] [--sync ms]
//                 [--tick percent] [--tick-ms ms]
//   rhythm.mjs set [--bpm n] [--rhythm id | --region region] [--mute drum,drum|none] [--next] [--buzz on|off]
//                 [--pause|--resume] [--sync ms]
//   rhythm.mjs calibrate [--sync ms] [--bpm n]   one click a beat and one flash: the picture against the sound
//   rhythm.mjs stop | list [--region region] | regions | check | install | uninstall | status
// Reads the pairing the molty skill stored (~/.config/molty/env + token; MOLTY_CONFIG_DIR, MOLTY_RELAY, MOLTY_DEVICE
// override). `play` installs the package first when Molty lacks it (Molty asks your human with one tap), shows the
// clock and sends the whole library once: the tempo, the drums and the wheel then live on Molty. Prints
// `SAY: <line>` (speak it, see SKILL.md) and `FACTS: <json>`. Never prints the token.
// --dry-run prints the commands as JSON lines instead of sending them.
import { existsSync, readFileSync } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';
import { LIMITS, REGIONS, load, regionsOf, wireLibrary } from './library.mjs';

const PKG = 'rhythm', LAYOUT = 'clock';
const RAW = 'https://raw.githubusercontent.com/apresmoi/molty.uno/main/';
const INDEX = process.env.MOLTY_PKG_INDEX ?? `${RAW}packages/index.json`;
const DEFAULT_RELAY = 'https://molty-relay.alicenet.workers.dev'; // the molty skill's default relay (MOLTY_RELAY overrides)

const VALUED = ['--bpm', '--mute', '--ttl', '--rhythm', '--buzz', '--tick', '--tick-ms', '--region', '--sync'];
const argv = process.argv.slice(2), flag = (k) => argv.includes(k), opt = (k, d) => { const i = argv.indexOf(k); return i < 0 ? d : argv[i + 1]; };
const [what, sub] = argv.filter((a, i) => !a.startsWith('--') && !VALUED.includes(argv[i - 1]));
const dry = flag('--dry-run');

function config() {
  const dir = process.env.MOLTY_CONFIG_DIR ?? join(homedir(), '.config/molty'), env = {};
  if (existsSync(join(dir, 'env'))) for (const l of readFileSync(join(dir, 'env'), 'utf8').split('\n')) { const m = /^\s*(?:export\s+)?([A-Z_]+)=["']?([^"'\s]*)/.exec(l); if (m) env[m[1]] = m[2]; }
  const relay = (process.env.MOLTY_RELAY ?? env.MOLTY_RELAY ?? DEFAULT_RELAY).replace(/\/$/, ''), device = process.env.MOLTY_DEVICE ?? env.MOLTY_DEVICE;
  const token = existsSync(join(dir, 'token')) ? readFileSync(join(dir, 'token'), 'utf8').trim() : '';
  if (!device || !token) throw new Error('not paired: pair with the molty skill first (scripts/pair.sh <DEVICE> <CODE>)');
  return { relay, device, token };
}
const cfg = () => (cfg.v ??= config());

async function rest(path, init = {}) {
  const { relay, token } = cfg();
  return (await fetch(relay + path, { ...init, headers: { authorization: `Bearer ${token}`, 'content-type': 'application/json', 'user-agent': 'molty-rhythm' } })).json();
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
/** The package on Molty at the index's version: installs it (one tap from your human) when missing or older. */
async function ensure() {
  const want = await entry(), have = (await send('pkg.list', {})).packages?.find((p) => p.id === PKG);
  if (have?.version !== want.version) await install(want);
}
async function install({ version, ...args }) {
  console.error(`Molty is asking your human to install ${args.name}: waiting for their tap (the question stays up 1 min)`);
  const r = await send('pkg.install', args, true);
  if (r.status === 'pending') throw Object.assign(new Error('pkg.install: still waiting for the tap on Molty; run rhythm.mjs status later'), { code: 'pending' });
  console.error(`installed ${r.installed ?? PKG} ${r.version ?? version}`);
}

const LIB = load();
const find = (id) => LIB.find((r) => r.id === id) ?? (() => { throw new Error(`no rhythm ${id}: one of ${LIB.map((r) => r.id).join(', ')}`); })();
/** A region's first rhythm (the wheel's order). */
const firstOf = (region) => LIB.find((r) => r.region === region) ?? (() => { throw new Error(`no region ${region}: one of ${regionsOf(LIB).map((g) => g.id).join(', ')}`); })();
/** --sync ms: the picture against the sound, kept on Molty (positive: the picture earlier). */
const syncOf = () => {
  if (opt('--sync') === undefined) return {};
  const v = Math.round(+opt('--sync'));
  if (!(v >= -250 && v <= 250)) throw new Error('--sync must be -250..250 (ms)');
  return { sync: v };
};
const bpmOf = (v) => {
  const n = Math.round(+v);
  if (!(n >= LIMITS.bpm[0] && n <= LIMITS.bpm[1])) throw new Error(`--bpm must be ${LIMITS.bpm[0]}..${LIMITS.bpm[1]}`);
  return n;
};
const mutes = (r, v) => {
  if (v === undefined) return undefined;
  if (v === 'none') return [];
  const ids = v.split(',');
  for (const id of ids) if (r && !r.layers[id]) throw new Error(`${r.id} has no drum ${id}: ${Object.keys(r.layers).join(', ')}`);
  return ids;
};
const facts = (r, bpm) => ({ id: r.id, name: r.name, bpm, status: r.status, steps: r.steps, beats: r.beats,
  drums: Object.entries(r.layers).map(([id, L]) => ({ id, name: L.name, sound: L.sound })), ...(r.phrase ? { phrase: r.phrase.order.join(' ') } : {}),
  tempo_range: r.tempo.range ?? null, source: r.source });

/** The clock on Molty (the package installed first), then the library with `pick` chosen. */
async function show(library, pickIndex, extra) {
  const ttl = Math.max(10, Math.min(600, +opt('--ttl', 600)));
  // the rim's tick on the motor (0: Molty's own, 22 ms at 80 percent); for tuning it by feel
  const tick = (k, lo, hi) => { const v = Math.round(+opt(k, 0)); if (v && !(v >= lo && v <= hi)) throw new Error(`${k} must be ${lo}..${hi} (or 0)`); return v; };
  const params = { tick_ms: tick('--tick-ms', 5, 60), tick: tick('--tick', 25, 100) };
  await ensure();
  const scene = () => send('scene.play', { pkg: PKG, layout: LAYOUT, params, ttl_s: ttl });
  try { await scene(); } catch (e) {
    if (e.code !== 'not_installed') throw e;
    await install(await entry());   // the store was wiped (a factory flash): once more
    await scene();
  }
  return send('rhythm.play', { ...library, pick: pickIndex, ...extra, ...syncOf() });
}

async function play() {
  const byRegion = !sub && opt('--region') !== undefined;
  const r = byRegion ? firstOf(opt('--region')) : find(sub), bpm = opt('--bpm') ? bpmOf(opt('--bpm')) : r.tempo.bpm, mute = mutes(r, opt('--mute')), still = flag('--still') || byRegion;
  await show(wireLibrary(LIB), LIB.indexOf(r), { bpm, ...(mute?.length ? { mute } : {}), ...(flag('--buzz') ? { buzz: true } : {}), ...(still ? { paused: true } : {}) });
  const off = mute?.map((id) => r.layers[id].name.split(',')[0].toLowerCase()).join(' and ');
  const region = REGIONS.find((g) => g.id === r.region).name;
  console.log(`SAY: ${byRegion ? `${region}: turn the rim to choose, tap the middle to play.` : still ? `${r.name}. Tap the middle to start.` : off ? `${r.name} without ${off}.` : `${r.name}, at ${bpm}.`}`);
  console.log(`FACTS: ${JSON.stringify(facts(r, bpm))}`);
}

/** One click a beat, one flash: where the picture and the sound part is easiest to see and hear. */
async function calibrate() {
  const bpm = opt('--bpm') ? bpmOf(opt('--bpm')) : 60;
  const one = { id: 'sync-check', label: 'SYNC CHECK', steps: 4, per: 1, beats: [1, 2, 3, 4], region: 0, count_in: 0,
    layers: [{ id: 'click', sound: 'click', color: '#ffffff', hits: [1, 2, 3, 4], accent: [1] }] };
  const got = await show({ regions: [{ label: 'CALIBRATE', color: '#cfd3e0' }], rhythms: [one] }, 0, { bpm });
  console.log('SAY: A click on every beat. If the flash comes before the click, sync down; after it, sync up.');
  if (got && Object.keys(got).length) console.log(`FACTS: ${JSON.stringify(got)}`);
}

async function set() {
  const r = opt('--rhythm') ? find(opt('--rhythm')) : opt('--region') !== undefined ? firstOf(opt('--region')) : null, args = { ...syncOf() };
  if (opt('--bpm')) args.bpm = bpmOf(opt('--bpm'));
  if (r) args.pick = LIB.indexOf(r);
  if (opt('--mute') !== undefined) args.mute = mutes(r, opt('--mute'));
  if (flag('--next')) args.next = true;
  if (opt('--buzz') !== undefined) args.buzz = opt('--buzz') === 'on';
  if (flag('--pause')) args.paused = true;    // stopped on the clock, choosing (the human taps the middle to play)
  if (flag('--resume')) args.paused = false;
  if (!Object.keys(args).length) throw new Error('set what? --bpm, --rhythm, --region, --mute, --next, --buzz, --pause, --resume or --sync');
  const got = await send('rhythm.set', args);
  if (r) console.log(`SAY: ${r.name}${args.bpm ? `, at ${args.bpm}` : ''}.`);
  else if (args.bpm) console.log(`SAY: ${args.bpm}.`);
  if (got && Object.keys(got).length) console.log(`FACTS: ${JSON.stringify(got)}`);
}

try {
  switch (what) {
    case 'play': if (!sub && opt('--region') === undefined) throw new Error('play which rhythm? rhythm.mjs list (or --region, rhythm.mjs regions)'); await play(); break;
    case 'set': await set(); break;
    case 'stop': await send('rhythm.stop', {}); break;
    case 'list':
      for (const r of LIB.filter((x) => opt('--region') === undefined || x.region === opt('--region')))
        console.log(`${r.id.padEnd(17)} ${r.name.padEnd(22)} ${r.region.padEnd(13)} ${String(r.tempo.bpm).padStart(3)} bpm  ${Object.keys(r.layers).join(', ')}`);
      break;
    case 'regions': for (const g of regionsOf(LIB)) console.log(`${g.id.padEnd(14)} ${g.name.padEnd(34)} ${LIB.filter((r) => r.region === g.id).length} rhythms`); break;
    case 'calibrate': await calibrate(); break;
    case 'check': console.log(`${LIB.length} rhythms in ${regionsOf(LIB).length} regions, ${LIB.filter((r) => r.status === 'verified').length} verified, ${JSON.stringify(wireLibrary(LIB)).length} bytes sent`); break;
    case 'install': await ensure(); break;
    case 'uninstall': await send('pkg.remove', { id: PKG }); console.log(`removed ${PKG}`); break;
    case 'status': console.log(JSON.stringify(await send('pkg.list', {}))); break;
    default: throw new Error('usage: rhythm.mjs play <id> | --region <r> [--bpm n] [--mute drum] [--buzz] [--still] [--sync ms] [--tick pct] [--tick-ms ms] | set ... | calibrate | stop | list | regions | check | install | uninstall | status [--dry-run]');
  }
} catch (e) {
  console.error(`molty-rhythm: ${e.message}`);
  process.exit(e.code === 'unsupported' || e.code === 'firmware_too_old' ? 3 : 1);
}
