#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// astro.mjs: show the real sky on Molty with the astrology package.
//   astro.mjs tonight | chart | signs | aspects [kind] | events [kind] [--date YYYY-MM-DD|ISO] [--ttl s] [--dry-run]
//   astro.mjs chart [--all]: every aspect that fits instead of the 3 tightest (the tightest brightest)
//   astro.mjs chart --interactive [--all] [--ttl s] [--dry-run --tap <sign>]: the chart, then a tap on a sign opens it up close
//   astro.mjs install | uninstall | status
// Reads the pairing the molty skill stored (~/.config/molty/env + token; MOLTY_CONFIG_DIR,
// MOLTY_RELAY, MOLTY_DEVICE override). Installs the package first when Molty lacks it (Molty asks
// your human with one tap), then sends scene.play / scene.queue with a few params. Prints
// `SAY: <line>` (speak it, see SKILL.md) and `FACTS: <json>`. Never prints the token.
// --dry-run prints the commands as JSON lines instead of sending them.
import { existsSync, readFileSync } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';
import { EVENTS, SIGNS, aspect, chart, closeup, instant, tonight } from './sky.mjs';

const PKG = 'astrology';
const RAW = 'https://raw.githubusercontent.com/apresmoi/molty.uno/main/';
const INDEX = process.env.MOLTY_PKG_INDEX ?? `${RAW}packages/index.json`;
const DEFAULT_RELAY = 'https://molty-relay.alicenet.workers.dev'; // the molty skill's default relay (MOLTY_RELAY overrides)
const ASPECT_KINDS = ['conjunction', 'sextile', 'square', 'trine', 'opposition'];
// The screens' own lengths (../screens/*.yaml, duration); a scene's ttl covers it, a held one (chart) gets --ttl.
const DUR = { globe: 2.5, saturn: 2.5, sign: 4, signs: 36, aspect: 5, chart: 0, transit: 6, occultation: 6, appulse: 6, 'solar-eclipse': 6, 'lunar-eclipse': 6 };

const argv = process.argv.slice(2), flag = (k) => argv.includes(k), opt = (k, d) => { const i = argv.indexOf(k); return i < 0 ? d : argv[i + 1]; };
const [what, sub] = argv.filter((a, i) => !a.startsWith('--') && !['--date', '--ttl', '--tap'].includes(argv[i - 1]));
const date = instant(opt('--date', new Date().toISOString().slice(0, 10)));
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

/** Molty's events that came with any answer (the relay's ride-along marks them read): --interactive takes them. */
const inbox = [];
/** One authenticated relay request (path under the relay), as JSON. */
async function rest(path, init = {}) {
  const { relay, token } = cfg();
  const out = await (await fetch(relay + path, { ...init, headers: { authorization: `Bearer ${token}`, 'content-type': 'application/json', 'user-agent': 'molty-astro' } })).json();
  if (Array.isArray(out.events)) inbox.push(...out.events);
  return out;
}

/** One relay command; follows a pending job to its end. Returns the result or throws {error}. */
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
  const src = INDEX;
  const index = /^https?:/.test(src) ? await (await fetch(src)).json() : JSON.parse(readFileSync(src, 'utf8'));
  const e = index[PKG];
  if (!e) throw new Error(`no ${PKG} in ${src}`);
  // the package sits next to the index it came from (packages/index.json is one level below the repo root)
  const root = /^https?:/.test(src) ? new URL('..', src).href : RAW;
  return { id: PKG, url: root + e.path, sha256: e.sha256, size: e.size, name: e.name, version: e.version };
}

/** The package on Molty at the index's version: installs it (one tap from your human) when it is missing or older. */
async function ensure() {
  const want = await entry(), have = (await send('pkg.list', {})).packages?.find((p) => p.id === PKG);
  if (have?.version === want.version) return;
  await install(want);
}

/**
 * Molty asks its human with one tap (its question stays up 1 min), answers at once, and reports the outcome later (a
 * `pkg` event the relay turns into this job's end): wait for it, at most ~2 min (the tap, then the download; the
 * relay's deadline, pkg_timeout).
 */
async function install({ version, ...args }) {
  console.error(`Molty is asking your human to install ${args.name}: waiting for their tap (the question stays up 1 min)`);
  const r = await send('pkg.install', args, true);
  if (r.status === 'pending') throw Object.assign(new Error('pkg.install: still waiting for the tap on Molty; run astro.mjs status later to see whether it landed'), { code: 'pending' });
  console.error(`installed ${r.installed ?? PKG} ${r.version ?? version}`);
}

const ttl = (layout) => Math.min(600, +opt('--ttl', 0) || (DUR[layout] ? Math.ceil(DUR[layout]) + 5 : 60));
/** scenes: [[layout, params, ttl_s?], ...]: the first replaces the screen, the rest queue behind it. */
async function play(scenes) {
  for (const [i, [layout, params, ttl_s = ttl(layout)]] of scenes.entries()) {
    const go = () => send(i ? 'scene.queue' : 'scene.play', { pkg: PKG, layout, params, ttl_s: Math.max(1, Math.min(600, Math.ceil(ttl_s))) });
    try {
      await go();
    } catch (e) {
      if (e.code !== 'not_installed') throw e;
      await install(await entry()); // the store was wiped (a factory flash): once more
      await go();
    }
  }
}
/** shows: [{scenes, say, facts}, ...], played in turn. */
async function show(shows) {
  await ensure();
  await play(shows.flatMap((s) => s.scenes));
  const says = shows.map((s) => s.say).filter(Boolean);
  if (says.length) console.log(`SAY: ${says.length > 2 ? says.slice(0, 2).join(' ') + ' And more on screen.' : says.join(' ')}`);
  const facts = shows.map((s) => s.facts).filter(Boolean);
  if (facts.length) console.log(`FACTS: ${JSON.stringify(facts.length === 1 ? facts[0] : facts)}`);
}

/**
 * The chart, held for --ttl (default 300 s), then Molty's taps: a sign's spot opens that sign up close while Molty
 * reads it aloud, and the chart comes back after it (queued) or at a tap on empty space. A planet's spot needs
 * nothing: Molty reads it by itself. Ends at --ttl or Ctrl-C (which clears the screen).
 * --dry-run --tap <sign> prints the commands for one simulated tap.
 */
async function interactive() {
  const t0 = Date.now(), end = t0 + 1000 * Math.min(600, +opt('--ttl', 300)), left = () => (end - Date.now()) / 1000;
  const sky = chart(date, { all: flag('--all') }), [[, board]] = sky.scenes, signs = new Set(SIGNS.map((n) => n.toLowerCase()));
  await show([{ ...sky, scenes: [['chart', board, left()]] }]);
  let open = null, until = 0; // the sign up close, and when its close-up gives way to the chart
  const tap = async (spot) => {
    if (Date.now() >= until) open = null;
    if (spot && signs.has(spot) && spot !== open) {
      const c = closeup(date, spot), [[, , len]] = c.scenes;
      await play(left() > len + 1 ? [...c.scenes, ['chart', board, left() - len]] : c.scenes);
      try { await send('say', { text: c.read }); } catch (e) { if (e.code !== 'audio_busy') throw e; console.error('molty-astro: Molty is still speaking; the close-up shows without its reading'); }
      [open, until] = [spot, Date.now() + 1000 * len];
      console.error(`molty-astro: ${spot} up close`);
    } else if (!spot && open) {
      await play([['chart', board, left()]]);
      open = null;
    }
  };
  if (dry) return opt('--tap') && tap(opt('--tap'));
  const stop = new AbortController();
  process.once('SIGINT', () => stop.abort());
  const { device } = cfg();
  while (left() > 1 && !stop.signal.aborted) {
    try {
      const got = await rest(`/v1/devices/${device}/events?wait_s=${Math.min(25, Math.floor(left()))}`, { signal: stop.signal });
      if (!got.ok) throw new Error(`events: ${got.error}`);
      inbox.push(...(got.events ?? []));
    } catch (e) {
      if (stop.signal.aborted) break;
      throw e;
    }
    // taps since we started, in order (the inbox may hold older ones)
    for (const e of inbox.splice(0)) if (e.type === 'tap' && e.ts >= t0 - 2000) await tap(e.data?.spot);
  }
  if (stop.signal.aborted) await send('scene.clear', {});
}

try {
  switch (what) {
    case 'tonight': await show([tonight(date)]); break;
    case 'chart': await (flag('--interactive') ? interactive() : show([chart(date, { all: flag('--all') })])); break;
    case 'signs': await show([{ scenes: [['signs', {}]], say: 'The twelve signs.' }]); break;
    case 'aspects': await show((sub ? [sub] : ASPECT_KINDS).map((k) => aspect(k, date))); break;
    case 'events': {
      const kinds = sub ? [sub] : Object.keys(EVENTS);
      if (!kinds.every((k) => EVENTS[k])) throw new Error(`event must be one of ${Object.keys(EVENTS).join(', ')}`);
      await show(kinds.map((k) => EVENTS[k](date)));
      break;
    }
    case 'install': await ensure(); break;
    case 'uninstall': await send('pkg.remove', { id: PKG }); console.log(`removed ${PKG}`); break;
    case 'status': console.log(JSON.stringify(await send('pkg.list', {}))); break;
    default: throw new Error('usage: astro.mjs tonight|chart [--interactive [--tap sign]]|signs|aspects [kind]|events [kind]|install|uninstall|status [--date YYYY-MM-DD] [--ttl s] [--dry-run]');
  }
} catch (e) {
  console.error(`molty-astro: ${e.message}`);
  process.exit(e.code === 'unsupported' || e.code === 'firmware_too_old' ? 3 : 1);
}
