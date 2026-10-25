// The round Molty screen for the setup flow: the viewer's FluidStage (real fluid engine +
// real cute-kit Moltys, pastel palette) — imported via @viewer, never copied. Built as a
// separate module (vite.config.ts `virtual:molty-stage`) with three.js loaded from the CDN.
import { FluidStage } from '@viewer/setup/fluid-stage.js';

export class MoltyStage extends FluidStage {
  /** The pairing code, condensed out of the fluid — uniform, no speed-reader ORP accent:
   *  a fractional pivot centres the code and matches no letter, so none is highlighted. */
  word(w) {
    const tok = { word: w, orp: (w.length - 1) / 2, core: w, para: false };
    this.textT.setSentence([tok]);
    this.textT.setWord(tok);
    this.set('text');
  }
}
