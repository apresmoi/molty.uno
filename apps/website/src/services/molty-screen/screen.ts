// A round screen on the page that runs Molty as the device does: one instance of the firmware's
// renderer (core.ts) drawing into a canvas, the pointer as the finger on the glass. The module
// and the packages are fetched once, when the first screen mounts, never with the page itself.
//
//   frames     the display's, at most 60 a second, only while the screen is in view and the tab
//              is shown; the clock the firmware sees stops with them
//   reduced    prefers-reduced-motion: no frames; one settled still per change
//   <html data-fluid-moltys>   "live" once a screen draws, "off" when the module cannot load
//              (the page then keeps its still images)
import wasmUrl from "@molty-wasm/molty.wasm?url";
import { MoltyCore, PANEL, type MoltyQuestion, type MoltyState } from "./core";

/** The page's frame rate. The device paces its own frames at 33 a second (mole_fluid.c). */
const FPS = 60;
const FRAME_MS = 1000 / FPS;
/** The longest step the firmware takes for one frame (mole_fluid.c clamps the same). */
const MAX_STEP_MS = 100;
/** Reduced motion and a screen that starts on its creature: this much is played unseen first. */
const SETTLE_S = 1.6;
const SETTLE_STEP_MS = 1000 / 30;

let moduleLoad: Promise<WebAssembly.Module> | null = null;
function loadModule(): Promise<WebAssembly.Module> {
  return (moduleLoad ??= WebAssembly.compileStreaming(fetch(wasmUrl)).catch(async () =>
    // a server that does not say application/wasm
    WebAssembly.compile(await (await fetch(wasmUrl)).arrayBuffer()),
  ));
}
const files = new Map<string, Promise<Uint8Array>>();
function loadFile(url: string): Promise<Uint8Array> {
  let file = files.get(url);
  if (!file) {
    file = fetch(url).then(async (r) => {
      if (!r.ok) throw new Error(`${url}: ${r.status}`);
      return new Uint8Array(await r.arrayBuffer());
    });
    files.set(url, file);
  }
  return file;
}
/** "off" never takes the page back from "live": one screen that failed is not all of them. */
function pageState(state: "live" | "off") {
  const root = document.documentElement;
  if (state === "live" || root.dataset.fluidMoltys !== "live") root.dataset.fluidMoltys = state;
}

/** The entrance (the hero): the cat starts outside the frame, to the left, and walks in. In
 *  world units, measured on the firmware's own frames: the frame's middle to its rim is 0.87. */
const ENTER = {
  across: -1.1,
  back: 0.3,
  /** Played unseen first: it gets up and takes its first steps. */
  lead: 1.0,
  /** Its clock runs this much faster until it sits: under 4 s on the page. */
  rate: 1.8,
  /** The window beside the screen looks this far to the side... */
  pan: -0.9,
};
/** ... which is this much of the panel's width (0.9 world units: 240 of 466 px). */
export const ENTER_SHIFT = 240 / 466;

export interface MoltyScreenOptions {
  /** The creature package: its id and its file's URL (packages/<id>/<id>.json). None: a
   *  screen that only plays scenes (a word). */
  creature?: { id: string; url: string };
  /** A scenes package's file, for `scene`. */
  scenes?: string;
  /** The panel's side in pixels (the canvas must be that size). */
  px?: number;
  /** Start with the device's boot screen ("molty.uno"), else on the creature. */
  boot: boolean;
  /** A scene it plays from the start and keeps up (a word). */
  scene?: { layout: string; params?: Record<string, unknown> };
  /** The finger is every tap on the page that lands on the canvas (a canvas that takes no
   *  pointer itself: a word under the page's text), not the canvas's own pointer. */
  pageTaps?: boolean;
  /** The creature walks in from outside the frame. `outside`: a canvas beside the screen (same
   *  size, ENTER_SHIFT of it to the left) that shows the water there while it does. */
  entrance?: { outside: HTMLCanvasElement | null };
  reducedMotion: boolean;
  /** A tap (or `pick`) answered the question on screen with this option. */
  onAnswer?: (index: number) => void;
  /** Every frame drawn, with the seconds this screen has run (they stop while it is unseen). */
  onFrame?: (seconds: number) => void;
  /** The entrance is over: the creature sits. */
  onEntered?: () => void;
}

export interface MoltyScreenHandle {
  /** POST /api/creature/state's body, as on the device: { mood }, { act }, { state, text, wpm }. */
  state(body: Record<string, unknown>): boolean;
  /** GET /api/creature/state. */
  read(): MoltyState | null;
  ask(q: MoltyQuestion): boolean;
  askEnd(): void;
  pick(index: number): void;
  /** A listen records: the recording ring, moving with `level` (0..1) while it is on. */
  listen(on: boolean): void;
  level(v: number): void;
  shake(): void;
  scene(layout: string, params?: Record<string, unknown>, ttlS?: number): boolean;
  sceneClear(): void;
  destroy(): void;
}

/** Start fetching what a screen will need (a screen waiting for its device to turn in). */
export function warmMoltyScreen(options: Pick<MoltyScreenOptions, "creature" | "scenes">) {
  for (const load of [
    loadModule(),
    options.creature ? loadFile(options.creature.url) : null,
    options.scenes ? loadFile(options.scenes) : null,
  ])
    load?.catch(() => {}); // the mount reports it
}

/** Mount a Molty on `canvas` (px x px pixels; CSS sizes it). Rejects when the module or a
 *  package cannot load: the caller keeps its still image. */
export async function mountMoltyScreen(
  canvas: HTMLCanvasElement,
  options: MoltyScreenOptions,
): Promise<MoltyScreenHandle> {
  try {
    const [module, creature, scenes] = await Promise.all([
      loadModule(),
      options.creature ? loadFile(options.creature.url) : null,
      options.scenes ? loadFile(options.scenes) : null,
    ]);
    const seed = (Math.random() * 0x100000000) >>> 0;
    const make = async () => {
      const core = await MoltyCore.create(module);
      if (creature && !core.creature(options.creature!.id, creature)) throw new Error("not a creature package");
      if (scenes && !core.pack(scenes)) throw new Error("not a scenes package");
      if (!core.start(seed, options.boot && !options.reducedMotion, options.px ?? PANEL)) throw new Error("no memory");
      return core;
    };
    const core = await make();
    // the window beside the screen: the same Molty (same seed, same calls, same frames), its
    // camera looking to the side
    const entering = options.entrance && !options.reducedMotion;
    const twin = entering && options.entrance!.outside ? await make() : null;
    const screen = new Screen(canvas, core, twin, options);
    pageState("live");
    return screen;
  } catch (e) {
    pageState("off");
    throw e;
  }
}

/** A scene held up is asked for again this long before the device would let it go (its ttl). */
const SCENE_TTL = 600;
const SCENE_AGAIN = 540;

const FINGER = ["pointerdown", "pointermove", "pointerup", "pointercancel", "pointerleave"];

class Screen implements MoltyScreenHandle {
  private ctx: CanvasRenderingContext2D;
  private px: number;
  private raf = 0;
  private last = 0;
  private seconds = 0;
  private inView = false;
  private alive = true;
  private watch: IntersectionObserver;
  private entering = false;
  private outside: CanvasRenderingContext2D | null = null;
  private sceneAt = 0;

  constructor(
    private canvas: HTMLCanvasElement,
    private core: MoltyCore,
    private twin: MoltyCore | null,
    private options: MoltyScreenOptions,
  ) {
    this.ctx = canvas.getContext("2d", { alpha: false })!;
    this.px = options.px ?? PANEL;
    if (options.scene) this.hold();
    if (options.entrance && !options.reducedMotion) this.enter();
    // straight to the creature (or the scene): it has formed by the first frame shown
    else if (!options.boot || options.reducedMotion) this.settle(options.scene ? 3.2 : SETTLE_S);
    else this.draw();
    this.watch = new IntersectionObserver((entries) => {
      this.inView = entries[entries.length - 1]!.isIntersecting;
      this.sync();
    });
    this.watch.observe(canvas);
    document.addEventListener("visibilitychange", this.sync);
    for (const type of FINGER) this.pointer.addEventListener(type, this.finger as EventListener);
  }

  /** The scene it holds up, (again) from now. */
  private hold() {
    const scene = this.options.scene!;
    this.core.scene(scene.layout, scene.params, SCENE_TTL);
    this.sceneAt = this.seconds;
  }

  /** The entrance: the creature is on screen after a few frames; then it is put outside and
   *  called, its first steps played unseen. */
  private enter() {
    const cores = this.twin ? [this.core, this.twin] : [this.core];
    this.twin?.pan(ENTER.pan);
    this.outside = this.options.entrance!.outside?.getContext("2d", { alpha: false }) ?? null;
    for (const c of cores) {
      for (let i = 0; i < 3; i++) c.frame(SETTLE_STEP_MS * 1000);
      this.entering = c.enter(ENTER.across, ENTER.back);
      for (let t = 0; t < ENTER.lead * 1000; t += SETTLE_STEP_MS) c.frame(SETTLE_STEP_MS * 1000);
    }
    this.draw();
  }

  /** Where the finger's events come from. */
  private get pointer(): EventTarget {
    return this.options.pageTaps ? window : this.canvas;
  }

  destroy() {
    this.alive = false;
    this.sync();
    this.watch.disconnect();
    document.removeEventListener("visibilitychange", this.sync);
    for (const type of FINGER) this.pointer.removeEventListener(type, this.finger as EventListener);
  }

  // ---------------------------------------------------------------- frames

  private sync = () => {
    const run = this.alive && this.inView && !document.hidden && !this.options.reducedMotion;
    if (run && !this.raf) {
      this.last = 0;
      this.raf = requestAnimationFrame(this.tick);
    } else if (!run && this.raf) {
      cancelAnimationFrame(this.raf);
      this.raf = 0;
    }
  };

  private tick = (now: number) => {
    this.raf = requestAnimationFrame(this.tick);
    const dt = this.last ? now - this.last : FRAME_MS;
    if (dt < FRAME_MS - 2) return; // a faster display: every other frame of it
    this.last = now;
    this.step(Math.min(dt, MAX_STEP_MS));
    this.draw();
    this.options.onFrame?.(this.seconds);
  };

  private step(ms: number) {
    const us = Math.round(ms * 1000 * (this.entering ? ENTER.rate : 1));
    this.core.frame(us);
    this.twin?.frame(us);
    this.seconds += ms / 1000;
    if (this.entering && !this.core.entering()) {
      // it sits: the window beside the screen is done with
      this.entering = false;
      this.twin = null;
      this.outside = null;
      this.options.onEntered?.();
    }
    if (this.options.scene && this.seconds - this.sceneAt > SCENE_AGAIN) this.hold();
    const answer = this.core.answer();
    if (answer >= 0) this.options.onAnswer?.(answer);
  }

  private draw() {
    this.ctx.putImageData(new ImageData(this.core.pixels() as Uint8ClampedArray<ArrayBuffer>, this.px, this.px), 0, 0);
    if (this.twin && this.outside)
      this.outside.putImageData(
        new ImageData(this.twin.pixels() as Uint8ClampedArray<ArrayBuffer>, this.px, this.px),
        0,
        0,
      );
  }

  /** Play `seconds` unseen, then show where it got to (reduced motion: one still per change). */
  private settle(seconds = SETTLE_S) {
    for (let t = 0; t < seconds * 1000; t += SETTLE_STEP_MS) this.step(SETTLE_STEP_MS);
    this.draw();
    this.options.onFrame?.(this.seconds);
  }
  private changed<T>(result: T): T {
    if (this.options.reducedMotion) this.settle();
    return result;
  }

  // ---------------------------------------------------------------- the finger

  private finger = (e: PointerEvent) => {
    const r = this.canvas.getBoundingClientRect();
    const on = e.clientX >= r.left && e.clientX <= r.right && e.clientY >= r.top && e.clientY <= r.bottom;
    const down = on && (e.type === "pointerdown" || (e.type === "pointermove" && e.buttons > 0 && e.pressure > 0));
    if (e.type === "pointermove" && !down) return;
    const x = ((e.clientX - r.left) / r.width) * this.px;
    const y = ((e.clientY - r.top) / r.height) * this.px;
    this.core.touch(down, x, y);
    this.twin?.touch(down, x, y); // the same Molty: the same finger
    if (down && e.type === "pointerdown") this.changed(null);
  };

  // ---------------------------------------------------------------- the calls

  state(body: Record<string, unknown>) {
    return this.changed(this.core.state(body));
  }
  read() {
    return this.core.read();
  }
  ask(q: MoltyQuestion) {
    // reduced motion: the question standing (it forms within FLUID_ASK_FORM_S, 3 s at the latest)
    const ok = this.core.ask(q);
    if (this.options.reducedMotion) this.settle(3.2);
    return ok;
  }
  askEnd() {
    this.changed(this.core.askEnd());
  }
  pick(index: number) {
    this.changed(this.core.pick(index));
  }
  listen(on: boolean) {
    this.changed(this.core.listen(on));
  }
  level(v: number) {
    this.core.level(v);
  }
  shake() {
    this.changed(this.core.shake());
  }
  scene(layout: string, params?: Record<string, unknown>, ttlS = 0) {
    const ok = this.core.scene(layout, params, ttlS);
    if (this.options.reducedMotion) this.settle(3.2);
    return ok;
  }
  sceneClear() {
    this.changed(this.core.sceneClear());
  }
}
