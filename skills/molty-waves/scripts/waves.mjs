#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// waves.mjs: a note, an interval or a chord on Molty, as one drum hit at different speeds (the waves package and
// Molty's tone player).
//   waves.mjs play <note> [<chord>] [--count | --hz n | --octave n] [--tuning just|equal] [--ttl s]
//   waves.mjs show <note> [<chord>]        stopped on the chooser (a tap in the middle plays it)
//   waves.mjs set [--count | --hz n | --octave n] [--note X] [--chord K] [--tuning T] [--mute 2,3|none] [--pause|--resume]
//   waves.mjs stop | list | install | uninstall | status
// <note>: C, C#, Db ... B (a flat as b). <chord>: an id or a name from data/chords.json (major, minor, major-7,
// "minor seventh", maj7, ...). Reads the pairing the molty skill stored (~/.config/molty/env + token; MOLTY_CONFIG_DIR,
// MOLTY_RELAY, MOLTY_DEVICE override). `play` and `show` install the package first when Molty lacks it (Molty asks your
// human with one tap), show the scope and send the chord kinds once. Prints `SAY: <line>` and `FACTS: <json>`. Never
// prints the token. --dry-run prints the commands as JSON lines instead of sending them.
import { existsSync, readFileSync } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';
import { CHORDS, NOTES, wire } from './chords.mjs';

const PKG = 'waves', LAYOUT = 'scope';
const RAW = 'https://raw.githubusercontent.com/apresmoi/molty.uno/main/';
const INDEX = process.env.MOLTY_PKG_INDEX ?? `${RAW}packages/index.json`;
const DEFAULT_RELAY = 'https://molty-relay.alicenet.workers.dev'; // the molty skill's default relay (MOLTY_RELAY overrides)

const VALUED = ['--hz', '--octave', '--note', '--chord', '--tuning', '--mute', '--ttl'];
const argv = process.argv.slice(2), flag = (k) => argv.includes(k), opt = (k, d) => { const i = argv.indexOf(k); return i < 0 ? d : argv[i + 1]; };
const [what, a1, a2] = argv.filter((a, i) => !a.startsWith('--') && !VALUED.includes(argv[i - 1]));
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
  return (await fetch(relay + path, { ...init, headers: { authorization: `Bearer ${token}`, 'content-type': 'application/json', 'user-agent': 'molty-waves' } })).json();
}
/** One relay command; follows a pending job to its end. Returns the result or throws {code}. */
async function send(name, args, wait = false) {
  if (dry) { console.log(JSON.stringify({ name, args })); return name === 'pkg.list' ? { packages: [{ id: PKG, version: 'dry' }] } : {}; }
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
  if (r.status === 'pending') throw Object.assign(new Error('pkg.install: still waiting for the tap on Molty; run waves.mjs status later'), { code: 'pending' });
  console.error(`installed ${r.installed ?? PKG} ${r.version ?? version}`);
}
/** The package on Molty at the index's version: installs it (one tap from your human) when missing or older. */
async function ensure() {
  if (dry) return;
  const want = await entry(), have = (await send('pkg.list', {})).packages?.find((p) => p.id === PKG);
  if (have?.version !== want.version) await install(want);
}

const hz = (m) => 440 * 2 ** ((m - 69) / 12);
function note(s) {
  const t = String(s ?? 'C').trim(), n = NOTES.find((q) => [q.sharp, q.flat].some((v) => v.toLowerCase() === t.toLowerCase()));
  if (!n) throw new Error(`no note ${s}: C, C#, Db ... B`);
  return n.pc;
}
function chord(s) {
  const t = String(s).trim().toLowerCase(), k = CHORDS.findIndex((c) => [c.id, c.name, c.short].some((v) => String(v).toLowerCase() === t));
  if (k < 0) throw new Error(`no chord ${s}: ${CHORDS.map((c) => c.id).join(', ')}`);
  return k;
}
const label = (pc, c) => (c.id === 'alone' ? NOTES[pc][c.spell_in] : `${NOTES[pc][c.spell_in]} ${c.name}`);
/** The pitch asked for: --hz, --octave, --count (Molty works it out: the clock about a turn a second), or none. */
function pitch(pc, play) {
  const p = {};
  if (opt('--hz') !== undefined) { const v = +opt('--hz'); if (!(v >= 1 && v <= 5760)) throw new Error('--hz must be 1..5760'); p.hz = v; }
  if (opt('--octave') !== undefined) { const o = Math.round(+opt('--octave')); if (!(o >= -3 && o <= 8)) throw new Error('--octave must be -3..8'); play ? (p.midi = 12 * (o + 1) + pc) : (p.octave = o); }
  if (flag('--count')) {
    if (!play) p.count = true;
    else { let m = 60 + pc; while (hz(m) / CHORDS[chord(a2 ?? 'alone')].ratio[0] > 1.4) m -= 12; p.midi = m; }
  }
  if (Object.keys(p).length > 1) throw new Error('one of --hz, --octave, --count');
  return p;
}
const tuning = () => { const t = opt('--tuning'); if (t === undefined) return {}; if (t !== 'just' && t !== 'equal') throw new Error('--tuning just or equal'); return { tuning: t }; };
const facts = (pc, c) => ({ id: c.id, name: label(pc, c), ratio: c.ratio, semis: c.semis, cents_off: c.cents_off, about: c.about });

async function show(paused) {
  const pc = note(a1), k = chord(a2 ?? (paused ? 'major' : 'alone')), c = CHORDS[k];
  const ttl = Math.max(10, Math.min(600, +opt('--ttl', 600)));
  await ensure();
  const scene = () => send('scene.play', { pkg: PKG, layout: LAYOUT, params: {}, ttl_s: ttl });
  try { await scene(); } catch (e) {
    if (e.code !== 'not_installed') throw e;
    await install(await entry());   // the store was wiped (a factory flash): once more
    await scene();
  }
  const got = await send('wave.play', { chords: wire(), pick: k, note: pc, ...pitch(pc, true), ...tuning(), ...(paused ? { paused: true } : {}) });
  console.log(`SAY: ${paused ? `${label(pc, c)}. Turn the rim for another, tap the middle to play.` : flag('--count') ? `${label(pc, c)}: ${c.ratio.join(', ')}. Count the dots, then turn it up.` : `${label(pc, c)}.`}`);
  console.log(`FACTS: ${JSON.stringify({ ...facts(pc, c), ...got })}`);
}

async function set() {
  const args = { ...tuning() };
  if (opt('--note') !== undefined) args.note = note(opt('--note'));
  if (opt('--chord') !== undefined) args.pick = chord(opt('--chord'));
  if (opt('--mute') !== undefined) args.mute = opt('--mute') === 'none' ? [] : opt('--mute').split(',').map(Number);
  Object.assign(args, pitch(args.note ?? 0, false));
  if (flag('--pause')) args.paused = true;
  if (flag('--resume')) args.paused = false;
  if (!Object.keys(args).length) throw new Error('set what? --count, --hz, --octave, --note, --chord, --tuning, --mute, --pause or --resume');
  const got = await send('wave.set', args);
  console.log(`SAY: ${args.count ? 'One turn a second. Count the dots.' : args.octave === 4 ? 'Back to the middle of the piano.' : ''}`);
  if (got && Object.keys(got).length) console.log(`FACTS: ${JSON.stringify(got)}`);
}

try {
  switch (what) {
    case 'play': await show(false); break;
    case 'show': await show(true); break;
    case 'set': await set(); break;
    case 'stop': await send('wave.stop', {}); break;
    case 'list': for (const c of CHORDS) console.log(`${c.id.padEnd(16)} ${c.on_c.padEnd(14)} ${c.ratio.join(':').padEnd(16)} ${c.about}`); break;
    case 'install': await ensure(); break;
    case 'uninstall': await send('pkg.remove', { id: PKG }); console.log(`removed ${PKG}`); break;
    case 'status': console.log(JSON.stringify(await send('pkg.list', {}))); break;
    default: throw new Error('usage: waves.mjs play <note> [<chord>] [--count|--hz n|--octave n] [--tuning just|equal] | show <note> [<chord>] | set ... | stop | list | install | uninstall | status [--dry-run]');
  }
} catch (e) {
  console.error(`molty-waves: ${e.message}`);
  process.exit(e.code === 'unsupported' || e.code === 'unknown_command' || e.code === 'firmware_too_old' ? 3 : 1);
}
