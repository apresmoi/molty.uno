import { CATALOG, byId } from './catalog.js';
import { MOODS } from './mood.js';
import { Stage, BUDGET } from './stage.js';
import { PALETTE, PATTERNS, HEAD_ITEMS, NECK_ITEMS } from './proc.js';

const $ = (s, r = document) => r.querySelector(s);
const h = (tag, attrs = {}, ...kids) => { const e = document.createElement(tag); for (const [k, v] of Object.entries(attrs)) { if (k.startsWith('on')) e.addEventListener(k.slice(2), v); else if (k === 'class') e.className = v; else e.setAttribute(k, v); } for (const c of kids) e.append(c); return e; };

const state = {
  mood: 'idle', active: 0, compare: false, autopilot: false, emotes: true, sound: false,
  clip: { loop: true, fade: 0.4, speed: 1, paused: false },
  device: { on: false, decimate: true, target: BUDGET.targetTris, method: 'lattice', glyph: true, grid: 72, cells: 'rect' },
};
const measured = {}; // id → tris measured at load

// ---------- stages ----------
const stagesEl = $('#stages');
const stages = [new Stage(stagesEl, 'A')];
const activeStage = () => stages[state.active] || stages[0];
function wireStage(s, i) {
  s.el.addEventListener('pointerdown', () => setActive(i));
  s.onPoke(poke);
  s.vp.addEventListener('pointermove', touch);
  s.engine.env.onSyllable = (amp) => { if (state.sound && i === 0 && s.engine.mood === 'speaking') blip(amp); };
}
wireStage(stages[0], 0);
function setActive(i) {
  if (state.active === i) return;
  state.active = i; stages.forEach((s, j) => s.el.classList.toggle('active', j === i));
  renderCards(); renderClips(); renderCustomize();
}

async function load(stage, id) {
  const entry = byId[id];
  const c = await stage.setCreature(entry);
  if (!c) return;
  Object.assign(stage.device, state.device); stage.applyDevice();
  stage.engine.set(state.mood);
  measured[id] = c.sourceTris ?? c.tris();
  const def = c.defaultClip();
  stage.clipName = def;
  if (def) c.play(def, { loop: true, fade: 0, speed: state.clip.speed });
  renderCards(); renderClips(); renderCustomize();
}

// ---------- creature cards ----------
const GROUPS = [
  ['Route 3 — Cute kit', (c) => c.kind === 'cute'],
  ['Route 2 — procedural (built in code)', (c) => c.kind === 'proc'],
  ['Quaternius — CC0, skinned', (c) => c.source.startsWith('Quaternius')],
  ['Kenney Cube Pets — CC0, rigid nodes', (c) => c.source.startsWith('Kenney')],
];
function renderCards() {
  const host = $('#cards'); host.innerHTML = '';
  const cur = activeStage().entry?.id;
  const other = stages.length > 1 ? stages[1 - state.active].entry?.id : null;
  for (const [title, f] of GROUPS) {
    host.append(h('div', { class: 'group' }, title));
    for (const c of CATALOG.filter(f)) {
      const tris = measured[c.id] ?? c.tris;
      const over = tris && tris > BUDGET.targetTris;
      const card = h('button', { class: `card${c.id === cur ? ' sel' : ''}${c.id === other ? ' other' : ''}`, onclick: () => load(activeStage(), c.id) },
        h('div', { class: 'row1' }, h('span', { class: 'cname' }, c.name), h('span', { class: 'arch' }, c.archetype)),
        h('div', { class: 'row2' },
          h('span', { class: `lic ${c.license === 'CC0' ? 'cc0' : ''}` }, c.license),
          h('span', { class: `tri${over ? ' over' : ''}` }, tris ? `${tris.toLocaleString()} tris` : '< 600 tris'),
          h('span', { class: 'src' }, c.source)),
        c.roster ? h('div', { class: 'roster' }, c.roster) : '');
      host.append(card);
    }
  }
}

// ---------- moods ----------
function setMood(m, fromUser = true) {
  state.mood = m; if (fromUser) baseMood = m;
  stages.forEach((s) => s.engine.set(m));
  document.querySelectorAll('#moods button').forEach((b) => b.classList.toggle('on', b.dataset.mood === m));
}
let baseMood = 'idle';
$('#moods').append(...MOODS.map((m) => h('button', { 'data-mood': m, onclick: () => { touch(); setMood(m); } }, m)));

// pokes + quiet initiative (rule 12)
let lastTouch = performance.now(), pokes = [], reactUntil = 0, reactQueue = [];
function touch() { lastTouch = performance.now(); if (state.autopilot && state.mood === 'sleepy' && reactQueue.length === 0) setMood('idle', false); }
function poke() {
  touch();
  const now = performance.now(); pokes = pokes.filter((t) => now - t < 6000); pokes.push(now);
  if (pokes.length > 3) { reactQueue = [['sleepy', 4000]]; pokes = []; $('#autonote').textContent = 'too many pokes — tired'; }
  else reactQueue = [['surprised', 1100], ['happy', 2200]];
  reactUntil = 0;
}
function tickInitiative(now) {
  if (reactQueue.length && now >= reactUntil) { const [m, d] = reactQueue.shift(); setMood(m, false); reactUntil = now + d; return; }
  if (!reactQueue.length && reactUntil && now >= reactUntil) { reactUntil = 0; setMood(baseMood, false); $('#autonote').textContent = ''; }
  if (!state.autopilot || reactUntil) return;
  const idle = (now - lastTouch) / 1000;
  const note = $('#autonote');
  if (idle > 45) { if (state.mood !== 'sleepy') { setMood('sleepy', false); note.textContent = 'ignored 45 s → dozing'; } }
  else if (idle > 20 && idle % 14 < 4) { if (state.mood !== 'thinking') { setMood('thinking', false); note.textContent = 'ignored → self-amusing'; } }
  else if (state.mood !== baseMood && state.mood !== 'sleepy') { setMood(baseMood, false); note.textContent = ''; }
}

// ---------- toggles ----------
const bindCheck = (sel, fn) => { const e = $(sel); e.addEventListener('change', () => fn(e.checked)); return e; };
bindCheck('#autopilot', (v) => { state.autopilot = v; touch(); $('#autonote').textContent = v ? 'autopilot: ignore it for 20 s / 45 s, or poke it' : ''; });
bindCheck('#gaze', (v) => stages.forEach((s) => (s.gaze = v)));
bindCheck('#emotes', (v) => (state.emotes = v));
bindCheck('#sound', (v) => { state.sound = v; if (v) audio(); });
bindCheck('#compare', async (v) => {
  state.compare = v; stagesEl.classList.toggle('two', v);
  if (v && stages.length === 1) {
    const s = new Stage(stagesEl, 'B'); stages.push(s); wireStage(s, 1); s.gaze = $('#gaze').checked;
    const a = stages[0].entry?.id; await load(s, a === 'cute-mochi2' ? 'mochi' : 'cute-mochi2');
  } else if (!v && stages.length === 2) { stages.pop().dispose(); state.active = 0; }
  stages.forEach((s, j) => s.el.classList.toggle('active', v && j === state.active));
  renderCards(); renderClips(); renderCustomize();
});

// ---------- clips ----------
function renderClips() {
  const s = activeStage(), c = s.creature, host = $('#clips'); host.innerHTML = '';
  if (!c) return;
  if (!c.clips.length) { host.append(h('p', { class: 'muted' }, c.entry?.kind === 'proc' || c.entry?.kind === 'cute' ? 'Procedural creature — no authored clips; everything you see is the mood layer.' : 'No embedded clips.')); return; }
  const note = c.rawClipCount && c.rawClipCount !== c.clips.length ? ` (${c.rawClipCount} in file, duplicates merged)` : '';
  host.append(h('p', { class: 'muted' }, `${c.clips.length} embedded clips${note}`));
  host.append(h('button', { class: `clip${!s.clipName ? ' on' : ''}`, onclick: () => { s.clipName = null; c.play(null, { fade: state.clip.fade }); renderClips(); } }, '— none (rest + mood)'));
  for (const { name, clip } of c.clips) {
    host.append(h('button', { class: `clip${s.clipName === name ? ' on' : ''}`, onclick: () => { s.clipName = name; c.play(name, state.clip); c.setPaused(state.clip.paused); renderClips(); } },
      name, h('span', { class: 'dur' }, `${clip.duration.toFixed(1)} s`)));
  }
}
bindCheck('#loop', (v) => { state.clip.loop = v; const s = activeStage(); if (s.clipName) s.creature.play(s.clipName, state.clip); });
$('#fade').addEventListener('input', (e) => { state.clip.fade = +e.target.value; $('#fadev').textContent = `${(+e.target.value).toFixed(2)} s`; });
$('#speed').addEventListener('input', (e) => { state.clip.speed = +e.target.value; $('#speedv').textContent = `${(+e.target.value).toFixed(2)}×`; stages.forEach((s) => s.creature?.setSpeed(state.clip.speed)); });
$('#pause').addEventListener('click', (e) => { state.clip.paused = !state.clip.paused; e.target.textContent = state.clip.paused ? 'play' : 'pause'; stages.forEach((s) => s.creature?.setPaused(state.clip.paused)); });

// ---------- device ----------
function pushDevice() { stages.forEach((s) => { Object.assign(s.device, state.device); s.applyDevice(); }); $('#devopts').classList.toggle('off', !state.device.on); }
bindCheck('#dev-on', (v) => { state.device.on = v; pushDevice(); });
bindCheck('#dev-glyph', (v) => { state.device.glyph = v; pushDevice(); });
$('#dev-grid').addEventListener('change', (e) => { state.device.grid = +e.target.value; pushDevice(); });
$('#dev-cells').addEventListener('change', (e) => { state.device.cells = e.target.value; pushDevice(); });

// ---------- customisation (procedural only) ----------
function renderCustomize() {
  const s = activeStage(), c = s.creature, host = $('#custom'); host.innerHTML = '';
  const custom = c && (c.entry?.kind === 'proc' || c.entry?.kind === 'cute');
  $('#custom-sec').style.display = custom ? '' : 'none';
  if (!custom) return;
  const patterns = c.patterns ?? PATTERNS, headItems = c.headItems ?? HEAD_ITEMS, neckItems = c.neckItems ?? NECK_ITEMS;
  const sw = (key) => h('div', { class: 'swatches' }, ...PALETTE.map((p) => h('button', { class: 'sw', style: `background:${p}`, title: p, onclick: () => { c.setSkin({ [key]: p }); renderCustomize(); } })));
  const color = (key, label) => { const i = h('input', { type: 'color', value: c.skin[key] }); i.addEventListener('input', () => c.setSkin({ [key]: i.value })); return h('label', { class: 'line' }, label, i); };
  const sel = (opts, val, fn) => { const e = h('select', {}, ...opts.map((o) => { const op = h('option', { value: o }, o); if (o === val) op.selected = true; return op; })); e.addEventListener('change', () => fn(e.value)); return e; };
  const blush = h('input', { type: 'checkbox' }); blush.checked = !!c.skin.blush; blush.addEventListener('change', () => c.setSkin({ blush: blush.checked }));
  host.append(
    color('body', 'body colour'), sw('body'),
    color('accent', 'accent colour'), sw('accent'),
    patterns.length ? h('label', { class: 'line' }, 'pattern', sel(patterns, c.skin.pattern, (v) => c.setSkin({ pattern: v }))) : '',
    h('label', { class: 'line' }, 'blush', blush),
    h('div', { class: 'slots' },
      h('label', { class: 'line' }, 'head slot', sel(headItems, c.items.head, (v) => { c.setItems({ head: v }); s.applyDevice(); })),
      h('label', { class: 'line' }, 'neck slot', sel(neckItems, c.items.neck, (v) => { c.setItems({ neck: v }); s.applyDevice(); }))),
    h('p', { class: 'muted' }, 'Items are rigid parts on sockets that ride the same deform as the body (≤ 60 tris each); head items lag the head with a spring.'));
}

// ---------- audio blips (Animalese-ish, off by default) ----------
let ac = null;
function audio() { if (!ac) ac = new (window.AudioContext || window.webkitAudioContext)(); return ac; }
function blip(amp) {
  const a = audio(); const o = a.createOscillator(), g = a.createGain();
  o.type = 'square'; o.frequency.value = 300 + Math.random() * 260;
  g.gain.setValueAtTime(0.0001, a.currentTime); g.gain.exponentialRampToValueAtTime(0.05 * amp, a.currentTime + 0.01); g.gain.exponentialRampToValueAtTime(0.0001, a.currentTime + 0.09);
  o.connect(g).connect(a.destination); o.start(); o.stop(a.currentTime + 0.1);
}

// ---------- loop ----------
let last = performance.now(), fpsAcc = 0, fpsN = 0, lastStat = 0;
function loop(now) {
  const dt = (now - last) / 1000; last = now;
  tickInitiative(now);
  for (const s of stages) { s.update(dt, state.emotes); s.render(); }
  fpsAcc += dt; fpsN++;
  if (now - lastStat > 250) {
    lastStat = now;
    for (const s of stages) {
      const src = s.creature?.sourceTris;
      s.el.querySelector('.stris').textContent = s.creature ? `${s.stats.tris.toLocaleString()} tris${src && src !== s.stats.tris ? ` (from ${src.toLocaleString()})` : ''}` : '';
      const bud = s.el.querySelector('.sbudget');
      if (state.device.on) {
        const b = s.budgetText();
        bud.innerHTML = `device est. <b class="${b.ok ? 'ok' : 'bad'}">${b.total.toFixed(1)} ms</b> ≈ ${b.fps.toFixed(0)} fps · flush 10 + geom ${b.geo.toFixed(1)} + ${s.stats.cells} cells ${b.cells.toFixed(1)}${b.over ? ' · <b class="bad">over 600 tris</b>' : ''}`;
      } else bud.textContent = '';
    }
    const env = stages[0].pose?.env || 0;
    $('#envbar').style.width = `${Math.round(env * 100)}%`;
    const p = stages[0].pose;
    if (p) $('#posev').textContent = `eye ${p.eye} · lid ${p.lid.toFixed(2)} · squash ${(p.squash * 100).toFixed(1)}%`;
    $('#fps').textContent = `${(fpsN / fpsAcc).toFixed(0)} fps (browser)`; fpsAcc = 0; fpsN = 0;
  }
  requestAnimationFrame(loop);
}

setMood('idle');
renderCards();
await load(stages[0], 'cute-duck');
requestAnimationFrame(loop);
window.__viewer = { stages, state, load, setMood, CATALOG }; // for the smoke test
