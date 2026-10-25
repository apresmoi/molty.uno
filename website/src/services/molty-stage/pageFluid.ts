// The landing page's one fluid: the REAL engine (creatures/js/fluid: FluidSim, GlyphRenderer,
// CreatureTarget, the cute-kit Moltys) stretched from the device's round 412 px disc to a
// full-bleed rectangle behind the page. Nothing here re-implements the fluid; it only
//   1. builds a FluidSim sized to the viewport and opens its round mask to a rectangle,
//   2. shims the GlyphRenderer's 2D context (non-uniform panel→pixel transform, no disc clip,
//      dot size per cell instead of the panel's 1.5-unit minimum),
//   3. writes TARGETS into the sim from the DOM, the same way the engine's own targets do:
//        [data-fluid-word]   a heading; its text is rasterised to ink, so it condenses out of
//                            the fluid while on screen and melts back when scrolled past
//        [data-fluid-molty]  a square slot; a CreatureTarget renders that Molty into a small
//                            proxy sim and its ink/colour is blitted into the page sim, so the
//                            Molty emerges from the page fluid itself and dances there
//        [data-fluid-quiet]  the fluid recedes under prose (background multiplier ↓)
//        [data-fluid-pool]   the fluid is denser here (an invitation to touch it)
//   4. keeps the fluid glued to the page: the sim scrolls by whole rows, the canvas by the rest.
// Cheap by construction: ≤176 columns, 30 fps, DPR ≤ 1.5, idles in hidden tabs, and with
// prefers-reduced-motion it draws one settled frame per scroll stop instead of animating.
import type { FluidSim } from "@creatures/fluid/sim.js";
import type { CreatureTarget } from "@creatures/fluid/targets.js";

type Core = {
  FluidSim: typeof import("@creatures/fluid/sim.js").FluidSim;
  GlyphRenderer: typeof import("@creatures/fluid/render.js").GlyphRenderer;
  GlyphRamps: Record<string, string>;
};
type Creatures = {
  CreatureTarget: typeof import("@creatures/fluid/targets.js").CreatureTarget;
  CUTE_CATALOG: typeof import("@creatures/cute/index.js").CUTE_CATALOG;
};
type RGB = [number, number, number];

const PANEL = 412; // GlyphRenderer draws in a fixed 412×412 panel space
const FPS = 30;
const MARGIN = 4; // spare rows above and below the viewport, so a fast scroll never shows an edge
const ASPECT = 1.6; // cell height / width, like the device's 52×32 grid
const DOT = { w: 0.26, h: 0.15 }; // braille dot size as a fraction of the cell (render.js DOT_W/H)
const FLUID: RGB = [0x6f / 255, 0x8f / 255, 0x82 / 255]; // the dim braille field
const RELAX = { up: 3, down: 2.4 };
// after a Molty emerges it dances (happy = hopping), rests, looks around (as fluid-stage.js)
const DANCE: Array<[string, number]> = [
  ["idle", 1.6],
  ["happy", 3.2],
  ["idle", 1.8],
  ["surprised", 1.0],
  ["happy", 3.6],
  ["listening", 1.8],
];

const timeout = <T>(p: Promise<T>, ms: number) =>
  Promise.race([p, new Promise<never>((_, no) => setTimeout(() => no(new Error("molty engine timeout")), ms))]);

async function loadCore(): Promise<Core> {
  const [sim, render, synth] = await Promise.all([
    import("@creatures/fluid/sim.js"),
    import("@creatures/fluid/render.js"),
    import("@creatures/fluid/synth.js"),
  ]);
  return { FluidSim: sim.FluidSim, GlyphRenderer: render.GlyphRenderer, GlyphRamps: synth.GlyphRamps };
}
/** The Moltys need three.js from the CDN; the field runs without them if that fails. */
async function loadCreatures(): Promise<Creatures> {
  const [targets, cute] = await timeout(
    Promise.all([import("@creatures/fluid/targets.js"), import("@creatures/cute/index.js")]),
    12000,
  );
  return { CreatureTarget: targets.CreatureTarget, CUTE_CATALOG: cute.CUTE_CATALOG };
}

const hex = (c: string | undefined): RGB | null =>
  c && /^#[\da-f]{6}$/i.test(c)
    ? [parseInt(c.slice(1, 3), 16) / 255, parseInt(c.slice(3, 5), 16) / 255, parseInt(c.slice(5, 7), 16) / 255]
    : null;
const smooth = (x: number) => (x <= 0 ? 0 : x >= 1 ? 1 : x * x * (3 - 2 * x));

/** Dispose a CreatureTarget AND give its WebGL context back (browsers cap live contexts). */
function release(ct: CreatureTarget | null) {
  if (!ct) return;
  ct.dispose();
  ct.renderer.forceContextLoss();
}

/** Open the sim's round device mask to the whole rectangle (borders stay closed). */
function rectify(sim: FluidSim) {
  const { W, H } = sim;
  let inside = 0;
  for (let j = 0; j < H; j++)
    for (let i = 0; i < W; i++) {
      const k = j * W + i,
        d = Math.min(i, W - 1 - i, j, H - 1 - j);
      sim.mask[k] = d > 0 ? 1 : 0;
      inside += sim.mask[k]!;
      sim.sponge[k] = Math.max(0, 1 - d / 3); // ripples fade at the edges instead of ringing
    }
  sim.rim.fill(1);
  sim.inside = inside;
}

/** The engine's GlyphRenderer on a rectangle: its context draws the 412 panel stretched to the
 *  canvas, skips the disc clip, and keeps dots proportional to the (smaller) page cells. */
function rectRenderer(core: Core, canvas: HTMLCanvasElement) {
  const renderer = new core.GlyphRenderer(canvas);
  const g = renderer.g as CanvasRenderingContext2D & Record<string, unknown>;
  const k = { sx: 1, sy: 1, dotW: 1, dotH: 1 };
  const setTransform = g.setTransform.bind(g);
  const fillRect = g.fillRect.bind(g);
  g.setTransform = (() => setTransform(k.sx, 0, 0, k.sy, 0, 0)) as typeof g.setTransform;
  g.arc = () => {};
  g.clip = (() => {}) as typeof g.clip;
  g.fillRect = (x: number, y: number, w: number, h: number) => {
    if (w >= PANEL) return fillRect(x, y, w, h); // the panel background
    const w2 = Math.min(w, k.dotW),
      h2 = Math.min(h, k.dotH);
    fillRect(x + (w - w2) / 2, y + (h - h2) / 2, w2, h2);
  };
  return { renderer, k };
}

interface Word {
  el: HTMLElement;
  i0: number;
  j0: number; // document row
  w: number;
  h: number;
  cov: Float32Array;
  delay: number;
  on: boolean;
}
interface Slot {
  el: HTMLElement;
  name: string;
  delay: number;
  every: number;
  size: number; // cells across (proxy W)
  i0: number;
  j0: number; // document row of the square's top
  proxy: FluidSim | null;
  ct: CreatureTarget | null;
  id: string | null;
  on: boolean;
  danceI: number;
  danceT: number;
  mood: string;
  hopT: number;
}
interface Zone {
  el: HTMLElement;
  pool: boolean;
  level: number;
  i0: number;
  i1: number;
  j0: number;
  j1: number;
}

export interface PageFluidHandle {
  destroy(): void;
}

export interface PageFluidOptions {
  reducedMotion: boolean;
  /** Called once the field is drawing (and again when the Moltys are ready). */
  onState?: (state: "live" | "moltys") => void;
}

export async function mountPageFluid(host: HTMLElement, options: PageFluidOptions): Promise<PageFluidHandle> {
  const core = await timeout(loadCore(), 10000);
  return new PageFluid(host, core, options);
}

class PageFluid implements PageFluidHandle {
  private canvas = document.createElement("canvas");
  private shim: ReturnType<typeof rectRenderer>;
  private sim!: FluidSim;
  private cols = 0;
  private rows = 0;
  private cw = 10;
  private ch = 16;
  private r0 = 0; // document row of the sim's top row
  private words: Word[] = [];
  private slots: Slot[] = [];
  private zones: Zone[] = [];
  private creatures: Creatures | null = null;
  private t = 0;
  private last = performance.now();
  private lastDraw = 0;
  private lastEvent = 0;
  private breathPh = 0;
  private raf = 0;
  private alive = true;
  private pointer = { x: 0, y: 0, px: NaN, py: NaN, moved: false };
  private cleanup: Array<() => void> = [];
  private settleTimer = 0;

  constructor(
    private host: HTMLElement,
    private core: Core,
    private options: PageFluidOptions,
  ) {
    this.canvas.setAttribute("aria-hidden", "true");
    this.shim = rectRenderer(core, this.canvas);
    host.replaceChildren(this.canvas);
    this.layout();
    this.listen();
    loadCreatures().then(
      (c) => {
        if (!this.alive) return;
        this.creatures = c;
        this.options.onState?.("moltys");
        if (this.options.reducedMotion) this.settle();
      },
      () => {}, // no three.js: the field and the headings still run, slots show their stills
    );
    if (options.reducedMotion) this.settle();
    else this.raf = requestAnimationFrame((n) => this.frame(n));
    options.onState?.("live");
  }

  destroy() {
    this.alive = false;
    cancelAnimationFrame(this.raf);
    clearTimeout(this.settleTimer);
    this.cleanup.forEach((f) => f());
    for (const s of this.slots) release(s.ct);
    this.canvas.remove();
  }

  // ---------------------------------------------------------------- layout

  private layout() {
    const vw = document.documentElement.clientWidth,
      vh = window.innerHeight;
    // ~144 columns on a laptop; never coarser than 10 px or finer than 5.4 px per cell, so a
    // heading always spans enough rows to read, and never more than 176 columns of work
    const cols = Math.min(176, Math.round(vw / Math.max(5.4, Math.min(10, vw / 144))));
    const cw = vw / cols,
      ch = cw * ASPECT;
    const rows = Math.ceil(vh / ch) + 2 * MARGIN + 1;
    // a phone's toolbar collapsing changes only the height: keep the fluid unless it must grow
    if (cols !== this.cols || rows > this.rows || Math.abs(cw - this.cw) > 0.01) {
      this.cols = cols;
      this.rows = rows;
      this.cw = cw;
      this.ch = ch;
      const sim = new this.core.FluidSim(cols, rows, cw, ch);
      rectify(sim);
      sim.p.bgColor = [...FLUID];
      sim.p.bgLevel = 0.18;
      // the engine's field and eddy frequencies are per disc; keep their size in pixels
      const zoom = Math.max(1, ((cols * cw) / PANEL) * 0.6);
      sim.p.bgFreq *= zoom;
      sim.p.swirlFreq *= zoom;
      sim.p.relaxUp = RELAX.up;
      sim.p.relaxDown = RELAX.down;
      sim.edgeW = 1;
      sim.r.fill(FLUID[0]);
      sim.g.fill(FLUID[1]);
      sim.b.fill(FLUID[2]);
      this.sim = sim;
      this.r0 = Math.floor(window.scrollY / ch) - MARGIN;
      const dpr = Math.min(1.5, window.devicePixelRatio || 1);
      this.canvas.width = Math.round(cols * cw * dpr);
      this.canvas.height = Math.round(rows * ch * dpr);
      this.canvas.style.width = `${cols * cw}px`;
      this.canvas.style.height = `${rows * ch}px`;
      const k = this.shim.k;
      k.sx = this.canvas.width / PANEL;
      k.sy = this.canvas.height / PANEL;
      k.dotW = (DOT.w * cw * dpr) / k.sx; // back in panel units
      k.dotH = (DOT.h * ch * dpr) / k.sy;
      for (const s of this.slots) release(s.ct);
      this.slots = [];
    }
    this.measure();
    this.place();
  }

  /** Read every fluid element's document position (on load, resize and content reflow). */
  private measure() {
    const { cw, ch } = this,
      sy = window.scrollY;
    this.words = [...document.querySelectorAll<HTMLElement>("[data-fluid-word]")].map((el) => {
      const prev = this.words.find((w) => w.el === el);
      return {
        ...this.rasterWord(el, cw, ch, sy),
        el,
        delay: Number(el.dataset.fluidDelay ?? 0),
        on: prev?.on ?? false,
      };
    });
    const slotEls = [...document.querySelectorAll<HTMLElement>("[data-fluid-molty]")];
    const kept: Slot[] = [];
    for (const el of slotEls) {
      const r = el.getBoundingClientRect(),
        px = Math.min(r.width, r.height);
      const size = Math.max(8, Math.round(px / cw));
      const i0 = Math.round((r.left + r.width / 2) / cw - size / 2);
      const j0 = Math.round((r.top + sy + r.height / 2) / ch - (size * cw) / ch / 2);
      let s = this.slots.find((x) => x.el === el);
      if (s && s.size !== size) {
        release(s.ct);
        s.ct = null;
        s.proxy = null;
        s.id = null;
      }
      s ??= {
        el,
        name: el.dataset.fluidSlot ?? "",
        delay: Number(el.dataset.fluidDelay ?? 0),
        every: Number(el.dataset.fluidEvery ?? 0),
        size,
        i0,
        j0,
        proxy: null,
        ct: null,
        id: null,
        on: false,
        danceI: 0,
        danceT: 0,
        mood: "idle",
        hopT: 0,
      };
      Object.assign(s, { size, i0, j0 });
      kept.push(s);
    }
    for (const s of this.slots) if (!kept.includes(s)) release(s.ct);
    this.slots = kept;
    this.zones = [...document.querySelectorAll<HTMLElement>("[data-fluid-quiet],[data-fluid-pool]")].map((el) => {
      const r = el.getBoundingClientRect();
      return {
        el,
        pool: el.hasAttribute("data-fluid-pool"),
        level: el.hasAttribute("data-fluid-pool") ? 3.2 : 0.12,
        i0: Math.floor(r.left / cw),
        i1: Math.ceil(r.right / cw),
        j0: Math.floor((r.top + sy) / ch),
        j1: Math.ceil((r.bottom + sy) / ch),
      };
    });
  }

  /** A heading's own text, in its own computed font, rasterised to per-cell coverage. */
  private rasterWord(el: HTMLElement, cw: number, ch: number, sy: number) {
    const r = el.getBoundingClientRect(),
      top = r.top + sy;
    const i0 = Math.floor(r.left / cw),
      j0 = Math.floor(top / ch);
    const w = Math.max(1, Math.ceil(r.right / cw) - i0),
      h = Math.max(1, Math.ceil((top + r.height) / ch) - j0);
    const S = 2; // supersampling
    const cv = document.createElement("canvas");
    cv.width = Math.ceil(w * cw * S);
    cv.height = Math.ceil(h * ch * S);
    const g = cv.getContext("2d", { willReadFrequently: true })!;
    const cs = getComputedStyle(el);
    g.scale(S, S);
    g.font = `${cs.fontWeight} ${cs.fontSize} ${cs.fontFamily}`;
    if ("letterSpacing" in g) (g as { letterSpacing: string }).letterSpacing = cs.letterSpacing;
    g.textBaseline = "alphabetic";
    g.fillStyle = "#fff";
    const text = el.textContent ?? "";
    const m = g.measureText(text);
    const asc = m.fontBoundingBoxAscent,
      desc = m.fontBoundingBoxDescent;
    const padL = parseFloat(cs.paddingLeft) || 0;
    g.fillText(text, r.left - i0 * cw + padL, top - j0 * ch + (r.height - (asc + desc)) / 2 + asc);
    const img = g.getImageData(0, 0, cv.width, cv.height).data;
    const cov = new Float32Array(w * h);
    for (let j = 0; j < h; j++)
      for (let i = 0; i < w; i++) {
        const xa = Math.floor(i * cw * S),
          xb = Math.min(cv.width, Math.floor((i + 1) * cw * S));
        const ya = Math.floor(j * ch * S),
          yb = Math.min(cv.height, Math.floor((j + 1) * ch * S));
        let s = 0,
          n = 0;
        for (let y = ya; y < yb; y++) for (let x = xa; x < xb; x++) ((s += img[(y * cv.width + x) * 4 + 3]!), n++);
        cov[j * w + i] = Math.min(1, (s / Math.max(1, n * 255)) * 1.35);
      }
    return { i0, j0, w, h, cov };
  }

  // ---------------------------------------------------------------- scrolling + input

  private listen() {
    const on = <K extends keyof WindowEventMap>(type: K, fn: (e: WindowEventMap[K]) => void) => {
      window.addEventListener(type, fn, { passive: true });
      this.cleanup.push(() => window.removeEventListener(type, fn));
    };
    let resizeTimer = 0;
    on("resize", () => {
      clearTimeout(resizeTimer);
      resizeTimer = window.setTimeout(() => this.layout(), 150);
    });
    on("scroll", () => {
      this.place();
      if (this.options.reducedMotion) {
        clearTimeout(this.settleTimer);
        this.settleTimer = window.setTimeout(() => this.settle(), 140);
      }
    });
    on("pointermove", (e) => {
      const p = this.pointer;
      p.x = e.clientX;
      p.y = e.clientY;
      p.moved = true;
    });
    on("pointerdown", (e) => {
      if (this.options.reducedMotion) return;
      const [x, y] = this.toStage(e.clientX, e.clientY);
      this.sim.impulse(x, y, 1.5, 0.045);
    });
    const ro = new ResizeObserver(() => this.measure());
    ro.observe(document.body);
    this.cleanup.push(() => ro.disconnect());
    if (this.options.reducedMotion) {
      const mo = new MutationObserver(() => this.settle());
      mo.observe(document.body, { subtree: true, attributes: true, attributeFilter: ["data-fluid-molty"] });
      this.cleanup.push(() => mo.disconnect());
    }
    document.fonts?.ready.then(() => this.alive && this.measure());
  }

  /** Keep the drawn rows glued to the document between frames. */
  private place() {
    this.canvas.style.transform = `translate3d(0, ${(this.r0 * this.ch - window.scrollY).toFixed(2)}px, 0)`;
  }

  private toStage(cx: number, cy: number): [number, number] {
    const top = this.r0 * this.ch - window.scrollY;
    return [(cx / (this.cols * this.cw)) * 2 - 1, ((cy - top) / (this.rows * this.ch)) * 2 - 1];
  }
  private cellToStage(i: number, j: number): [number, number] {
    return [((i + 0.5) / this.cols) * 2 - 1, ((j - this.r0 + 0.5) / this.rows) * 2 - 1];
  }

  /** Scroll the sim with the page: shift every carried field by whole rows. */
  private scrollRows() {
    const want = Math.floor(window.scrollY / this.ch) - MARGIN,
      d = want - this.r0;
    if (!d) return;
    this.r0 = want;
    const sim = this.sim,
      { W, H, N } = sim;
    const fields: Array<[Float32Array, number]> = [
      [sim.h, 0],
      [sim.hp, 0],
      [sim.ink, 0],
      [sim.acc, 0],
      [sim.u, 0],
      [sim.v, 0],
      [sim.r, FLUID[0]],
      [sim.g, FLUID[1]],
      [sim.b, FLUID[2]],
    ];
    for (const [a, fill] of fields) {
      if (Math.abs(d) >= H) a.fill(fill);
      else if (d > 0) {
        a.copyWithin(0, d * W);
        a.fill(fill, (H - d) * W, N);
      } else {
        a.copyWithin(-d * W, 0, (H + d) * W);
        a.fill(fill, 0, -d * W);
      }
    }
  }

  // ---------------------------------------------------------------- targets

  private event(i: number, j: number, strength: number) {
    this.lastEvent = this.t;
    this.sim.swirlKick = Math.max(this.sim.swirlKick, 1.2);
    const [x, y] = this.cellToStage(i, j);
    this.sim.impulse(x, y, strength, 0.06);
  }

  private colorOf(el: HTMLElement): RGB {
    const tint = el.dataset.fluidTint;
    if (tint && this.creatures) {
      const slot = this.slots.find((s) => s.name === tint);
      const body = this.creatures.CUTE_CATALOG.find((c) => c.id === slot?.id)?.spec.palette.body;
      const c = hex(body);
      if (c) return c;
    }
    return hex(el.dataset.fluidColor) ?? FLUID;
  }

  /** Which Molty a slot wants now (cycling slots rest for a beat between Moltys). */
  private slotWant(s: Slot, settled: boolean): string | null {
    const ids = (s.el.dataset.fluidMolty ?? "").split(",").filter(Boolean);
    if (!ids.length) return null;
    if (settled) return ids[0]!;
    if (this.t < s.delay) return null;
    if (!s.every || ids.length === 1) return ids[0]!;
    const k = (this.t - s.delay) / s.every;
    if (k - Math.floor(k) > 1 - 1.6 / s.every) return null; // the gap: it melts, the fluid settles
    return ids[Math.floor(k) % ids.length]!;
  }

  /** Viewport band test on a document-row span. */
  private inView(j0: number, j1: number, lo: number, hi: number) {
    const vh = window.innerHeight,
      sy = window.scrollY;
    const mid = ((j0 + j1) / 2) * this.ch - sy;
    return mid > vh * lo && mid < vh * hi;
  }

  private fillTargets(dt: number, swell: number, settled: boolean) {
    const sim = this.sim,
      { W, H, T, TR, TG, TB, TF, TE, TEX, TEY, TA, BGM } = sim;
    T.fill(0);
    TF.fill(0);
    TE.fill(0);
    TA.fill(0);

    // headings
    for (const w of this.words) {
      const want =
        (settled || this.t >= w.delay) && this.inView(w.j0, w.j0 + w.h, settled ? -0.2 : 0.07, settled ? 1.2 : 0.95);
      if (want !== w.on) {
        w.on = want;
        if (!settled) this.event(w.i0 + w.w / 2, w.j0 + w.h / 2, want ? 0.5 : 0.9);
      }
      if (!w.on) continue;
      const [cr, cg, cb] = this.colorOf(w.el);
      for (let jj = 0; jj < w.h; jj++) {
        const j = w.j0 + jj - this.r0;
        if (j < 1 || j >= H - 1) continue;
        for (let ii = 0; ii < w.w; ii++) {
          const i = w.i0 + ii;
          if (i < 1 || i >= W - 1) continue;
          const v = w.cov[jj * w.w + ii]!,
            k = j * W + i;
          if (v > T[k]!) {
            T[k] = v;
            TR[k] = cr;
            TG[k] = cg;
            TB[k] = cb;
          }
        }
      }
    }

    // Moltys
    const cat = this.creatures;
    for (const s of this.slots) {
      const sizeRows = Math.round((s.size * this.cw) / this.ch);
      const visible = this.inView(s.j0, s.j0 + sizeRows, settled ? -0.3 : 0.05, settled ? 1.3 : 0.98);
      const want = visible ? this.slotWant(s, settled) : null;
      if (!cat) continue;
      if (want && !s.ct) {
        s.proxy = new this.core.FluidSim(s.size, sizeRows, this.cw, this.ch);
        s.ct = new cat.CreatureTarget(s.proxy);
      }
      if (want && want !== s.id && s.ct) {
        const entry = cat.CUTE_CATALOG.find((c) => c.id === want);
        if (entry) {
          s.ct.setCreature(entry);
          s.id = want;
          s.danceI = 0;
          s.danceT = 0;
          this.setMood(s, DANCE[0]![0]);
          if (!settled) this.event(s.i0 + s.size / 2, s.j0 + sizeRows / 2, 1.1);
        }
      }
      const on = !!want && want === s.id;
      if (on !== s.on) {
        s.on = on;
        if (!settled) this.event(s.i0 + s.size / 2, s.j0 + sizeRows / 2, on ? 0.6 : 0.8);
      }
      if (!s.on || !s.ct || !s.proxy) continue;
      if (settled) this.setMood(s, "idle");
      else this.dance(s, dt, sizeRows);
      s.ct.update(settled ? 1 / 30 : dt);
      s.ct.fill(s.proxy, this.t, swell);
      const p = s.proxy;
      for (let jj = 0; jj < p.H; jj++) {
        const j = s.j0 + jj - this.r0;
        if (j < 1 || j >= H - 1) continue;
        for (let ii = 0; ii < p.W; ii++) {
          const i = s.i0 + ii;
          if (i < 1 || i >= W - 1) continue;
          const q = jj * p.W + ii,
            v = p.T[q]!,
            k = j * W + i;
          if (v < 0.004 || v < T[k]!) continue;
          T[k] = v;
          TR[k] = p.TR[q]!;
          TG[k] = p.TG[q]!;
          TB[k] = p.TB[q]!;
          TF[k] = p.TF[q]!;
          TE[k] = p.TE[q]!;
          TEX[k] = p.TEX[q]!;
          TEY[k] = p.TEY[q]!;
        }
      }
    }

    // where the fluid recedes (prose) or pools (the touch area)
    BGM.fill(1);
    for (const z of this.zones) {
      if (z.pool) {
        // a soft-edged pond: denser toward its middle, no box
        const ci = (z.i0 + z.i1) / 2,
          cj = (z.j0 + z.j1) / 2,
          ri = (z.i1 - z.i0) / 2,
          rj = (z.j1 - z.j0) / 2;
        for (let jd = z.j0; jd <= z.j1; jd++) {
          const j = jd - this.r0;
          if (j < 0 || j >= H) continue;
          for (let i = Math.max(0, z.i0); i <= Math.min(W - 1, z.i1); i++) {
            const d = Math.hypot((i - ci) / ri, (jd - cj) / rj);
            if (d < 1) BGM[j * W + i] = 1 + (z.level - 1) * smooth((1 - d) / 0.7);
          }
        }
        continue;
      }
      for (let jd = z.j0 - 1; jd <= z.j1; jd++) {
        const j = jd - this.r0;
        if (j < 0 || j >= H) continue;
        const edgeY = jd < z.j0 || jd === z.j1;
        for (let i = Math.max(0, z.i0 - 1); i <= Math.min(W - 1, z.i1); i++) {
          const edge = edgeY || i < z.i0 || i === z.i1;
          const k = j * W + i;
          BGM[k] = edge ? (BGM[k]! + z.level) / 2 : z.level;
        }
      }
    }
  }

  private setMood(s: Slot, mood: string) {
    s.mood = mood;
    s.ct?.setMood(mood);
  }

  private dance(s: Slot, dt: number, sizeRows: number) {
    s.danceT += dt;
    if (s.danceT > DANCE[s.danceI]![1]) {
      s.danceI = (s.danceI + 1) % DANCE.length;
      s.danceT = 0;
      this.setMood(s, DANCE[s.danceI]![0]);
    }
    if (s.mood !== "happy") return;
    s.hopT += dt;
    if (s.hopT > 0.476) {
      // every landing of the hop sends a little wave out from under its feet
      s.hopT = 0;
      const [x, y] = this.cellToStage(s.i0 + s.size / 2, s.j0 + sizeRows * 0.81);
      this.sim.impulse(x, y, 0.55, 0.03);
    }
  }

  // ---------------------------------------------------------------- frames

  private frame(now: number) {
    if (!this.alive) return;
    this.raf = requestAnimationFrame((n) => this.frame(n));
    if (document.hidden || now - this.lastDraw < 1000 / FPS - 3) {
      if (document.hidden) this.last = now;
      return;
    }
    this.lastDraw = now;
    const dt = Math.min(0.1, (now - this.last) / 1000);
    this.last = now;
    this.t += dt;
    this.breathPh += dt / 4;
    const breath = 0.5 - 0.5 * Math.cos(this.breathPh * Math.PI * 2),
      swell = 1 + 0.04 * (breath * 2 - 1);
    this.scrollRows();
    this.place();
    this.stir(dt);
    const sim = this.sim;
    this.fillTargets(dt, swell, false);
    const q = (this.t - this.lastEvent - 0.4) / 1.0;
    sim.p.hold = smooth(q);
    sim.p.tintRate = 3 + 9 * sim.p.hold;
    sim.applyBackground(this.t, breath);
    sim.step(Math.min(dt, 1 / 30));
    sim.computeView(null);
    this.draw(0.12);
  }

  /** The pointer drags the fluid and leaves a wake. */
  private stir(dt: number) {
    const p = this.pointer;
    if (!p.moved) return;
    p.moved = false;
    const dx = p.x - p.px,
      dy = p.y - p.py;
    p.px = p.x;
    p.py = p.y;
    if (Number.isNaN(dx)) return; // first sample: no velocity yet
    const speed = Math.hypot(dx, dy) / Math.max(dt, 1 / 60);
    if (speed < 20 || speed > 20000) return;
    const [x, y] = this.toStage(p.x, p.y);
    const s = Math.min(1, speed / 1600);
    this.sim.impulse(x, y, 0.35 * s, 0.03);
    this.sim.push(x, y, (dx / this.cw) * 0.5, (dy / this.cw) * 0.5, 0.05);
  }

  private draw(shimmer: number) {
    this.shim.renderer.draw(this.sim, {
      ramp: this.core.GlyphRamps.Fade,
      palette: "pastel",
      braille: "all",
      edges: 1,
      time: this.t,
      shimmer,
      letters: [],
      melt: 0,
    });
  }

  /** prefers-reduced-motion: one settled frame — every target fully formed, no flow. */
  private settle() {
    if (!this.alive) return;
    this.scrollRows();
    this.place();
    const sim = this.sim;
    this.fillTargets(0, 1, true);
    sim.applyBackground(this.t, 0.5);
    sim.ink.set(sim.T);
    sim.r.set(sim.TR);
    sim.g.set(sim.TG);
    sim.b.set(sim.TB);
    sim.acc.fill(0);
    sim.h.fill(0);
    sim.hp.fill(0);
    sim.p.hold = 1;
    sim.computeView(null);
    this.draw(0);
  }
}
