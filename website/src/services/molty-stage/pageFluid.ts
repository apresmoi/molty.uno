// The landing page's one fluid: @glyphcss/fluid (molty's sim, bit-identical, drawn as braille
// DOM text in one <pre>, the sim in a Web Worker) glued to the document behind the page. This
// file is only the GLUE between the page and the package; it reads its targets from the DOM:
//   [data-fluid-word]   a heading; its own text condenses out of the fluid while on screen and
//                       melts back when scrolled past (`data-fluid-color`, `data-fluid-delay`,
//                       `data-fluid-tint`: take the body colour of that slot's current Molty)
//   [data-fluid-molty]  a square slot a Molty emerges in: the cute-kit rig poses it, glyphcss
//                       rasterises it into the fluid (moltyMesh.ts); a comma list with
//                       `data-fluid-every` cycles, resting a beat between Moltys
//                       `data-fluid-run-in`: the first time that Molty appears it runs in
//                       from the viewport's right edge to the slot, then dances as usual
//                       Now and then a Molty comes up to the screen (the glass beat): the slot's
//                       round clip is the window; each Molty has its own rotation of scenes
//                       (an eye close-up, a confused look, a bonk, a face pressed on, a peek)
//   [data-fluid-act]    (opt-in, on a molty slot) asks its Molty to do something NOW: the
//                       value is "mood:<mood>#n" (held MOOD_HOLD s, then the dance goes on)
//                       or "beat:<beat name>#n" (one of beatsOf(id)); `#n` makes a repeat a
//                       new value. It waits until the slot's Molty has formed, is dropped if
//                       the slot's Molty changes first, and the slot dispatches ACT_EVENT
//                       ({ act, kind, name, phase: "start" | "end" }) as it starts and ends
//                       (or is cut short). Reduced motion: a mood is a settled still, a beat
//                       a still of its midpoint. actions.ts has the protocol (the showcase).
//   [data-fluid-hold]   (set by the Device as its board turns in, on a molty slot) the Molty
//                       waits melted: the fluid cannot turn with the board, so it condenses
//                       as the turn ends. `data-fluid-yaw` (radians) turns the Molty with it.
//   [data-fluid-quiet]  the fluid recedes under prose
//   [data-fluid-pool]   the fluid pools here (an invitation to touch it)
// The pointer stirs it and taps ripple it (the package's window pointer). 30 fps, ≤176
// columns; with prefers-reduced-motion the package draws one settled still per change.
import { createGlyphFluid, GlyphFluidTargets, type GlyphFluid, type GlyphFluidTarget } from "@glyphcss/fluid";
import FluidWorker from "@glyphcss/fluid/worker?worker";
import { loadRig, MoltyRig, moltyCamera, MOLTY_EXTENT, MOLTY_KEY_LIGHT } from "./moltyMesh";
import { TargetShift } from "./shiftTarget";
import { BeatRun } from "./beats/player";
import { beatsOf, pickBeat, seedOf } from "./beats/registry";
import { ACT_EVENT, MOOD_HOLD, isMood, parseAct, type ActDetail, type ActionKind } from "./actions";

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
// the run-in: hopping in from the right edge (ease-out), turned 3/4 to face its way, then
// turning back to the camera as it lands in the slot
const RUN = 1.6; // s from beyond the edge to the slot
const TURN = 0.55; // s it takes to face the camera again, starting just before arrival
const RUN_YAW = -0.95; // rad: facing screen-left
// a Molty's silhouette fills about half its round fit, so draw it 1.18x larger in its slot
// (more cells per Molty); the run-in's sub-cell shift is in the same units
const EXTENT = MOLTY_EXTENT / 1.18;
// the glass beat: now and then a Molty comes up to the screen and plays a short scene (its
// own signature scenes and generic ones, beats/), then goes back to its place. It replaces the
// "surprised" beat of every 2nd dance loop; a cycling slot (the hero) does it once per
// appearance, after its first hops, when the appearance has time left for it; a Molty just
// picked in the gallery does it on its 3rd dance beat (~5 s in).
const GLASS_AT = 3; // the DANCE beat it replaces
const SOON_AT = 2; // …and the one it takes right after a pick
// an asked action (data-fluid-act) waits this long after its Molty starts condensing / swaps in
const FORM_READY = 1.5;
const SWAP_READY = 0.35;
const easeOut = (x: number) => 1 - Math.pow(1 - Math.min(1, Math.max(0, x)), 3);
const smooth = (x: number) => (x <= 0 ? 0 : x >= 1 ? 1 : x * x * (3 - 2 * x));
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
/** An action asked of a slot (data-fluid-act), for the Molty that was picked when asked. */
interface Act {
  key: string;
  kind: ActionKind;
  name: string;
  molty: string;
}
interface Slot {
  el: HTMLElement;
  name: string;
  delay: number;
  every: number;
  rig: MoltyRig;
  target: GlyphFluidTarget;
  shift: TargetShift;
  /** The Molty that runs in on its first appearance (consumed then, run or not). */
  runIn: string | null;
  /** Seconds into the run-in, while it plays. */
  run: number | null;
  /** The glass beat while it plays; whether this appearance had one; beats played per Molty
   *  in this slot (the next one steps on from it); a pick asks for one soon. */
  beat: BeatRun | null;
  glassed: boolean;
  plays: Map<string, number>;
  soon: boolean;
  /** Whole dance loops since this Molty appeared (the glass beat takes every 2nd). */
  loops: number;
  id: string | null;
  danceI: number;
  danceT: number;
  hopT: number;
  last: number;
  /** data-fluid-act: the action waiting for its Molty, the one playing (a beat in `beat`, a
   *  mood for `held` more seconds), when the Molty is formed enough, the last value seen. */
  ask: Act | null;
  act: Act | null;
  held: number;
  until: number;
  readyAt: number;
  seen: string;
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
    // a picker swapping a slot's Molty (MoltyPicker writes data-fluid-molty); the showcase
    // asking it for an action (data-fluid-act)
    const mo = new MutationObserver((records) => {
      for (const r of records) {
        const s = this.slots.find((x) => x.el === r.target);
        if (s && r.attributeName === "data-fluid-act") this.request(s);
        else if (s && r.attributeName === "data-fluid-hold") this.tick(s, false);
        else if (s) this.tick(s, true);
      }
      this.retint();
    });
    mo.observe(document.body, {
      subtree: true,
      attributes: true,
      attributeFilter: ["data-fluid-molty", "data-fluid-act", "data-fluid-hold"],
    });
    this.cleanup.push(() => mo.disconnect());
    // the slot schedule (cycling, the gap, the dance, asked actions) on a light clock; the
    // fluid has its own
    this.timer = window.setInterval(
      () =>
        this.slots.forEach((s) => {
          this.tick(s, false);
          this.serve(s);
        }),
      100,
    );
    this.debugHook();
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
      shift: new TargetShift(),
      runIn: this.options.reducedMotion ? null : (el.dataset.fluidRunIn ?? null),
      run: null,
      beat: null,
      glassed: false,
      plays: new Map(),
      soon: false,
      loops: 0,
      id: null,
      danceI: 0,
      danceT: 0,
      hopT: 0,
      last: performance.now(),
      ask: null,
      act: null,
      held: 0,
      until: 0,
      readyAt: 0,
      seen: "",
      target: null!,
    };
    s.target = this.fluid.addTarget(
      s.shift.wrap(
        GlyphFluidTargets.mesh(el, {
          polygons: () => this.pose(s),
          camera: moltyCamera(),
          extent: EXTENT,
          // no breathing scale: a formed Molty holds still while the field breathes around it
          swell: false,
          light: { direction: MOLTY_KEY_LIGHT, intensity: 1 },
          ambient: 0.4,
          // reduced motion: a settled Molty stays drawn while any of it is on screen, as before
          visibility: this.options.reducedMotion ? "always" : "viewport",
        }),
      ),
    );
    s.target.melt();
    this.slots.push(s);
    this.tick(s, false);
    this.request(s);
  }

  /** Which Molty a slot wants now (cycling slots rest for a beat between Moltys). */
  private want(s: Slot): string | null {
    const ids = (s.el.dataset.fluidMolty ?? "").split(",").filter(Boolean);
    if (!ids.length) return null;
    if (this.options.reducedMotion) return ids[0]!;
    if (this.t < s.delay || s.el.hasAttribute("data-fluid-hold")) return null;
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
        s.loops = 0;
        s.glassed = false;
        s.soon = picked && !this.options.reducedMotion;
        s.rig.setMood(this.options.reducedMotion ? "idle" : DANCE[0]![0]);
        this.endRun(s);
        this.endGlass(s);
        this.finish(s);
        s.readyAt = this.t + (s.target.enabled ? SWAP_READY : FORM_READY);
        if (want === s.runIn) {
          s.runIn = null;
          if (!picked && !s.target.enabled && this.inView(s)) this.startRun(s);
        }
        s.rig.pose(1 / 30);
        if (s.target.enabled) this.ripple(s, 0.5, 1.1); // swapped in place: the fluid answers
      }
    }
    const on = !!want && want === s.id;
    if (on && !s.target.enabled) {
      s.target.condense();
      s.readyAt = this.t + FORM_READY;
    } else if (!on && s.target.enabled) s.target.melt();
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
    if (s.run !== null) return this.running(s, dt);
    s.rig.yaw = Number(s.el.dataset.fluidYaw) || 0; // the Device turning in (0 at rest)
    if (s.beat) return this.glassing(s, dt);
    if (s.held > 0) return this.holding(s, dt);
    s.danceT += dt;
    if (s.danceT > DANCE[s.danceI]![1]) {
      s.danceI = (s.danceI + 1) % DANCE.length;
      s.danceT = 0;
      if (s.danceI === 0) s.loops++;
      if (this.glassDue(s)) {
        this.startGlass(s);
        return this.glassing(s, 0);
      }
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

  // ---------------------------------------------------------------- the run-in

  private inView(s: Slot) {
    const r = s.el.getBoundingClientRect(),
      mid = r.top + r.height / 2;
    return r.width > 0 && mid > innerHeight * 0.07 && mid < innerHeight * 0.95;
  }

  /** CSS px from the slot to just past the viewport's right edge (re-read every frame, so a
   *  resize mid-run only bends the path). */
  private runway(s: Slot) {
    const r = s.el.getBoundingClientRect();
    return Math.max(0, innerWidth - (r.left + r.width / 2) + r.width * 0.55);
  }

  private startRun(s: Slot) {
    s.run = 0;
    s.hopT = 0;
    s.rig.setMood("happy");
    s.rig.yaw = RUN_YAW;
    s.rig.shift = s.shift.set(this.runway(s), EXTENT); // the condense starts off screen
  }

  private endRun(s: Slot) {
    s.run = null;
    s.rig.yaw = 0;
    s.rig.shift = s.shift.set(0, EXTENT);
  }

  /** One frame of the run-in: slide in from the edge hopping, a footstep ripple per landing,
   *  turn back to the camera on arrival, then the usual dance from its start. */
  private running(s: Slot, dt: number) {
    const t = (s.run! += dt);
    const px = (1 - easeOut(t / RUN)) * this.runway(s);
    s.rig.shift = s.shift.set(px, EXTENT);
    s.rig.yaw = RUN_YAW * (1 - smooth((t - (RUN - 0.2)) / TURN));
    s.hopT += dt;
    if (t < RUN && s.hopT > HOP) {
      s.hopT = 0;
      this.ripple(s, 0.81, 0.7, 0.035, px); // a footstep where it lands
    }
    if (t >= RUN - 0.2 + TURN) {
      this.endRun(s);
      this.ripple(s, 0.81, 0.9); // arrived
      s.danceI = 0;
      s.danceT = 0;
      s.hopT = 0;
      s.rig.setMood(DANCE[0]![0]);
    }
    return s.rig.pose(dt);
  }

  // ---------------------------------------------------------------- the glass beat

  /** The beat this slot's Molty plays next. */
  private nextBeat(s: Slot) {
    return pickBeat(s.id!, this.slots.indexOf(s), s.plays.get(s.id!) ?? 0);
  }

  /** Is the beat just entered the glass beat's turn? A cycling slot: once per appearance,
   *  from its 2nd beat on, if it can finish before the slot rests; others: every 2nd loop,
   *  and soon after a pick. */
  private glassDue(s: Slot) {
    if (s.every && (s.el.dataset.fluidMolty ?? "").includes(",")) {
      if (s.glassed || s.danceI < 2 || s.danceI > GLASS_AT) return false;
      const k = (this.t - s.delay) / s.every;
      return (1 - GAP / s.every - (k - Math.floor(k))) * s.every > this.nextBeat(s).len + 0.4;
    }
    return (s.soon && s.danceI === SOON_AT) || (s.danceI === GLASS_AT && s.loops % 2 === 0);
  }

  private startGlass(s: Slot, beat = this.nextBeat(s), still = false) {
    this.endGlass(s);
    const k = s.plays.get(s.id!) ?? 0;
    s.plays.set(s.id!, k + 1);
    s.glassed = true;
    s.soon = false;
    s.beat = new BeatRun(
      beat,
      s.rig,
      still
        ? { rect: () => s.el.getBoundingClientRect(), impulse: () => {}, push: () => {} } // a still: no splashes
        : {
            rect: () => s.el.getBoundingClientRect(),
            impulse: (x, y, strength, radius) => this.fluid.impulse(x, y, { strength, radius }),
            push: (x, y, dx, dy, radius) => this.fluid.push(x, y, dx, dy, { radius }),
          },
      seedOf(s.id!, this.slots.indexOf(s), k),
    );
  }

  private endGlass(s: Slot) {
    s.beat?.end();
    s.beat = null;
  }

  /** One frame of the glass beat; on its end the dance goes on from the beat it replaced. */
  private glassing(s: Slot, dt: number) {
    if (!s.beat!.frame(dt)) {
      this.endGlass(s);
      s.danceT = 0;
      s.rig.setMood(DANCE[s.danceI]![0]);
      if (s.act?.kind === "beat") this.finish(s);
    }
    return s.rig.pose(dt);
  }

  // ---------------------------------------------------------------- asked actions (data-fluid-act)

  /** Read the slot's data-fluid-act: a new, well-formed value waits for its Molty. */
  private request(s: Slot) {
    const value = s.el.dataset.fluidAct ?? "";
    if (value === s.seen) return;
    s.seen = value;
    const a = parseAct(value);
    s.ask = a ? { ...a, molty: (s.el.dataset.fluidMolty ?? "").split(",")[0]! } : null;
  }

  /** Play the waiting ask once its Molty is the slot's and has formed; drop it if a newer pick
   *  took the slot first. */
  private serve(s: Slot) {
    if (s.act && this.options.reducedMotion && this.t >= s.until) this.settle(s);
    const a = s.ask;
    if (!a) return;
    if (a.molty !== (s.el.dataset.fluidMolty ?? "").split(",")[0]) return void (s.ask = null);
    if (s.id !== a.molty || !s.target.enabled || this.t < s.readyAt) return;
    s.ask = null;
    const beat = a.kind === "beat" ? beatsOf(a.molty).find((b) => b.name === a.name) : undefined;
    if (a.kind === "beat" ? !beat : !isMood(a.name)) return;
    this.finish(s);
    this.endRun(s);
    this.endGlass(s);
    s.soon = false;
    s.hopT = 0;
    const still = this.options.reducedMotion;
    if (beat) this.startGlass(s, beat, still);
    else {
      s.rig.setMood(a.name);
      s.held = MOOD_HOLD;
    }
    s.act = a;
    s.until = this.t + (beat ? beat.len : MOOD_HOLD);
    this.emit(s, a, "start");
    if (still) {
      // one settled still: a mood after a second of posing, a beat at its midpoint
      if (s.beat) while (s.beat.t < s.beat.beat.len / 2 && s.beat.frame(1 / 30)) s.rig.pose(1 / 30);
      else for (let i = 0; i < 36; i++) s.rig.pose(1 / 30);
      this.fluid.relayout();
    }
  }

  /** One frame of a held mood; then the dance goes on from where it was. */
  private holding(s: Slot, dt: number) {
    s.held -= dt;
    if (s.held <= 0) {
      s.danceT = 0;
      s.rig.setMood(DANCE[s.danceI]![0]);
      this.finish(s);
    } else if (s.act?.name === "happy" && (s.hopT += dt) > HOP) {
      s.hopT = 0;
      this.ripple(s, 0.81, 0.55, 0.03);
    }
    return s.rig.pose(dt);
  }

  /** Reduced motion: an asked action's time is up, back to the idle still. */
  private settle(s: Slot) {
    this.endGlass(s);
    this.finish(s);
    s.rig.setMood("idle");
    for (let i = 0; i < 36; i++) s.rig.pose(1 / 30);
    this.fluid.relayout();
  }

  /** The asked action is over (or cut short by another, or by a new Molty). */
  private finish(s: Slot) {
    const a = s.act;
    s.act = null;
    s.held = 0;
    if (a) this.emit(s, a, "end");
  }

  private emit(s: Slot, a: Act, phase: ActDetail["phase"]) {
    const detail: ActDetail = { act: a.key, kind: a.kind, name: a.name, phase };
    s.el.dispatchEvent(new CustomEvent<ActDetail>(ACT_EVENT, { detail }));
  }

  /** ?beats: a console hook to play any beat on any slot (beats/api.ts says how). */
  private debugHook() {
    if (!new URLSearchParams(location.search).has("beats")) return;
    const w = window as unknown as { __moltyBeats?: unknown };
    w.__moltyBeats = {
      slots: () => this.slots.map((s, i) => ({ i, slot: s.name, id: s.id, beat: s.beat?.beat.name ?? null })),
      list: (id: string) => beatsOf(id).map((b) => b.name),
      /** The slot's last polygons: count, NaN vertices, screen bounds (fit units). */
      inspect: (i: number) => {
        const ps = this.slots[i]?.rig.polygons ?? [];
        let nan = 0,
          x0 = Infinity,
          x1 = -Infinity,
          y0 = Infinity,
          y1 = -Infinity;
        for (const q of ps)
          for (const v of q.vertices) {
            if (v.some((c) => !Number.isFinite(c))) nan++;
            x0 = Math.min(x0, v[0]);
            x1 = Math.max(x1, v[0]);
            y0 = Math.min(y0, v[1]);
            y1 = Math.max(y1, v[1]);
          }
        return { n: ps.length, nan, x: [x0, x1], y: [y0, y1] };
      },
      pick: (i: number, id: string) => {
        const s = this.slots[i];
        if (s) s.el.dataset.fluidMolty = id;
      },
      play: (i: number, name: string) => {
        const s = this.slots[i];
        const beat = s?.id ? beatsOf(s.id).find((b) => b.name === name) : undefined;
        if (!s || !beat) return false;
        this.endRun(s);
        this.startGlass(s, beat);
        return true;
      },
    };
    this.cleanup.push(() => delete w.__moltyBeats);
  }

  /** A ripple at a point of the slot's square (`fy` down from its top), `dx` px to its right. */
  private ripple(s: Slot, fy: number, strength: number, radius = 0.06, dx = 0) {
    const r = s.el.getBoundingClientRect(),
      side = Math.min(r.width, r.height);
    this.fluid.impulse(r.left + r.width / 2 + dx, r.top + r.height / 2 + (fy - 0.5) * side, { strength, radius });
  }
}
