// The round Molty screen at the top of setup. The blob and the pairing code run on the
// REAL fluid engine in the page (src/stage/stage.js); a chosen Molty is a creature
// package only the device plays, so show() switches to the live device preview
// (GET /api/framebuffer). ?nostage, or an engine that fails to start, keeps the preview.
import type { DeviceApi } from '../api/device';
import { MoltyStage } from '../stage/stage.js';
import { h } from './dom';
import { preview, type Preview } from './preview';

let stage: MoltyStage | null = null; // one engine for the whole session; its canvas moves between pages
let broken = new URLSearchParams(location.search).has('nostage');

export interface MoltyScreen { el: HTMLElement; show(): void; blob(): void; word(w: string): void; stop(): void }

export function moltyScreen(api: DeviceApi, px = 248): MoltyScreen {
  const el = h('div', { class: 'fstage', style: `--px:${px}px` });
  let pv: Preview | null = null;
  const live = () => {
    if (!pv) { pv = preview(api, { size: px }); el.replaceChildren(pv.el); el.dataset.mode = 'device'; }
  };
  const fluid = (s: MoltyStage) => {
    pv?.stop(); pv = null;
    s.setSize(px); el.replaceChildren(s.el); el.dataset.mode = 'fluid';
  };
  const engine = () => {
    if (!broken) try { stage ??= new MoltyStage(px); } catch (e) { console.info('molty: fluid stage failed, using the live device preview', e); broken = true; }
    return broken ? null : stage;
  };
  const run = (f: (s: MoltyStage) => void) => { const s = engine(); if (s) { fluid(s); f(s); } else live(); };
  run((s) => s.blob());
  return {
    el,
    show: live,
    blob: () => run((s) => s.blob()),
    word: (w) => run((s) => s.word(w)),
    stop() { pv?.stop(); },
  };
}
