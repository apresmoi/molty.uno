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

export interface MoltyScreenOptions {
  /** The creature package: its id and its file's URL (packages/<id>/<id>.json). */
  creature: { id: string; url: string };
  /** A scenes package's file, for `scene`. */
  scenes?: string;
  /** Start with the device's boot screen ("molty.uno"), else on the creature. */
  boot: boolean;
  reducedMotion: boolean;
  /** A tap (or `pick`) answered the question on screen with this option. */
  onAnswer?: (index: number) => void;
  /** Every frame drawn, with the seconds this screen has run (they stop while it is unseen). */
  onFrame?: (seconds: number) => void;
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
  for (const load of [loadModule(), loadFile(options.creature.url), options.scenes ? loadFile(options.scenes) : null])
    load?.catch(() => {}); // the mount reports it
}

/** Mount a Molty on `canvas` (PANEL x PANEL pixels; CSS sizes it). Rejects when the module or a
 *  package cannot load: the caller keeps its still image. */
export async function mountMoltyScreen(
  canvas: HTMLCanvasElement,
  options: MoltyScreenOptions,
): Promise<MoltyScreenHandle> {
  try {
    const [module, creature, scenes] = await Promise.all([
      loadModule(),
      loadFile(options.creature.url),
      options.scenes ? loadFile(options.scenes) : null,
    ]);
    const core = await MoltyCore.create(module);
    if (!core.creature(options.creature.id, creature)) throw new Error("not a creature package");
    if (scenes && !core.pack(scenes)) throw new Error("not a scenes package");
    if (!core.start((Math.random() * 0x100000000) >>> 0, options.boot && !options.reducedMotion))
      throw new Error("no memory");
    const screen = new Screen(canvas, core, options);
    pageState("live");
    return screen;
  } catch (e) {
    pageState("off");
    throw e;
  }
}

class Screen implements MoltyScreenHandle {
  private ctx: CanvasRenderingContext2D;
  private raf = 0;
  private last = 0;
  private seconds = 0;
  private inView = false;
  private alive = true;
  private watch: IntersectionObserver;

  constructor(
    private canvas: HTMLCanvasElement,
    private core: MoltyCore,
    private options: MoltyScreenOptions,
  ) {
    this.ctx = canvas.getContext("2d", { alpha: false })!;
    // straight to the creature: it has formed by the first frame shown
    if (!options.boot || options.reducedMotion) this.settle();
    else this.draw();
    this.watch = new IntersectionObserver((entries) => {
      this.inView = entries[entries.length - 1]!.isIntersecting;
      this.sync();
    });
    this.watch.observe(canvas);
    document.addEventListener("visibilitychange", this.sync);
    canvas.addEventListener("pointerdown", this.finger);
    canvas.addEventListener("pointermove", this.finger);
    canvas.addEventListener("pointerup", this.finger);
    canvas.addEventListener("pointercancel", this.finger);
    canvas.addEventListener("pointerleave", this.finger);
  }

  destroy() {
    this.alive = false;
    this.sync();
    this.watch.disconnect();
    document.removeEventListener("visibilitychange", this.sync);
    for (const type of ["pointerdown", "pointermove", "pointerup", "pointercancel", "pointerleave"])
      this.canvas.removeEventListener(type, this.finger as EventListener);
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
    this.core.frame(Math.round(ms * 1000));
    this.seconds += ms / 1000;
    const answer = this.core.answer();
    if (answer >= 0) this.options.onAnswer?.(answer);
  }

  private draw() {
    this.ctx.putImageData(new ImageData(this.core.pixels() as Uint8ClampedArray<ArrayBuffer>, PANEL, PANEL), 0, 0);
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
    const down = e.type === "pointerdown" || (e.type === "pointermove" && e.buttons > 0 && e.pressure > 0);
    if (e.type === "pointermove" && !down) return;
    const r = this.canvas.getBoundingClientRect();
    this.core.touch(down, ((e.clientX - r.left) / r.width) * PANEL, ((e.clientY - r.top) / r.height) * PANEL);
    if (e.type === "pointerdown") this.changed(null);
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
