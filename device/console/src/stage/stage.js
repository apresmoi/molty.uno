// The round Molty screen for the setup flow while no Molty is on it: the viewer's fluid
// engine (sim, blob and text targets, glyph renderer, pastel palette), imported via
// @viewer, never copied. The Moltys themselves are the device's creature packages, so a
// chosen one is shown from the device (src/ui/screen.ts), not here.
import { FluidSim } from '@viewer/fluid/sim.js';
import { BlobTarget, TextTarget } from '@viewer/fluid/targets.js';
import { GlyphRenderer } from '@viewer/fluid/render.js';
import { GlyphRamps } from '@viewer/fluid/synth.js';

const W = 52, H = 32, STEP = 1 / 60;
const RATES = { blob: [1.6, 2.0], text: [7, 2.4] };
const DRAW = { ramp: GlyphRamps.Fade, palette: 'pastel', braille: 'all', edges: 1 };
const clamp01 = (v) => (v < 0 ? 0 : v > 1 ? 1 : v);

export class MoltyStage {
  constructor(px = 260) {
    this.el = document.createElement('canvas');
    this.el.className = 'molty-screen';
    this.renderer = new GlyphRenderer(this.el); this.renderer.resize(1);
    this.setSize(px);
    this.sim = new FluidSim(W, H, 412 / W, 412 / H);
    this.blobT = new BlobTarget();
    this.textT = new TextTarget(this.sim); this.textT.mode = 'plain';
    this.cur = { name: 'blob', target: this.blobT };
    this.t = 0; this.stateT = 0; this.acc = 0; this.breathPh = 0; this.overlay = []; this.meltT = 9;
    this.el.addEventListener('pointerdown', (e) => {
      const r = this.el.getBoundingClientRect();
      this.sim.impulse(((e.clientX - r.left) / r.width) * 2 - 1, ((e.clientY - r.top) / r.height) * 2 - 1, 1.6, 0.09);
    });
    this.last = performance.now();
    requestAnimationFrame((n) => this.frame(n));
  }
  setSize(px) { this.el.style.width = this.el.style.height = `${px}px`; }
  blob() { this.set('blob'); }
  /** The pairing code, condensed out of the fluid; uniform, no speed-reader ORP accent:
   *  a fractional pivot centres the code and matches no letter, so none is highlighted. */
  word(w) {
    const tok = { word: w, orp: (w.length - 1) / 2, core: w, para: false };
    this.textT.setSentence([tok]);
    this.textT.setWord(tok);
    this.set('text');
  }

  set(name) {
    const sim = this.sim, from = this.cur.name;
    if (from === name && name !== 'text') return;
    let sx = 0, sy = 0, s = 0; // break the old shape up from its centroid
    for (let k = 0; k < sim.N; k++) { const w = sim.ink[k]; sx += sim.X[k] * w; sy += sim.Y[k] * w; s += w; }
    sim.swirlKick = Math.max(sim.swirlKick, 3);
    sim.impulse(s > 1 ? sx / s : 0, s > 1 ? sy / s : 0, 0.6, 0.2);
    this.cur = { name, target: name === 'text' ? this.textT : this.blobT };
    if (name !== 'text') sim.BGM.fill(1);
    if (from === 'text' && name !== 'text') this.meltT = 0;
    sim.edgeW = 0;
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
