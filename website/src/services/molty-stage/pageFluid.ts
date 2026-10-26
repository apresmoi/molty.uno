// The landing page's one fluid: @glyphcss/fluid (molty's sim, bit-identical, drawn as braille
// DOM text in one <pre>, the sim in a Web Worker) glued to the document behind the page. This
// file is only the GLUE between the page and the package; it reads its targets from the DOM:
//   [data-fluid-word]   a heading; its own text condenses out of the fluid while on screen and
//                       melts back when scrolled past (`data-fluid-color`, `data-fluid-delay`,
//                       `data-fluid-tint`: take the body colour of that slot's current Molty)
//   [data-fluid-molty]  a square slot a Molty emerges in: the cute-kit rig poses it, glyphcss
//                       rasterises it into the fluid (moltyMesh.ts); a comma list with
//                       `data-fluid-every` cycles, resting a beat between Moltys
//   [data-fluid-quiet]  the fluid recedes under prose
//   [data-fluid-pool]   the fluid pools here (an invitation to touch it)
// The pointer stirs it and taps ripple it (the package's window pointer). 30 fps, ≤176
// columns; with prefers-reduced-motion the package draws one settled still per change.
import { createGlyphFluid, GlyphFluidTargets, type GlyphFluid, type GlyphFluidTarget } from "@glyphcss/fluid";
import FluidWorker from "@glyphcss/fluid/worker?worker";
import { loadRig, MoltyRig, moltyCamera, MOLTY_EXTENT, MOLTY_KEY_LIGHT } from "./moltyMesh";

type Rig = Awaited<ReturnType<typeof loadRig>>;
type Catalog = typeof import("@creatures/cute/index.js").CUTE_CATALOG;

const FPS = 30;
const FLUID = "#6f8f82"; // the dim braille field
const GAP = 1.6; // seconds a cycling slot rests between Moltys: it melts, the fluid settles
// after a Molty emerges it dances (happy = hopping), rests, looks around (as fluid-stage.js)
const DANCE: Array<[string, number]> = [
  ["idle", 1.6],
  ["happy", 3.2],
  ["idle", 1.8],
  ["surprised", 1.0],
  ["happy", 3.6],
  ["listening", 1.8],
];
const HOP = 0.476; // s between the landings of a happy hop

const timeout = <T>(p: Promise<T>, ms: number) =>
  Promise.race([p, new Promise<never>((_, no) => setTimeout(() => no(new Error("molty engine timeout")), ms))]);

/** The Moltys need three.js from the CDN (the rig); the field runs without them if that fails. */
async function loadMoltys(): Promise<{ rig: Rig; catalog: Catalog }> {
  const [rig, cute] = await timeout(Promise.all([loadRig(), import("@creatures/cute/index.js")]), 12000);
  return { rig, catalog: cute.CUTE_CATALOG };
}

interface Word {
  el: HTMLElement;
  color: string;
  target: GlyphFluidTarget;
}
interface Slot {
  el: HTMLElement;
  name: string;
  delay: number;
  every: number;
  rig: MoltyRig;
  target: GlyphFluidTarget;
  id: string | null;
  danceI: number;
  danceT: number;
  hopT: number;
  last: number;
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
  const fluid = createGlyphFluid(host, {
    glue: "document",
    pointer: "window",
    fps: FPS,
    fpsFallback: false,
    worker: () => new FluidWorker(),
    field: { color: FLUID, level: 0.18 },
  });
  try {
    await timeout(fluid.ready, 10000);
  } catch (e) {
    fluid.destroy();
    throw e;
  }
  return new PageFluid(fluid, options);
}

class PageFluid implements PageFluidHandle {
  private words: Word[] = [];
  private slots: Slot[] = [];
  private moltys: { rig: Rig; catalog: Catalog } | null = null;
  private start = performance.now();
  private alive = true;
  private cleanup: Array<() => void> = [];
  private timer = 0;

  constructor(
    private fluid: GlyphFluid,
    private options: PageFluidOptions,
  ) {
    for (const el of document.querySelectorAll<HTMLElement>("[data-fluid-word]")) this.addWord(el, this.colorOf(el));
    for (const el of document.querySelectorAll<HTMLElement>("[data-fluid-quiet],[data-fluid-pool]"))
      fluid.addZone({ el, kind: el.hasAttribute("data-fluid-pool") ? "pool" : "quiet" });
    loadMoltys().then(
      (m) => {
        if (!this.alive) return;
        this.moltys = m;
        for (const el of document.querySelectorAll<HTMLElement>("[data-fluid-molty]")) this.addSlot(el);
        this.retint();
        this.options.onState?.("moltys");
      },
      () => {}, // no three.js: the field and the headings still run, slots show their stills
    );
    // a picker swapping a slot's Molty (MoltyPicker writes data-fluid-molty)
    const mo = new MutationObserver((records) => {
      for (const r of records) {
        const s = this.slots.find((x) => x.el === r.target);
        if (s) this.tick(s, true);
      }
      this.retint();
    });
    mo.observe(document.body, { subtree: true, attributes: true, attributeFilter: ["data-fluid-molty"] });
    this.cleanup.push(() => mo.disconnect());
    // the slot schedule (cycling, the gap, the dance) on a light clock; the fluid has its own
    this.timer = window.setInterval(() => this.slots.forEach((s) => this.tick(s, false)), 100);
    options.onState?.("live");
  }

  destroy() {
    this.alive = false;
    clearInterval(this.timer);
    this.cleanup.forEach((f) => f());
    for (const s of this.slots) s.rig.dispose();
    this.fluid.destroy();
  }

  private get t() {
    return (performance.now() - this.start) / 1000;
  }

  // ---------------------------------------------------------------- headings

  private addWord(el: HTMLElement, color: string) {
    const target = this.fluid.addTarget(
      // reduced motion: every heading stands settled (no delay, no scroll band), as before
      GlyphFluidTargets.text(
        el,
        this.options.reducedMotion
          ? { color, visibility: "always" }
          : { color, delay: Number(el.dataset.fluidDelay ?? 0), visibility: "viewport" },
      ),
    );
    this.words.push({ el, color, target });
  }

  private colorOf(el: HTMLElement): string {
    const tint = el.dataset.fluidTint;
    if (tint && this.moltys) {
      const id = this.slots.find((s) => s.name === tint)?.id;
      const body = this.moltys.catalog.find((c) => c.id === id)?.spec.palette.body;
      if (body && /^#[\da-f]{6}$/i.test(body)) return body;
    }
    const c = el.dataset.fluidColor;
    return c && /^#[\da-f]{6}$/i.test(c) ? c : FLUID;
  }

  /** A tinted heading follows its slot's Molty; a text target's colour is fixed, so swap it. */
  private retint() {
    for (const w of [...this.words]) {
      if (!w.el.dataset.fluidTint) continue;
      const color = this.colorOf(w.el);
      if (color === w.color) continue;
      w.target.remove();
      this.words.splice(this.words.indexOf(w), 1);
      this.addWord(w.el, color);
    }
  }

  // ---------------------------------------------------------------- Moltys

  private addSlot(el: HTMLElement) {
    const rig = new MoltyRig(this.moltys!.rig);
    const s: Slot = {
      el,
      name: el.dataset.fluidSlot ?? "",
      delay: Number(el.dataset.fluidDelay ?? 0),
      every: Number(el.dataset.fluidEvery ?? 0),
      rig,
      id: null,
      danceI: 0,
      danceT: 0,
      hopT: 0,
      last: performance.now(),
      target: null!,
    };
    s.target = this.fluid.addTarget(
      GlyphFluidTargets.mesh(el, {
        polygons: () => this.pose(s),
        camera: moltyCamera(),
        extent: MOLTY_EXTENT,
        light: { direction: MOLTY_KEY_LIGHT, intensity: 1 },
        ambient: 0.4,
        visibility: "viewport",
      }),
    );
    s.target.melt();
    this.slots.push(s);
    this.tick(s, false);
  }

  /** Which Molty a slot wants now (cycling slots rest for a beat between Moltys). */
  private want(s: Slot): string | null {
    const ids = (s.el.dataset.fluidMolty ?? "").split(",").filter(Boolean);
    if (!ids.length) return null;
    if (this.options.reducedMotion) return ids[0]!;
    if (this.t < s.delay) return null;
    if (!s.every || ids.length === 1) return ids[0]!;
    const k = (this.t - s.delay) / s.every;
    if (k - Math.floor(k) > 1 - GAP / s.every) return null; // the gap
    return ids[Math.floor(k) % ids.length]!;
  }

  private tick(s: Slot, picked: boolean) {
    const want = this.want(s);
    if (want && want !== s.id) {
      const entry = this.moltys?.catalog.find((c) => c.id === want);
      if (entry) {
        s.rig.setCreature(entry);
        s.id = want;
        s.danceI = 0;
        s.danceT = 0;
        s.rig.setMood(this.options.reducedMotion ? "idle" : DANCE[0]![0]);
        s.rig.pose(1 / 30);
        if (s.target.enabled) this.ripple(s, 0.5, 1.1); // swapped in place: the fluid answers
      }
    }
    const on = !!want && want === s.id;
    if (on && !s.target.enabled) s.target.condense();
    else if (!on && s.target.enabled) s.target.melt();
    // reduced motion draws a new still only on a change it can see (a target toggling or a
    // re-measure); a Molty swapped in place is neither, so re-measure
    if (picked && this.options.reducedMotion) this.fluid.relayout();
  }

  /** The mesh target asks every frame it is shown: advance the dance, pose, return polygons. */
  private pose(s: Slot) {
    const now = performance.now(),
      dt = Math.min(0.1, (now - s.last) / 1000);
    s.last = now;
    if (this.options.reducedMotion) return s.rig.polygons;
    s.danceT += dt;
    if (s.danceT > DANCE[s.danceI]![1]) {
      s.danceI = (s.danceI + 1) % DANCE.length;
      s.danceT = 0;
      s.rig.setMood(DANCE[s.danceI]![0]);
    }
    if (DANCE[s.danceI]![0] === "happy") {
      s.hopT += dt;
      if (s.hopT > HOP) {
        s.hopT = 0;
        this.ripple(s, 0.81, 0.55, 0.03); // every landing sends a little wave out from its feet
      }
    }
    return s.rig.pose(dt);
  }

  /** A ripple at a point of the slot's square (`fy` down from its top). */
  private ripple(s: Slot, fy: number, strength: number, radius = 0.06) {
    const r = s.el.getBoundingClientRect(),
      side = Math.min(r.width, r.height);
    this.fluid.impulse(r.left + r.width / 2, r.top + r.height / 2 + (fy - 0.5) * side, { strength, radius });
  }
}
