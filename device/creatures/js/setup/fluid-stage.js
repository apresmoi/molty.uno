// The setup console's Molty screen: the real fluid engine (js/fluid) + the real cute-kit
// creatures (js/cute), in their own colours (pastel palette). One persistent stage that
// the page moves between layouts; plus fluid-rendered thumbnails for the picker.

import { FluidSim } from '../fluid/sim.js';
import { BlobTarget, TextTarget, CreatureTarget } from '../fluid/targets.js';
import { GlyphRenderer } from '../fluid/render.js';
import { GlyphRamps } from '../fluid/synth.js';
import { tokenize } from '../fluid/reader.js';
import { CUTE_CATALOG } from '../cute/index.js';

export { CUTE_CATALOG };
const W = 52, H = 32, STEP = 1 / 60;
const RATES = { blob: [1.6, 2.0], creature: [2.6, 3.4], text: [7, 2.4] };
const DRAW = { ramp: GlyphRamps.Fade, palette: 'pastel', braille: 'all', edges: 1 };
// after a Molty emerges it dances (happy = hopping), rests, looks around, dances again
const DANCE = [['idle', 1.6], ['happy', 3.2], ['idle', 1.8], ['surprised', 1.0], ['happy', 3.6], ['listening', 1.8]];

export class FluidStage {
  constructor(px = 260) {
    this.el = document.createElement('canvas');
    this.el.className = 'molty-screen';
    this.renderer = new GlyphRenderer(this.el); this.renderer.resize(1);
    this.setSize(px);
    this.sim = new FluidSim(W, H, 412 / W, 412 / H);
    this.blobT = new BlobTarget();
    this.textT = new TextTarget(this.sim); this.textT.mode = 'plain';
    this.creatureT = new CreatureTarget(this.sim);
    this.id = null; this.cur = { name: 'blob', target: this.blobT };
    this.t = 0; this.stateT = 0; this.acc = 0; this.breathPh = 0; this.overlay = []; this.meltT = 9;
    this.danceI = 0; this.danceT = 0; this.mood = 'idle'; this.hopT = 0;
    this.el.addEventListener('pointerdown', (e) => {
      const r = this.el.getBoundingClientRect();
      this.ripple(((e.clientX - r.left) / r.width) * 2 - 1, ((e.clientY - r.top) / r.height) * 2 - 1, 1.6, 0.09);
    });
    this.last = performance.now();
    requestAnimationFrame((n) => this.frame(n));
  }
  setSize(px) { this.el.style.width = this.el.style.height = `${px}px`; }

  /** Morph into a Molty (by cute-kit id); it emerges from the fluid, then dances. */
  show(id) {
    const entry = CUTE_CATALOG.find((c) => c.id === id); if (!entry) return;
    if (id !== this.id) { this.creatureT.setCreature(entry); this.id = id; }
    this.danceI = 0; this.danceT = 0; this.setMood(DANCE[0][0]);
    this.set('creature');
  }
  blob() { this.id = null; this.set('blob'); }
  /** Condense a short word (the pairing code) out of the fluid. */
  word(w) {
    const toks = tokenize(w); this.textT.setSentence(toks); this.textT.setWord(toks[0]);
    this.set('text');
  }
  setMood(m) { this.mood = m; this.creatureT.setMood(m); }

  ripple(x, y, s, r) { this.sim.impulse(x, y, s, r); }
  set(name) {
    const sim = this.sim, from = this.cur.name;
    if (from === name && name !== 'text') return;
    const target = name === 'creature' ? this.creatureT : name === 'text' ? this.textT : this.blobT;
    let sx = 0, sy = 0, s = 0; // break the old shape up from its centroid
    for (let k = 0; k < sim.N; k++) { const w = sim.ink[k]; sx += sim.X[k] * w; sy += sim.Y[k] * w; s += w; }
    sim.swirlKick = Math.max(sim.swirlKick, 3);
    this.ripple(s > 1 ? sx / s : 0, s > 1 ? sy / s : 0, name === 'creature' ? 1.1 : 0.6, 0.2);
    this.cur = { name, target };
    if (name !== 'text') sim.BGM.fill(1);
    if (from === 'text' && name !== 'text') this.meltT = 0;
    sim.edgeW = name === 'creature' ? 1 : 0;
    [sim.p.relaxUp, sim.p.relaxDown] = RATES[name];
    this.stateT = 0;
  }

  frame(now) {
    requestAnimationFrame((n) => this.frame(n));
    if (!this.el.isConnected) { this.last = now; return; }
    const sim = this.sim, rdt = Math.min(0.1, (now - this.last) / 1000); this.last = now;
    this.t += rdt; this.stateT += rdt;
    this.breathPh += rdt / 4; const breath = 0.5 - 0.5 * Math.cos(this.breathPh * Math.PI * 2), swell = 1 + 0.05 * (breath * 2 - 1);
    const name = this.cur.name;
    if (name === 'creature') {
      this.danceT += rdt;
      if (this.danceT > DANCE[this.danceI][1]) { this.danceI = (this.danceI + 1) % DANCE.length; this.danceT = 0; this.setMood(DANCE[this.danceI][0]); }
      // every landing of the hop sends a little wave out from under its feet
      if (this.mood === 'happy') { this.hopT += rdt; if (this.hopT > 0.476) { this.hopT = 0; this.ripple(0, 0.62, 0.55, 0.1); } }
      this.creatureT.update(rdt);
    }
    const q = name === 'text' ? clamp01((this.stateT - 0.2) / 0.4) : clamp01((this.stateT - 0.7) / 1.0);
    sim.p.hold = q * q * (3 - 2 * q); sim.p.tintRate = 3 + 9 * sim.p.hold;
    this.cur.target.fill(sim, this.t, swell);
    sim.applyBackground(this.t, breath);
    this.acc += rdt; let n = 0;
    while (this.acc >= STEP && n < 4) { sim.step(STEP); this.acc -= STEP; n++; }
    if (n === 4) this.acc = 0;
    sim.computeView(null);
    let melt = 0;
    if (name === 'text') this.overlay = this.textT.letters;
    else { this.meltT += rdt; melt = this.meltT; if (this.meltT > 0.8) this.overlay = []; }
    this.renderer.draw(sim, { ...DRAW, time: this.t, shimmer: 0.12, letters: this.overlay, melt });
  }
}
const clamp01 = (v) => (v < 0 ? 0 : v > 1 ? 1 : v);

/** Still fluid-rendered portraits of every Molty, one per idle slice (calls back per id). */
export function renderThumbs(onThumb, px = 150) {
  const sim = new FluidSim(W, H, 412 / W, 412 / H);
  const ct = new CreatureTarget(sim), cv = document.createElement('canvas'), r = new GlyphRenderer(cv);
  r.resize(1);
  const out = document.createElement('canvas'); out.width = out.height = px * 2;
  const og = out.getContext('2d');
  let i = 0;
  const next = () => {
    if (i >= CUTE_CATALOG.length) { ct.dispose(); return; }
    const e = CUTE_CATALOG[i++];
    ct.setCreature(e); ct.setMood('happy');
    for (let f = 0; f < 3; f++) ct.update(1 / 30);
    sim.T.fill(0); ct.fill(sim, 0, 1);
    sim.ink.set(sim.T); sim.r.set(sim.TR); sim.g.set(sim.TG); sim.b.set(sim.TB); sim.acc.fill(0);
    sim.p.hold = 1; sim.edgeW = 1;
    sim.computeView(null);
    r.draw(sim, { ...DRAW, time: 0, shimmer: 0, letters: [], melt: 0 });
    og.clearRect(0, 0, out.width, out.height); og.drawImage(cv, 0, 0, out.width, out.height);
    onThumb(e.id, out.toDataURL('image/png'));
    setTimeout(next, 0);
  };
  setTimeout(next, 50);
}
