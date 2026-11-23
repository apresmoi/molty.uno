// A round screen on the page that runs Molty as the device does: one instance of the firmware's
// renderer (core.ts) drawing into a canvas, the pointer as the finger on the glass. The module
// and the packages are fetched once, when the first screen mounts, never with the page itself.
//
//   frames     the page's (clock.ts), only while the screen is in view and the tab is shown; the
//              clock the firmware sees stops with them
//   water      one with the page's (water.ts): before each frame the screen takes the page's
//              water at its rim, so a wave from outside comes in and its own goes out
//   reduced    prefers-reduced-motion: no frames; one settled still per change
//   <html data-fluid-moltys>   "live" once a screen draws, "off" when the module cannot load
//              (the page then keeps its still images)
import { onFrame } from "./clock";
import { MoltyCore, PANEL, type MoltyQuestion, type MoltyState } from "./core";
import { ENTER, Entrance, play } from "./entrance";
import { WATER } from "./field";
import { loadModule } from "./module";
import type { WordIntro } from "./intro";
import { pageWater, type PageWater } from "./water";
import { HEADING_GAIN } from "../../config/water.mjs";

/** Reduced motion and a screen that starts on its creature: this much is played unseen first. */
const SETTLE_S = 1.6;
const SETTLE_STEP_MS = 1000 / 30;
/** One water with the page: a screen's water goes to the page's own from this far out of the
 *  glass's radius, wholly from that far (its outer band, where the device's water has its
 *  sponge: fluid_sim.c). */
const RIM = [0.74, 0.86] as const;
/** The fastest the firmware's water goes (its c2: cells a step, squared) and stays itself. */
const C2_MAX = 0.6;
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
  /** The scene forms in view instead of unseen (intro.ts; not with reduced motion). */
  intro?: WordIntro;
  /** A heading's window: its scene is on the braille haze (the page's own grain), all of its
   *  water is the page's, and it takes no finger (a tap there is the page's). */
  heading?: boolean;
  /** The creature walks in from beyond the window's right edge. `outside`: a canvas the size and
   *  place of the screen's own, clear but for the cat: this moves it along with the cat while
   *  the cat is outside the glass (the same renderer's picture of the cat alone). */
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
      if (options.heading) core.haze();
      core.time(Date.now() / 1000); // the browser's date: today's moon behind the cat
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
/** The browser's date given to the module again after this long (its clock runs with the frames). */
const TIME_AGAIN_MS = 60_000;

const FINGER = ["pointerdown", "pointermove", "pointerup", "pointercancel", "pointerleave"] as const;

class Screen implements MoltyScreenHandle {
  private ctx: CanvasRenderingContext2D;
  private px: number;
  private stop: (() => void) | null = null;
  private seconds = 0;
  /** The page's water this screen's water was last given the numbers of, and those. */
  private water: PageWater | null = null;
  private waterAs = "";
  private inView = false;
  private alive = true;
  private watch: IntersectionObserver;
  /** The cat walking in, until it sits. */
  private walk: Entrance | null = null;
  private outside: CanvasRenderingContext2D | null = null;
  private sceneAt = 0;
  private timeAt = Date.now();
  /** The word forming in view, its seconds so far, and its water a frame ago (cells' h). */
  private intro: WordIntro | null = null;
  private introT = 0;
  private introH: Float32Array | null = null;

  constructor(
    private canvas: HTMLCanvasElement,
    private core: MoltyCore,
    twin: MoltyCore | null,
    private options: MoltyScreenOptions,
  ) {
    this.ctx = canvas.getContext("2d", { alpha: false })!;
    this.px = options.px ?? PANEL;
    if (options.scene) this.hold();
    if (options.entrance && !options.reducedMotion) this.enter(twin);
    else if (options.intro && !options.reducedMotion) {
      this.intro = options.intro;
      if (options.intro.water) core.couple(0, 0);
      this.draw();
    }
    // straight to the creature (or the scene): it has formed by the first frame shown
    else if (!options.boot || options.reducedMotion) this.settle(options.scene ? 3.2 : SETTLE_S);
    else this.draw();
    this.watch = new IntersectionObserver((entries) => {
      this.inView = entries[entries.length - 1]!.isIntersecting;
      this.sync();
    });
    this.watch.observe(canvas);
    document.addEventListener("visibilitychange", this.sync);
    if (!options.heading) for (const type of FINGER) canvas.addEventListener(type, this.finger);
  }

  /** The scene it holds up, (again) from now. */
  private hold() {
    const scene = this.options.scene!;
    this.core.scene(scene.layout, scene.params, SCENE_TTL);
    this.sceneAt = this.seconds;
  }

  /** The entrance (entrance.ts), from beyond the window's right edge. While the cat walks in,
   *  the window beside the screen looks where the cat is: its picture of the cat alone is the one
   *  cat the page shows, outside the glass and inside it. */
  private enter(twin: MoltyCore | null) {
    this.outside = this.options.entrance!.outside?.getContext("2d") ?? null;
    const box = this.canvas.getBoundingClientRect();
    const aside = ((innerWidth - (box.left + box.width / 2)) / box.width) * ENTER.view;
    const walk = new Entrance(this.core, twin, aside);
    if (walk.walking) this.walk = walk;
    this.draw();
  }

  destroy() {
    this.alive = false;
    this.sync();
    this.watch.disconnect();
    document.removeEventListener("visibilitychange", this.sync);
    for (const type of FINGER) this.canvas.removeEventListener(type, this.finger);
  }

  // ---------------------------------------------------------------- frames

  private sync = () => {
    const run = this.alive && this.inView && !document.hidden && !this.options.reducedMotion;
    if (run && !this.stop) this.stop = onFrame(this.tick);
    else if (!run && this.stop) {
      this.stop();
      this.stop = null;
    }
  };

  /** A heading takes every other of the page's frames while the page's water moves (about the
   *  device's own pace: its window is four panels' worth of pixels), and one in six while it is
   *  flat (nothing of it moves then but its dust's slow shimmer). Headings take turns. */
  private static headings = 0;
  private turn = Screen.headings++;
  private owed = 0;

  private tick = (ms: number) => {
    const intro = this.intro;
    if (intro) {
      this.introT += ms / 1000;
      if (this.introT >= intro.seconds) this.intro = this.introH = null;
    } else if (this.options.heading) {
      this.owed += ms;
      if (++this.turn % (pageWater()?.moving ? 2 : 6)) return;
      ms = this.owed;
      this.owed = 0;
    }
    this.meetPage(intro);
    this.step(intro?.clock ? ms * intro.clock(this.introT) : ms);
    this.draw();
    this.options.onFrame?.(this.seconds);
  };

  /** One water: the screen's water has the page's numbers (its waves the page's speed on screen,
   *  whatever size the screen is shown at), and takes the page's water where the two meet: at
   *  its rim, or all over (a heading's window). */
  private meetPage(intro: WordIntro | null) {
    const water = pageWater();
    const own = intro?.water ? this.introWater(intro) : null;
    if (!water && !this.water && !own) return;
    const box = this.canvas.getBoundingClientRect();
    const as = water ? `${box.width} ${water.leak} ${water.damp}` : "";
    if (water !== this.water || as !== this.waterAs) {
      if (water && !this.water) this.core.couple(this.options.heading ? 0 : RIM[0], this.options.heading ? 0 : RIM[1]);
      this.water = water;
      this.waterAs = as;
      const cells = (water?.speed ?? 0) / (box.width / this.core.gridW);
      if (water) this.core.water(Math.min(C2_MAX, cells * cells), water.leak, water.damp);
      else this.core.water(WATER.c2, WATER.leak, WATER.damp);
    }
    const gain = this.options.heading ? HEADING_GAIN : 1;
    const under = water?.under(this.core.cells(), box, this.px) ?? null;
    if (own) {
      if (under) for (let i = 0; i < own.length; i++) own[i]! += under[i]! * gain;
      this.core.coupled(own, 1);
    } else if (water) this.core.coupled(under, gain);
  }

  /** The intro's water under the screen's cells (its height now and a frame ago each), easing to
   *  flat over its last 0.3 s so the word ends on calm water. */
  private introWater({ water: height, seconds }: WordIntro): Float32Array {
    const xy = this.core.cells();
    const n = xy.length / 2;
    const t = this.introT;
    const fade = 1 - Math.min(1, Math.max(0, (t - seconds + 0.3) / 0.3));
    const out = new Float32Array(xy.length);
    const was = this.introH;
    const h = (this.introH = new Float32Array(n));
    for (let i = 0; i < n; i++) {
      h[i] = fade * height!((xy[2 * i]! / this.px) * 2 - 1, (xy[2 * i + 1]! / this.px) * 2 - 1, t);
      out[2 * i] = h[i]!;
      out[2 * i + 1] = was ? was[i]! : h[i]!;
    }
    return out;
  }

  private step(ms: number) {
    this.seconds += ms / 1000;
    // the module's clock runs with its frames, which stop while the screen is unseen: the date again now and then
    if (Date.now() - this.timeAt > TIME_AGAIN_MS) {
      this.timeAt = Date.now();
      this.core.time(this.timeAt / 1000);
    }
    if (!this.walk) play(Math.round(ms * 1000), (us) => this.core.frame(us));
    else if (!this.walk.step(ms)) {
      // it sits: the window beside the screen is done with, the screen's own cat takes over
      this.walk = null;
      this.outside?.clearRect(0, 0, this.px, this.px);
      this.outside = null;
      this.options.onEntered?.();
    }
    if (this.options.scene && this.seconds - this.sceneAt > SCENE_AGAIN) this.hold();
    const answer = this.core.answer();
    if (answer >= 0) this.options.onAnswer?.(answer);
  }

  private draw() {
    const picture = (px: Uint8ClampedArray) => new ImageData(px as Uint8ClampedArray<ArrayBuffer>, this.px, this.px);
    const cat = this.outside && this.walk?.twin?.catPixels();
    if (!cat) return this.ctx.putImageData(picture(this.core.pixels()), 0, 0);
    // The cat walking in is one picture, the second window's, from one frame and with one shift:
    // on the clear canvas carried to where the cat stands (what of it is outside the glass shows
    // there, under the bezel), and drawn from that same canvas onto the screen's own picture of
    // the world without its cat (what of it is inside the glass).
    const outside = this.outside!;
    const aside = this.walk!.aside;
    outside.canvas.style.translate = `${(aside / this.px) * this.canvas.getBoundingClientRect().width}px 0`;
    outside.putImageData(picture(cat), 0, 0);
    this.ctx.putImageData(picture(this.core.worldPixels()), 0, 0);
    this.ctx.drawImage(outside.canvas, aside, 0);
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
    this.walk?.twin?.touch(down, x, y); // the same Molty: the same finger
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
