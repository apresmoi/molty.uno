// Molty's screen as the device draws it: the firmware's own renderer (the fluid, the creature
// player and its crisp dots, the rings, the question, the scene player) compiled to WebAssembly
// from device/firmware/main by device/firmware/web. This file is only the module's loader and
// its calls, with no DOM in it: the page (screen.ts) and the fidelity check
// (tools/web-molty/fidelity.mjs, under Node) drive the same class.

/** The panel's side in pixels: the Waveshare AMOLED 1.75C's 466 px round screen. */
export const PANEL = 466;

interface Exports {
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

  /** The module needs a clock and somewhere to lose its log; nothing else of a system. */
  static async create(module: WebAssembly.Module): Promise<MoltyCore> {
    let x: Exports;
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
    x = instance.exports as unknown as Exports;
    x._initialize();
    return new MoltyCore(x);
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
}
