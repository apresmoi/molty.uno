#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// check-skill.mjs — the agent skill (skills/molty/) and its plugin packaging, checked
// against the repo (run by scripts/hygiene.sh). The relay no longer serves the skill, so
// what its tests used to pin about the served text is checked here instead:
//   1. SKILL.md frontmatter (name, description) and a short core
//   2. every reference is linked from SKILL.md; every relative link (and #anchor) resolves
//   3. no hard-coded relay host: curl examples use $MOLTY_RELAY; the official relay URL only
//      where MOLTY_RELAY's default is defined
//   4. the skill documents what the relay has: every command, mood and MCP tool, and the
//      api_version it targets equals the relay's API_VERSION
//   5. load-bearing rules are still stated (shake to talk, ack, no_speech, caps, …)
//   6. plugin manifests (Claude Code + Codex) agree; the plugin root (the repo) holds no
//      symlinks and no script/document >= 256 KiB (the plugin loaders refuse both)
//   7. every place that names the skill's home agrees (relay redirect, MCP, console)
// Exits non-zero listing every problem.
import { execFileSync } from 'node:child_process';
import { existsSync, lstatSync, readFileSync, readdirSync } from 'node:fs';
import { dirname, join, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const SKILL = join(ROOT, 'skills/molty');
const HOME = 'https://github.com/apresmoi/molty.uno/tree/main/skills/molty';
const RAW = 'https://raw.githubusercontent.com/apresmoi/molty.uno/main/skills/molty/SKILL.md';
const MAX_SKILL_CHARS = 7000;
const MAX_PLUGIN_FILE = 256 * 1024;

const problems = [];
const bad = (msg) => problems.push(msg);
const read = (p) => readFileSync(join(ROOT, p), 'utf8');
const rel = (p) => relative(ROOT, p);

function walk(dir, out = []) {
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, e.name);
    if (e.isDirectory()) walk(p, out);
    else out.push(p);
  }
  return out;
}
const mdFiles = walk(SKILL).filter((p) => p.endsWith('.md'));
const allText = Object.fromEntries(walk(SKILL).filter((p) => !p.includes('__pycache__')).map((p) => [p, readFileSync(p, 'utf8')]));
const skillMd = allText[join(SKILL, 'SKILL.md')];
if (!skillMd) { console.error('::error::skills/molty/SKILL.md is missing'); process.exit(1); }

// 1. frontmatter + size
const fm = /^---\nname: ([a-z0-9-]+)\ndescription: (.+)\n---\n/.exec(skillMd);
if (!fm) bad('SKILL.md must start with ---/name/description/--- frontmatter');
else {
  if (fm[1] !== 'molty') bad(`SKILL.md name is ${fm[1]}, expected molty`);
  if (fm[2].length > 1024) bad(`SKILL.md description is ${fm[2].length} chars (max 1024)`);
}
if ([...skillMd].length > MAX_SKILL_CHARS) bad(`SKILL.md is ${[...skillMd].length} chars: keep the core under ${MAX_SKILL_CHARS}, move detail to references/`);
if (readdirSync(join(ROOT, 'skills')).filter((n) => !n.startsWith('.')).join() !== 'molty') bad('skills/ must hold exactly one skill: molty');

// 2. links
// GitHub's anchors: lower case, punctuation dropped except - and _, spaces to -
const slug = (h) => h.trim().toLowerCase().replace(/[^\p{L}\p{N} _-]/gu, '').replace(/ /g, '-');
const anchors = (text) => new Set([...text.matchAll(/^#{1,6} (.+)$/gm)].map((m) => slug(m[1])));
for (const file of mdFiles) {
  const text = allText[file];
  for (const m of text.matchAll(/\]\(([^)\s]+)\)/g)) {
    const target = m[1];
    if (/^(https?:|mailto:)/.test(target)) continue;
    const [path, anchor] = target.split('#');
    const dest = path ? resolve(dirname(file), path) : file;
    if (path && !dest.startsWith(SKILL)) { bad(`${rel(file)}: link ${target} leaves the skill (use a GitHub URL)`); continue; }
    if (!existsSync(dest)) { bad(`${rel(file)}: broken link ${target}`); continue; }
    if (anchor && dest.endsWith('.md') && !lstatSync(dest).isDirectory() && !anchors(readFileSync(dest, 'utf8')).has(anchor)) bad(`${rel(file)}: no heading #${anchor} in ${rel(dest)}`);
  }
}
for (const file of mdFiles) {
  if (file.endsWith('SKILL.md')) continue;
  if (!skillMd.includes(`](${relative(SKILL, file)})`)) bad(`${rel(file)} is not linked from SKILL.md's index`);
}
for (const m of skillMd.matchAll(/`([a-z_]+\.(?:sh|py))\b/g)) if (!existsSync(join(SKILL, 'scripts', m[1]))) bad(`SKILL.md names scripts/${m[1]}, which does not exist`);

// 3. relay hosts
const DEFAULT_RELAY = /https:\/\/molty-relay\.alicenet\.workers\.dev/;
for (const [file, text] of Object.entries(allText)) {
  text.split('\n').forEach((line, i) => {
    const at = `${rel(file)}:${i + 1}`;
    for (const m of line.matchAll(/(?:https?|wss?):\/\/([^\s/`'")]+)/g)) {
      const host = m[1];
      if (host.includes('<') || host.startsWith('…') || host.startsWith('$')) continue; // placeholder
      if (/^(github\.com|raw\.githubusercontent\.com|molty\.uno|example\.org)$/.test(host)) continue;
      if (DEFAULT_RELAY.test(m[0]) && /MOLTY_RELAY|DEFAULT_RELAY|default relay/i.test(line)) continue;
      bad(`${at}: hard-coded host ${host} (write $MOLTY_RELAY, or a <placeholder>)`);
    }
    if (file.endsWith('.md') && /\bcurl\b/.test(line) && /\/v1\//.test(line) && !/\$MOLTY_RELAY|<[^>]+>\.workers\.dev/.test(line)) bad(`${at}: curl to the relay without $MOLTY_RELAY`);
  });
}
if (!DEFAULT_RELAY.test(read('console/src/agents/agents.ts'))) bad('the skill\'s default relay differs from the console\'s DEFAULT_RELAY');

// 4. documented vs implemented
const all = Object.values(allText).join('\n');
const tsList = (src, name) => JSON.parse(`[${(new RegExp(`export const ${name} = \\[([^\\]]*)\\]`).exec(src) || [, ''])[1]}]`);
const commands = read('relay/src/core/commands.ts');
const api = allText[join(SKILL, 'references/api.md')] ?? '';
for (const c of tsList(commands, 'COMMANDS')) if (!api.includes(`| \`${c}\` |`)) bad(`api.md has no row for command ${c}`);
for (const m of tsList(commands, 'MOODS')) if (!api.includes(m)) bad(`api.md does not list mood ${m}`);
const tools = [...read('relay/src/surfaces/mcp.ts').matchAll(/name: "(molty_\w+)"/g)].map((m) => m[1]);
if (tools.length < 10) bad('could not read the MCP tool names from relay/src/surfaces/mcp.ts');
for (const t of tools) if (!api.includes(`\`${t}\``)) bad(`api.md does not list MCP tool ${t}`);
const relayApi = Number((/export const API_VERSION = (\d+);/.exec(read('relay/src/core/version.ts')) || [])[1]);
const skillApi = Number((/written for \*\*api_version (\d+)\*\*/.exec(all) || [])[1]);
if (!relayApi || relayApi !== skillApi) bad(`the skill targets api_version ${skillApi}, the relay speaks ${relayApi}`);
for (const [k, n] of [['MAX_ASK_LABEL', /1–(\d+) characters each/], ['MAX_ASK_OPTIONS', /"options": \[1\.\.(\d+) labels\]/], ['MAX_SAY_BYTES', /≤ (\d+) bytes; job done/]]) {
  const code = Number((new RegExp(`export const ${k} = (\\d+)`).exec(commands) || [])[1]);
  const doc = Number((n.exec(api) || [])[1]);
  if (code !== doc) bad(`api.md states ${k} = ${doc}, relay/src/core/commands.ts has ${code}`);
}

// 5. load-bearing rules (they used to be pinned by the relay's skill tests)
const FACTS = {
  'references/api.md': ['agent_token', 'pair_id', 'wait_s', '120 s', 'Authorization: Bearer', 'audio_busy', 'turn_claimed', 'firmware_too_old', 'listen_failed', 'A listen that broke is an error, never `no_speech`', 'one centred button', 'pie slices', '`timeout`', '`cancelled`', '`offline`', 'use **talk** when you', '{"label": "yes", "color": "green"}', 'green, red, amber, blue, violet, pink, cyan, white', 'green yes, red no', '"ack"?: true\\|false\\|"silent"'],
  'references/events.md': ['a shake means "I want to talk"', "**don't react to it**, the `utterance` follows", 'nothing at all when nobody spoke', 'The first agent to answer a turn claims it', '`409 turn_claimed`', '`timings.turn` `{id, reply, first_audio}`', '{{event.data.<field>}}', '{{event.data.ms.speech}}', '"events":["utterance"]', 'Molty-Signature: t=<unix ms>,v2=<hex hmac_sha256(secret, t \\n METHOD \\n path?query \\n body)>', 'never speak while the human talks', 'Nothing can wake an agent', 'Not an answer to a question', 'molty events', '--exec'],
  'references/talking-fast.md': ['Act on the event payload directly', 'no lock files', 'fastest model for conversation', 'say one short line first', 'Short first sentence', 'Keep a warm session', 'Check `relay.caps`, not versions', 'Molty acknowledges the turn itself', 'go straight to your answer', 'Never leave the human in silence', 'After a wake, meet `no_speech` with one short line or nothing — never a burst', '{"say":"I\'m here.","listen":false}', "Don't call `mood` around talking"],
  'references/etiquette.md': ['acknowledge / dismiss (never yes / no)', 'Ask sparingly', 'behaviour ledger'],
  'references/updates.md': ['When your human asks to check Molty for updates (any wording', '**ask before updating**', 'firmware_too_old', '`firmware_update`', '`can_self_update`', 'read `relay.caps`'.replace('read ', ''), 'claude plugin update molty@molty', 'codex plugin add molty@molty', 'deprecated'],
  'references/voice.md': ['`relay.voice`', '[stt unavailable]', 'voice_box_offline'],
  'references/self-hosting.md': ['d1_databases', 'pnpm run deploy', 'relay_url', 'Tokens belong to the relay'],
  'SKILL.md': ['Never print', 'in_reply_to', '`relay.caps`', 'acknowledge / dismiss, never an answer'],
};
for (const [file, facts] of Object.entries(FACTS)) {
  const text = allText[join(SKILL, file)] ?? '';
  for (const f of facts) if (!text.includes(f)) bad(`${file} no longer states: ${f}`);
}
if (/accept\/reject = yes\/no|accept = yes/i.test(all)) bad('buttons are acknowledge / dismiss, never yes / no');
if (/skill_update|skill\.updated|\/skill\.md\b(?! is \*\*deprecated)/.test(all.replace(/`\$MOLTY_RELAY\/skill\.md` is \*\*deprecated\*\*/, '').replace(/old `\$MOLTY_RELAY\/skill\.md`/, ''))) bad('the skill still refers to the relay-served skill (skill_update / skill.updated / /skill.md)');

// 6. plugin manifests + plugin root constraints
const json = (p) => { try { return JSON.parse(read(p)); } catch (e) { bad(`${p}: ${e.message}`); return {}; } };
const claudeM = json('.claude-plugin/marketplace.json');
const claudeP = json('.claude-plugin/plugin.json');
const codexP = json('.codex-plugin/plugin.json');
const codexM = json('.agents/plugins/marketplace.json');
for (const [p, m] of [['.claude-plugin/plugin.json', claudeP], ['.codex-plugin/plugin.json', codexP]]) {
  if (m.name !== 'molty') bad(`${p}: name must be molty`);
  if (m.description !== claudeP.description) bad(`${p}: description differs from .claude-plugin/plugin.json`);
  if (m.repository !== 'https://github.com/apresmoi/molty.uno') bad(`${p}: repository must be the repo`);
  if ('version' in m) bad(`${p}: no version — installs follow the repo's commits (see docs/AGENT-LINK.md "Agent skill")`);
}
if (codexP.skills !== './skills/') bad('.codex-plugin/plugin.json: skills must be ./skills/');
for (const [p, m] of [['.claude-plugin/marketplace.json', claudeM], ['.agents/plugins/marketplace.json', codexM]]) {
  if (m.name !== 'molty' || m.plugins?.length !== 1 || m.plugins[0].name !== 'molty') bad(`${p}: one plugin molty in marketplace molty`);
  const src = m.plugins?.[0]?.source;
  if ((typeof src === 'string' ? src : src?.path) !== './') bad(`${p}: the plugin root is the repo root (source ./): Codex only finds skills under <root>/skills/<name>/`);
}
const tracked = execFileSync('git', ['ls-files', '-z'], { cwd: ROOT, encoding: 'utf8' }).split('\0').filter(Boolean);
for (const f of tracked) {
  const p = join(ROOT, f);
  if (!existsSync(p) && !lstatSync(p, { throwIfNoEntry: false })) continue;
  const st = lstatSync(p);
  if (st.isSymbolicLink()) bad(`${f}: symlink in the plugin root (the plugin loaders refuse them)`);
  else if (!/\.(png|jpe?g|gif|webp|woff2?|bin|ico)$/i.test(f) && st.size >= MAX_PLUGIN_FILE) bad(`${f}: ${st.size} bytes; plugin loaders read scripts/documents only below 256 KiB`);
}

// 7. one home for the skill
const homes = {
  'relay/src/surfaces/skill.ts': RAW,
  'relay/src/surfaces/mcp.ts': HOME,
  'console/src/agents/agents.ts': HOME,
};
for (const [p, url] of Object.entries(homes)) if (!read(p).includes(`"${url}"`) && !read(p).includes(`'${url}'`)) bad(`${p} must name the skill at ${url}`);

if (problems.length) {
  for (const p of problems) console.log(`::error::skill: ${p}`);
  console.log(`check-skill: FAILED (${problems.length})`);
  process.exit(1);
}
console.log(`check-skill: ok (${mdFiles.length} documents, ${tools.length} MCP tools, api_version ${relayApi})`);
