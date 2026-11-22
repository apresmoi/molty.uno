// Molty's screen as the device draws it: the firmware's own renderer (the fluid, the creature
// player and its crisp dots, the rings, the question, the scene player) compiled to WebAssembly
// from device/firmware/main by device/firmware/web. This file is only the module's loader and
// its calls, with no DOM in it: the page (screen.ts) and the fidelity check
// (tools/web-molty/fidelity.mjs, under Node) drive the same class.

/** The panel's side in pixels: the Waveshare AMOLED 1.75C's 466 px round screen. */
export const PANEL = 466;

/** What the module needs of a system: a clock and somewhere to lose its log. */
export async function instantiate<X extends { memory: WebAssembly.Memory; _initialize(): void }>(
  module: WebAssembly.Module,
): Promise<X> {
  let x: X;
  const view = () => new DataView(x.memory.buffer);
  const instance = await WebAssembly.instantiate(module, {
    env: { emscripten_notify_memory_growth: () => {} },
    wasi_snapshot_preview1: {
      clock_time_get: (_id: number, _precision: bigint, out: number) => {
        view().setBigUint64(out, BigInt(Math.round(performance.now() * 1e6)), true);
        return 0;
      },
      fd_write: (_fd: number, iov: number, n: number, written: number) => {
        const v = view();
        let total = 0;
        for (let i = 0; i < n; i++) total += v.getUint32(iov + i * 8 + 4, true);
        v.setUint32(written, total, true);
        return 0;
      },
      fd_read: () => 8,
      fd_close: () => 0,
      fd_seek: () => 70,
    },
  });
  x = instance.exports as unknown as X;
  x._initialize();
  return x;
}

export interface Exports {
  memory: WebAssembly.Memory;
  _initialize(): void;
  wm_alloc(n: number): number;
  wm_free(p: number): void;
  wm_creature(id: number, json: number, len: number): number;
  wm_start(px: number, seed: number, boot: number): number;
  wm_frame(dtUs: number): void;
  wm_pixels(): number;
  wm_touch(down: number, x: number, y: number): void;
  wm_shake(): void;
  wm_enter(across: number, back: number): number;
  wm_entering(): number;
  wm_pan(across: number): number;
  wm_across(): number;
  wm_back(): number;
  wm_cat_pixels(): number;
  wm_world_pixels(): number;
  wm_haze(): void;
  wm_couple(r0: number, r1: number): number;
  wm_couple_xy(): number;
  wm_couple_in(): number;
  wm_couple_set(have: number, gain: number): void;
  wm_water(c2: number, leak: number, damp: number): void;
  wm_grid_w(): number;
  wm_grid_h(): number;
  wm_h(): number;
  wm_state(json: number): number;
  wm_state_json(): number;
  wm_listen(on: number): void;
  wm_listen_level(level: number): void;
  wm_ask(json: number): number;
  wm_ask_end(): void;
  wm_answer(): number;
  wm_pick(i: number): void;
  wm_package(json: number, len: number): number;
  wm_scene(layout: number, params: number, ttl: number): number;
  wm_scene_clear(): void;
  wm_time(unixS: number): void;
  wm_time_now(): number;
}

/** The last frame (web_device.c wm_frame_info): what went to the panel and what the device would
 *  have taken for it, on the panel's measured constants (ms). */
export interface FrameInfo {
  windows: number;
  pixels: number;
  /** A change of picture's composed pixels. */
  composed: number;
  wave: boolean;
  cellsMs: number;
  sendMs: number;
  drawMs: number;
  simMs: number;
  /** From this frame to the next on the device. */
  frameMs: number;
}

/** One of the firmware's experiment switches (web_device.c LAB). */
export interface LabSwitch {
  index: number;
  name: string;
  value: number;
  def: number;
  lo: number;
  hi: number;
}

/** A frame the device sends its agent ({"t":"evt","name":...,"data":{...}}), or the relay's own. */
export interface DeviceEvent {
  t: string;
  name?: string;
  data?: Record<string, unknown>;
  [key: string]: unknown;
}

/** A question with what its answer does: an agent's (an `answer` event), a pairing, an install. */
export interface DeviceQuestion extends MoltyQuestion {
  kind: "ask" | "pair" | "install";
  job: string;
  /** An install: the package, its version and how long its download takes (ms). */
  id?: string;
  version?: string;
  ms?: number;
}

/** A question on the glass: the link's `ask`, with the pairing question's detail and code. */
export interface MoltyQuestion {
  title?: string;
  detail?: string;
  code?: string;
  options: string[];
  colors?: string[];
}

/** GET /api/creature/state on the device: the parts the page reads. */
export interface MoltyState {
  /** "creature" | "text" | "scene" | ... */
  state: string;
  mood: string;
  /** The gesture playing, "" for none. */
  act: string;
  /** The reader: the word it is on, of how many. */
  word_idx: number;
  nwords: number;
  /** A rigged creature's dots on screen (-1: none shows). */
  crisp_dots: number;
  scene?: { pkg: string; layout: string };
  creature_info: { name: string; package: string; gestures?: Array<{ name: string }>; moods?: string[] };
}

const utf8 = new TextEncoder();
const text = new TextDecoder();

/** One Molty: one instance of the module (the firmware keeps its state in statics, as on the chip). */
export class MoltyCore {
  private x: Exports;
  private px = 0;

  private constructor(x: Exports) {
    this.x = x;
  }

  static async create(module: WebAssembly.Module): Promise<MoltyCore> {
    return new MoltyCore(await instantiate<Exports>(module));
  }

  /** `bytes` (and a closing zero) in the module's memory for the length of `call`. */
  private lend<T>(bytes: Uint8Array, call: (p: number, len: number) => T): T {
    const p = this.x.wm_alloc(bytes.length + 1);
    const mem = new Uint8Array(this.x.memory.buffer);
    mem.set(bytes, p);
    mem[p + bytes.length] = 0;
    try {
      return call(p, bytes.length);
    } finally {
      this.x.wm_free(p);
    }
  }
  private lendText<T>(s: string, call: (p: number) => T): T {
    return this.lend(utf8.encode(s), call);
  }

  /** A creature package's file (packages/<id>/<id>.json): the creature a start shows, or from now on. */
  creature(id: string, file: Uint8Array): boolean {
    return this.lendText(id, (i) => this.lend(file, (p, len) => this.x.wm_creature(i, p, len) === 0));
  }

  /** Power on. `boot`: the device's boot screen first, else straight to the creature. */
  start(seed: number, boot: boolean, px = PANEL): boolean {
    this.px = px;
    return this.x.wm_start(px, seed >>> 0, boot ? 1 : 0) === 0;
  }

  /** The time now (unix seconds, UTC: Date.now() / 1000), for the sky behind the cat (today's moon, the falling
   *  stars); the module runs it on with its frames. Never given: no sky (the frames as ever). */
  time(unixS: number) {
    this.x.wm_time(unixS);
  }
  /** The time as the module has it now (0: not known). */
  timeNow(): number {
    return this.x.wm_time_now();
  }

  /** One frame, `dtUs` microseconds after the last. */
  frame(dtUs: number) {
    this.x.wm_frame(dtUs);
  }

  /** What the panel shows now: px * px RGBA pixels, a view of the module's memory (read it before the next call). */
  pixels(): Uint8ClampedArray {
    return new Uint8ClampedArray(this.x.memory.buffer, this.x.wm_pixels(), this.px * this.px * 4);
  }

  /** The finger, in panel pixels. */
  touch(down: boolean, x = 0, y = 0) {
    this.x.wm_touch(down ? 1 : 0, Math.round(x), Math.round(y));
  }

  shake() {
    this.x.wm_shake();
  }

  /** The website's entrance: the creature is put outside the frame (world units across and back)
   *  and called, as a tap calls it; it walks in with its own clips and sits. */
  enter(across: number, back: number): boolean {
    return this.x.wm_enter(across, back) === 0;
  }
  /** It is still on its way in. */
  entering(): boolean {
    return this.x.wm_entering() !== 0;
  }
  /** Where the creature stands, across the frame from its middle (world units). */
  across(): number {
    return this.x.wm_across();
  }
  /** And how far back from the viewer (world units). */
  back(): number {
    return this.x.wm_back();
  }
  /** The creature alone, as the panel shows it (everything else clear): px * px RGBA, a view of
   *  the module's memory, or null without one. */
  catPixels(): Uint8ClampedArray | null {
    const p = this.x.wm_cat_pixels();
    return p ? new Uint8ClampedArray(this.x.memory.buffer, p, this.px * this.px * 4) : null;
  }
  /** Scenes on the braille haze from now on (what a board without the fine dust's memory shows):
   *  their dust is the lattice's own dots, a steep wave loosens their lines into it and calm
   *  water sharpens them back. A heading on the page is one. Before the first frame. */
  haze() {
    this.x.wm_haze();
  }
  /** The grid's width in cells (a 466 px panel: 59). */
  get gridW(): number {
    return this.x.wm_grid_w();
  }
  get gridH(): number {
    return this.x.wm_grid_h();
  }
  /** The water's height, a view of the module's memory: gridW x gridH cells, 1024 a unit. */
  height(): Int16Array {
    return new Int16Array(this.x.memory.buffer, this.x.wm_h(), this.gridW * this.gridH);
  }
  /** The water's own numbers (fluid_sim.h fluid_params_t): c2, its speed squared in cells a
   *  step; what a step keeps of a wave's height (leak) and of its motion (damp). */
  water(c2: number, leak: number, damp: number) {
    this.x.wm_water(c2, leak, damp);
  }
  /** One water with the page: from `r0` of the glass's radius out, the screen's water goes to the
   *  page's own there, wholly from `r1` out (0, 0: every cell: a heading's window). */
  couple(r0: number, r1: number) {
    this.coupledCells = this.x.wm_couple(r0, r1);
  }
  private coupledCells = 0;
  /** Where those cells are, (x, y) panel pixels each: a view of the module's memory. */
  cells(): Float32Array {
    return new Float32Array(this.x.memory.buffer, this.x.wm_couple_xy(), this.coupledCells * 2);
  }
  /** Before a frame: the page's water under those cells (its height now and a step ago each),
   *  or null where the page's water is flat; taken `gain` times over. */
  coupled(water: Float32Array | null, gain = 1) {
    if (water) new Float32Array(this.x.memory.buffer, this.x.wm_couple_in(), water.length).set(water);
    this.x.wm_couple_set(water ? 1 : 0, gain);
  }
  /** The panel without the creature (its cells clear to black): with `catPixels` of a window that
   *  follows the creature laid over it, the creature is one picture on both sides of the rim. */
  worldPixels(): Uint8ClampedArray {
    return new Uint8ClampedArray(this.x.memory.buffer, this.x.wm_world_pixels(), this.px * this.px * 4);
  }
  /** The picture's camera looks to the side, by the whole cells nearest `across` (world units): a
   *  window beside the screen. Returns how far that is in panel pixels: the window's picture
   *  carried that far falls on the screen's own lattice. */
  pan(across: number): number {
    return this.x.wm_pan(across);
  }

  /** POST /api/creature/state's body, as on the device: { mood }, { act }, { state, text }, ... */
  state(body: Record<string, unknown>): boolean {
    return this.lendText(JSON.stringify(body), (p) => this.x.wm_state(p) === 0);
  }

  /** GET /api/creature/state. */
  read(): MoltyState | null {
    const p = this.x.wm_state_json();
    if (!p) return null;
    const mem = new Uint8Array(this.x.memory.buffer);
    const json = text.decode(mem.subarray(p, mem.indexOf(0, p)));
    this.x.wm_free(p);
    return JSON.parse(json) as MoltyState;
  }

  /** A listen records (the recording ring), with the mic's level 0..1 while it does. */
  listen(on: boolean) {
    this.x.wm_listen(on ? 1 : 0);
  }
  level(v: number) {
    this.x.wm_listen_level(v);
  }

  ask(q: MoltyQuestion): boolean {
    return this.lendText(JSON.stringify(q), (p) => this.x.wm_ask(p) === 0);
  }
  askEnd() {
    this.x.wm_ask_end();
  }
  /** Answer the question with option `i` without a tap on the glass (a key, a button). */
  pick(i: number) {
    this.x.wm_pick(i);
  }
  /** The option the question was answered with, once; -1: none (yet). */
  answer(): number {
    return this.x.wm_answer();
  }

  /** A scenes package's file: the one `scene` plays from. */
  pack(file: Uint8Array): boolean {
    return this.lend(file, (p, len) => this.x.wm_package(p, len) === 0);
  }
  /** scene.play: a layout of the package in hand, with its params. */
  scene(layout: string, params?: Record<string, unknown>, ttlS = 0): boolean {
    const play = (q: number) => this.lendText(layout, (l) => this.x.wm_scene(l, q, ttlS) === 0);
    return params ? this.lendText(JSON.stringify(params), play) : play(0);
  }
  sceneClear() {
    this.x.wm_scene_clear();
  }

  /** The module itself, for the simulator's own calls (sim.ts): its exports and memory. */
  get raw(): Exports {
    return this.x;
  }
  /** `s` (and a closing zero) in the module's memory for the length of `call`. */
  withText<T>(s: string, call: (p: number) => T): T {
    return this.lendText(s, call);
  }
}
