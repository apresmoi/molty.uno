// Molty as a fluid of characters — wiring: targets, sequencer (show reel), bus, UI.

import { bus } from './bus.js';
import { FluidSim } from './sim.js';
import { BlobTarget, EffectTarget, TextTarget, CreatureTarget, EFFECTS } from './targets.js';
import { GlyphRenderer } from './render.js';
import { GlyphRamps, patchValue } from './synth.js';
import { tokenize, wordDelay } from './reader.js';
import { CUTE_CATALOG } from '../cute/index.js';
import { MOODS } from '../mood.js';

const $ = (id) => document.getElementById(id);
const GRIDS = { '44x27': [44, 27], '52x32': [52, 32], '60x37': [60, 37] };
const S = {
  grid: '52x32', ramp: 'Fade', palette: 'pastel', textMode: 'plain', wpm: 300, speed: 1, mic: 0, zoom: 1,
  autoplay: true, edges: true, braille: 'all', creature: 'cute-duck', mood: 'idle',
  sentence: 'hello, i am molty. i breathe in ascii and i speak in ascii. touch the water.',
};

// ---------------------------------------------------------------- show reel
const REEL = [
  { s: 'blob', dur: 3.2 },
  { s: 'creature', mood: 'idle', dur: 2.8 },
  { mood: 'happy', dur: 2.6 },
  { s: 'effect:ripple', dur: 2.8 },
  { s: 'text' },
  { s: 'creature', mood: 'speaking', dur: 3.4 },
  { mood: 'thinking', dur: 3.0 },
  { s: 'effect:vortex', dur: 2.8 },
  { s: 'blob', dur: 2.4 },
  { s: 'creature', mood: 'listening', dur: 2.6 },
  { mood: 'sleepy', dur: 3.0 },
  { s: 'effect:wave', dur: 2.8 },
];

let sim, renderer, blob, creature, text, effects, cur;
let tNow = 0, acc = 0, breathPh = 0, breath = 0, stateT = 0, settled = false;
let reelIdx = -1, reelT = 0, tokens = [], wordIdx = 0, wordUntil = 0, overlay = [];
const audio = { level: 0, pitch: 0, beat: false, at: -1 };
const stats = { fps: 0, simMs: 0, drawMs: 0, creatureMs: 0, glyphs: 0, frames: 0 };

function build() {
  const [W, H] = GRIDS[S.grid];
  sim = new FluidSim(W, H, 412 / W, 412 / H);
  sim.p.speedMul = S.speed;
  blob = new BlobTarget();
  text = new TextTarget(sim); text.mode = S.textMode;
  creature?.dispose();
  creature = new CreatureTarget(sim);
  const entry = CUTE_CATALOG.find((c) => c.id === S.creature) || CUTE_CATALOG[0];
  creature.setCreature(entry); creature.setMood(S.mood);
  effects = Object.fromEntries(Object.keys(EFFECTS).map((k) => [k, new EffectTarget(k)]));
  cur = { name: 'blob', target: blob, fx: null };
  $('gridinfo').textContent = `${W}×${H} cells · ${sim.inside} inside the disc · cell ${(412 / W).toFixed(1)}×${(412 / H).toFixed(1)} px`;
}

function centroid() {
  let sx = 0, sy = 0, s = 0;
  for (let k = 0; k < sim.N; k++) { const w = sim.ink[k]; sx += sim.X[k] * w; sy += sim.Y[k] * w; s += w; }
  return s > 1 ? [sx / s, sy / s] : [0, 0];
}

function ripple(x, y, strength, radius, emit = true) {
  sim.impulse(x, y, strength, radius);
  if (emit) bus.emit('ripple', { x: (x + 1) / 2, y: (y + 1) / 2, strength: Math.min(1, strength / 2) });
}

function setMood(m) {
  if (!MOODS.includes(m)) return;
  S.mood = m; creature.setMood(m);
  for (const b of $('moods').children) b.classList.toggle('on', b.dataset.m === m);
  bus.emit('mood', { name: m });
}

function setState(name, opt = {}) {
  const from = cur.name;
  if (cur.fx?.exit) cur.fx.exit(sim);
  if (opt.mood) setMood(opt.mood);
  let target, fx = null;
  if (name === 'blob') target = blob;
  else if (name === 'creature') target = creature;
  else if (name === 'text') {
    target = text; tokens = tokenize(opt.text || S.sentence); if (!tokens.length) tokens = tokenize('…');
    text.setSentence(tokens); wordIdx = 0; text.setWord(tokens[0]);
    wordUntil = tNow + 0.55 + wordDelay(tokens[0], S.wpm) / 1000; // lead-in: let the ink gather first
    bus.emit('word', { word: tokens[0].word, orp: tokens[0].orp, ms: Math.round((wordUntil - tNow) * 1000) });
  } else if (name.startsWith('effect:')) {
    const e = effects[name.slice(7)]; target = e; fx = e.fx; e.state = {};
  }
  if (!target) return;
  // the transition itself: the old shape breaks up in eddies + a ring from its centre;
  // the demand field (T − ink) then pulls the ink into the new target
  const [cx, cy] = centroid();
  sim.swirlKick = Math.max(sim.swirlKick, 3);
  if (name !== 'text') ripple(cx, cy, 0.7, 0.2);
  else ripple(0, 0, 0.5, 0.25);
  cur = { name, target, fx };
  if (name !== 'text') sim.BGM.fill(1);
  sim.edgeW = name === 'creature' ? 1 : 0;
  // how hard each kind of target pulls (text must condense within one word's time)
  const [up, down] = RATES[name.startsWith('effect:') ? 'effect' : name];
  sim.p.relaxUp = up; sim.p.relaxDown = down;
  if (from === 'text' && name !== 'text') meltT = 0;
  fx?.enter?.(sim);
  stateT = 0; settled = false;
  bus.emit('state', { from, to: name, ms: Math.round((name === 'text' ? 550 : 1500) / S.speed) });
  for (const b of document.querySelectorAll('[data-state]')) b.classList.toggle('on', b.dataset.state === name);
}

function nextReel() {
  reelIdx = (reelIdx + 1) % REEL.length; const st = REEL[reelIdx]; reelT = 0;
  if (st.s) setState(st.s, { mood: st.mood }); else if (st.mood) setMood(st.mood);
  $('reelstep').textContent = `${reelIdx + 1}/${REEL.length} · ${st.s || cur.name}${st.mood ? ' · ' + st.mood : ''}`;
}

// ---------------------------------------------------------------- per frame
const STEP = 1 / 60;
const RATES = { blob: [1.6, 2.0], creature: [2.6, 3.4], text: [7, 2.4], effect: [1.8, 2.2] };
let meltT = Infinity;
let last = performance.now();

function frame(now) {
  const rdt = Math.min(0.1, (now - last) / 1000); last = now;
  stats.fps = stats.fps ? stats.fps * 0.93 + (1 / Math.max(rdt, 1e-3)) * 0.07 : 60; stats.frames++;
  tNow += rdt; stateT += rdt; reelT += rdt;

  // breathing: a slow swell of the whole field (slower and deeper when sleepy)
  const period = S.mood === 'sleepy' ? 6 : 4;
  breathPh += rdt / period; breath = 0.5 - 0.5 * Math.cos(breathPh * Math.PI * 2);
  const swell = 1 + (S.mood === 'sleepy' ? 0.07 : 0.05) * (breath * 2 - 1);

  // sequencer
  if (S.autoplay) {
    const st = REEL[reelIdx];
    if (reelIdx < 0) nextReel();
    else if (cur.name === 'text' ? wordIdx >= tokens.length : reelT >= (st.dur || 3)) nextReel();
  }

  // speed reader
  if (cur.name === 'text' && wordIdx < tokens.length && tNow >= wordUntil) {
    wordIdx++;
    if (wordIdx < tokens.length) {
      const tok = tokens[wordIdx], ms = wordDelay(tok, S.wpm);
      text.setWord(tok); wordUntil = tNow + ms / 1000;
      bus.emit('word', { word: tok.word, orp: tok.orp, ms });
      // each word lands with a small ripple at the ORP letter — bigger with the speech level
      const lv = audioLive() ? audio.level : 0.3;
      ripple(0, 0, 0.45 + 0.6 * lv, 0.12); // a soft ring leaves the word on every change
    } else if (!S.autoplay) wordUntil = Infinity;
  }
  if (cur.name === 'text' && S.textMode === 'plain') overlay = text.letters;

  { const q = cur.name === 'text' ? Math.min(1, Math.max(0, (stateT * S.speed - 0.2) / 0.4)) : Math.min(1, Math.max(0, (stateT * S.speed - 0.7) / 1.0)); sim.p.hold = q * q * (3 - 2 * q); sim.p.tintRate = 3 + 9 * sim.p.hold; }
  const t0 = performance.now();
  // creature: re-render the living mesh (mood animation, blinks) every frame it's the target
  if (cur.name === 'creature' || stateT < 0.05) { creature.update(rdt); stats.creatureMs = creature.ms; }
  cur.target.fill(sim, tNow, swell);
  sim.applyBackground(tNow, breath); // the always-on dim fluid under every state
  cur.fx?.tick?.(sim, tNow, rdt, cur.target.state);

  // sound ↔ surface: audio level drives a point oscillator (rings at the audio rhythm),
  // pitch sets its frequency; thinking / mic add the motor-like shimmer
  const live = audioLive();
  const speechEnv = creature.engine.envValue || 0;
  const level = live ? audio.level : cur.name === 'creature' && S.mood === 'speaking' ? speechEnv : 0;
  const thinking = cur.name === 'creature' && S.mood === 'thinking';
  const speaking = cur.name === 'creature' && S.mood === 'speaking';
  // where the sound-rings come from and how hard: under the word while reading (so they
  // never punch through the letters), from the mouth while speaking, the centre otherwise
  if (cur.name === 'text') { sim.osc.x = 0; sim.osc.y = 0.5; sim.osc.amp = 0.8 * level; }
  else if (speaking) { sim.osc.x = 0; sim.osc.y = 0.12; sim.osc.amp = 1.2 * level; }
  else { sim.osc.x = 0; sim.osc.y = 0; sim.osc.amp = (thinking ? 0.25 : 0.5) * level; }
  sim.osc.freq = 2.5 + 6 * (live ? audio.pitch : 0.4);
  // thinking = the motor: a fine standing shimmer, pulsing with the hum's level
  sim.vibe.amp = (thinking ? 0.9 : 0) + (live && thinking ? audio.level * 1.2 : 0) + S.mic * 1.4;
  sim.vibe.freq = 7 + 9 * (live ? audio.pitch : 0.3);
  if (S.mic > 0 && Math.random() < S.mic * 0.35) ripple(Math.random() * 1.6 - 0.8, Math.random() * 1.6 - 0.8, 0.4 + S.mic, 0.06, false);

  // fixed-step physics
  acc += rdt; let n = 0;
  while (acc >= STEP && n < 4) { sim.step(STEP); acc -= STEP; n++; }
  if (n === 4) acc = 0;

  sim.computeView(null);
  const t1 = performance.now();
  // after the text, the condensed letters melt: they decode back into ramp glyphs and drain
  let melt = 0;
  if (cur.name !== 'text') { meltT += rdt; melt = meltT; if (meltT > 0.8) overlay = []; }
  stats.glyphs = renderer.draw(sim, { ramp: GlyphRamps[S.ramp], palette: S.palette, braille: S.braille, time: tNow, shimmer: 0.12 + (cur.name === 'creature' && S.mood === 'thinking' ? 0.35 : 0) + S.mic * 0.4, edges: S.edges ? 1 : 0, letters: S.textMode === 'plain' ? overlay : [], melt });
  const t2 = performance.now();
  stats.simMs = stats.simMs * 0.9 + (t1 - t0 - (cur.name === 'creature' ? creature.ms : 0)) * 0.1;
  stats.drawMs = stats.drawMs * 0.9 + (t2 - t1) * 0.1;

  if (!settled && stateT > 0.5 && sim.err < 0.06) { settled = true; bus.emit('settle', { state: cur.name }); }
  bus.emit('frame', { t: tNow, dt: rdt, energy: sim.energy, breath: breathPh % 1, state: cur.name, mood: S.mood });

  if ((stats.frames & 15) === 0) {
    $('stats').textContent = `${stats.fps.toFixed(0)} fps · sim ${stats.simMs.toFixed(2)} ms · draw ${stats.drawMs.toFixed(2)} ms · creature ${stats.creatureMs.toFixed(2)} ms · ${stats.glyphs} glyphs · err ${sim.err.toFixed(3)} · energy ${sim.energy.toFixed(2)}`;
    $('meter').style.width = `${(sim.energy * 100).toFixed(0)}%`;
  }
  requestAnimationFrame(frame);
}
const audioLive = () => audio.at >= 0 && tNow - audio.at < 0.3;

// ---------------------------------------------------------------- bus in
bus.on('audio', (a) => { audio.level = +a.level || 0; audio.pitch = +a.pitch || 0; if (a.beat) audio.beat = true; audio.at = tNow; });
bus.on('pulse', (p) => {
  let x = p.x != null ? p.x * 2 - 1 : sim.osc.x, y = p.y != null ? p.y * 2 - 1 : sim.osc.y, k = 1;
  if (cur.name === 'text' && Math.abs(y) < 0.3) { y += 0.5; k = 0.6; } // keep the word legible: pulse under it
  ripple(x, y, (0.5 + 1.5 * (p.strength ?? 0.5)) * k, 0.1); // sound ignores ripple echoes within 30 ms of its own pulse
});

// ---------------------------------------------------------------- UI
function ui() {
  const sel = $('creature');
  for (const c of CUTE_CATALOG) sel.add(new Option(c.name, c.id, false, c.id === S.creature));
  sel.onchange = () => { S.creature = sel.value; creature.setCreature(CUTE_CATALOG.find((c) => c.id === sel.value)); creature.setMood(S.mood); if (cur.name !== 'creature') setState('creature'); };
  for (const m of MOODS) { const b = document.createElement('button'); b.textContent = m; b.dataset.m = m; b.onclick = () => { setMood(m); if (cur.name !== 'creature') setState('creature'); }; $('moods').append(b); }
  for (const b of document.querySelectorAll('[data-state]')) b.onclick = () => { S.autoplay = false; $('autoplay').checked = false; setState(b.dataset.state); };
  $('autoplay').onchange = (e) => { S.autoplay = e.target.checked; if (S.autoplay) { reelIdx = -1; } };
  const rs = $('ramp'); for (const k of Object.keys(GlyphRamps)) rs.add(new Option(`${k}  “${GlyphRamps[k]}”`, k, false, k === S.ramp));
  rs.onchange = () => { S.ramp = rs.value; };
  $('edges').checked = S.edges; $('edges').onchange = (e) => { S.edges = e.target.checked; };
  $('braille').value = S.braille; $('braille').onchange = (e) => { S.braille = e.target.value; };
  $('palette').value = S.palette; $('palette').onchange = (e) => { S.palette = e.target.value; };
  $('textmode').value = S.textMode; $('textmode').onchange = (e) => { S.textMode = text.mode = e.target.value; if (cur.name === 'text') { text.setSentence(tokens); text.setWord(tokens[Math.min(wordIdx, tokens.length - 1)]); } };
  const rng = (id, key, fmt, apply) => { const el = $(id), out = $(id + 'v'); const f = () => { S[key] = +el.value; out.textContent = fmt(S[key]); apply?.(); }; el.value = S[key]; el.oninput = f; f(); };
  rng('wpm', 'wpm', (v) => `${v} wpm`);
  rng('speed', 'speed', (v) => `${v.toFixed(2)}×`, () => { if (sim) sim.p.speedMul = S.speed; });
  rng('mic', 'mic', (v) => v.toFixed(2));
  $('sentence').value = S.sentence; $('sentence').onchange = (e) => { S.sentence = e.target.value; };
  $('zoom').onchange = (e) => { S.zoom = e.target.checked ? 2 : 1; renderer.resize(S.zoom); };
  $('grid').value = S.grid; $('grid').onchange = (e) => { S.grid = e.target.value; const keep = cur.name; build(); setState(keep); };

  // touch: tap = ripple, drag = ripple trail + a push of the flow along the drag
  const cv = $('stage'); let down = null;
  const pos = (e) => { const r = cv.getBoundingClientRect(); return [((e.clientX - r.left) / r.width) * 2 - 1, ((e.clientY - r.top) / r.height) * 2 - 1]; };
  cv.addEventListener('pointerdown', (e) => { cv.setPointerCapture(e.pointerId); down = pos(e); ripple(down[0], down[1], 1.6, 0.09); });
  cv.addEventListener('pointermove', (e) => {
    if (!down) return; const p = pos(e), dx = p[0] - down[0], dy = p[1] - down[1];
    if (Math.hypot(dx, dy) < 0.03) return;
    sim.push(p[0], p[1], dx * sim.W * 6, dy * sim.W * 6, 0.16); ripple(p[0], p[1], 0.5, 0.06); down = p;
  });
  const up = () => { down = null; }; cv.addEventListener('pointerup', up); cv.addEventListener('pointercancel', up);
}

renderer = new GlyphRenderer($('stage'));
renderer.resize(1);
ui();
build();
setMood(S.mood);
requestAnimationFrame(frame);

// test / console hooks
window.__fluid = {
  S, stats, bus, REEL, setState, setMood,
  get sim() { return sim; }, get creatureTarget() { return creature; }, get state() { return cur.name; }, get reel() { return reelIdx; },
  autoplay(on) { S.autoplay = on; $('autoplay').checked = on; if (on) reelIdx = -1; },
};
