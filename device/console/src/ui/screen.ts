// The round Molty screen at the top of setup. Preferred: the REAL fluid engine with the
// real cute-kit Moltys (src/stage/entry.js, inlined as a string by vite.config.ts), which
// needs three.js from the CDN. If that import fails or stalls (offline, captive network),
// the screen falls back to the live device preview (GET /api/framebuffer) — which shows
// the same thing, just from the device itself.
import stageCode from 'virtual:molty-stage';
import type { DeviceApi } from '../api/device';
import { h } from './dom';
import { preview, type Preview } from './preview';

interface Stage { el: HTMLCanvasElement; show(id: string): void; blob(): void; word(w: string): void; setSize(px: number): void }
type StageCtor = new (px: number) => Stage;

let loading: Promise<StageCtor | null> | null = null;
let stage: Stage | null = null; // one engine for the whole session; its canvas moves between pages

/** Import the stage module once (blob URL → it imports three from the CDN). null = unavailable. */
export function loadStage(timeoutMs = 8000): Promise<StageCtor | null> {
  if (new URLSearchParams(location.search).has('nostage')) return Promise.resolve(null);
  loading ??= (async () => {
    const url = URL.createObjectURL(new Blob([stageCode], { type: 'text/javascript' }));
    try {
      const mod = await Promise.race([
        import(/* @vite-ignore */ url) as Promise<{ MoltyStage: StageCtor }>,
        new Promise<never>((_, no) => setTimeout(() => no(new Error('stage timeout')), timeoutMs)),
      ]);
      return mod.MoltyStage;
    } catch (e) {
      console.info('molty: fluid stage unavailable, using the live device preview', e);
      loading = null; // let a later page try again
      return null;
    } finally { setTimeout(() => URL.revokeObjectURL(url), 30000); }
  })();
  return loading;
}

type Cmd = { k: 'show'; id: string } | { k: 'blob' } | { k: 'word'; w: string };

export interface MoltyScreen { el: HTMLElement; mode(): 'loading' | 'fluid' | 'device'; show(id: string): void; blob(): void; word(w: string): void; stop(): void }

export function moltyScreen(api: DeviceApi, px = 248): MoltyScreen {
  const el = h('div', { class: 'fstage', style: `--px:${px}px` }, h('div', { class: 'molty-screen placeholder' }));
  let cmd: Cmd = { k: 'blob' }, pv: Preview | null = null, alive = true, mode: 'loading' | 'fluid' | 'device' = 'loading';
  const apply = () => {
    if (!stage || mode !== 'fluid') return;
    if (cmd.k === 'show') stage.show(cmd.id); else if (cmd.k === 'word') stage.word(cmd.w); else stage.blob();
  };
  loadStage().then((Ctor) => {
    if (!alive) return;
    if (Ctor) {
      try { stage ??= new Ctor(px); } catch (e) { console.info('molty: fluid stage failed', e); Ctor = null; }
    }
    if (Ctor && stage) { stage.setSize(px); el.replaceChildren(stage.el); mode = 'fluid'; apply(); }
    else { pv = preview(api, { auto: true, size: px }); el.replaceChildren(pv.el); mode = 'device'; }
    el.dataset.mode = mode;
  });
  const set = (c: Cmd) => { cmd = c; apply(); };
  return {
    el, mode: () => mode,
    show: (id) => set({ k: 'show', id }),
    blob: () => set({ k: 'blob' }),
    word: (w) => set({ k: 'word', w }),
    stop() { alive = false; pv?.stop(); },
  };
}
