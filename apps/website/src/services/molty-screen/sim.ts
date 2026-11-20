// The simulator's device (/sim/): one instance of the firmware's renderer on a canvas, driven as
// the device is. The pointer is the finger, the device's own input handling is on (web_device.c:
// taps, double taps, the buttons, a shake, as sense.c does them), and what an agent would hear
// comes back as events. Two speeds:
//   browser   a frame each of the browser's (60 a second), the whole panel drawn each frame
//   device    the device's own draw-and-send path: only the windows it sends, the panel keeping
//             the rest, each frame shown as long as the device would take for it (the panel's
//             measured costs, web_device.c) and the firmware's clock moved by as much
// Nothing here draws or decides what Molty does: every call is the module's.
import {
  MoltyCore,
  PANEL,
  type DeviceEvent,
  type DeviceQuestion,
  type Exports,
  type FrameInfo,
  type LabSwitch,
} from "./core";
import { loadModule } from "./module";

export type SimSpeed = "browser" | "device";

/** The last few seconds of frames. */
export interface SimStats {
  speed: SimSpeed;
  fps: number;
  p50: number;
  p95: number;
  worst: number;
  /** The last frame's windows and pixels to the panel, and what the device takes for it. */
  last: FrameInfo;
}

export interface SimEvent extends DeviceEvent {
  /** Seconds since the device booted (its clock). */
  at: number;
}

export interface SimOptions {
  creature: { id: string; url: string };
  scenes: string;
  onEvent: (e: SimEvent) => void;
  onStats?: (s: SimStats) => void;
}

/** The module's simulator calls (device/firmware/web/web_device.c). */
interface SimExports extends Exports {
  wm_device(on: number): void;
  wm_frame_info(): number;
  wm_sense(on: number): void;
  wm_events(): number;
  wm_button(b: number, down: number): void;
  wm_shake_imu(): void;
  wm_talk(on: number): void;
  wm_dnd(on: number): void;
  wm_screen(on: number): void;
  wm_goodbye(): void;
  wm_progress(pct: number, done: number): void;
  wm_progress_stop(): void;
  wm_question(json: number): number;
  wm_question_end(reason: number): void;
  wm_scene_op(layout: number, params: number, ttl: number, queue: number): number;
  wm_lab_n(): number;
  wm_lab_name(i: number): number;
  wm_lab_range(i: number): number;
  wm_lab_set(i: number, v: number): void;
}

const text = new TextDecoder();

/** The calls the simulator adds to a Molty: the device's speed, its inputs, what it tells its agent. */
export class DeviceCalls {
  private x: SimExports;
  constructor(private core: MoltyCore) {
    this.x = core.raw as SimExports;
  }

  private cString(p: number): string {
    const mem = new Uint8Array(this.x.memory.buffer);
    return text.decode(mem.subarray(p, mem.indexOf(0, p)));
  }
  /** Device mode: only the windows the device sends, the panel keeping the rest, its clock at the
   *  device's pace. Off: the whole panel every frame. */
  device(on: boolean) {
    this.x.wm_device(on ? 1 : 0);
  }
  frameInfo(): FrameInfo {
    const f = new Float32Array(this.x.memory.buffer, this.x.wm_frame_info(), 10);
    return {
      windows: f[0],
      pixels: f[1],
      composed: f[2],
      wave: f[3] > 0,
      cellsMs: f[4],
      sendMs: f[5],
      drawMs: f[6],
      simMs: f[7],
      frameMs: f[8],
    };
  }
  /** The device's own handling of its inputs (sense.c): taps, double taps, buttons, a shake. */
  sense(on: boolean) {
    this.x.wm_sense(on ? 1 : 0);
  }
  /** What an agent would have heard since the last call, oldest first. */
  events(): DeviceEvent[] {
    const p = this.x.wm_events();
    if (!p) return [];
    const json = this.cString(p);
    this.x.wm_free(p);
    return JSON.parse(json) as DeviceEvent[];
  }
  /** A side button down or up: 0 accept, 1 reject. */
  button(b: 0 | 1, down: boolean) {
    this.x.wm_button(b, down ? 1 : 0);
  }
  /** The device is shaken: its IMU feels it. */
  shakeImu() {
    this.x.wm_shake_imu();
  }
  /** The agent's listen, and its end of turn. */
  talk(on: boolean) {
    this.x.wm_talk(on ? 1 : 0);
  }
  dnd(on: boolean) {
    this.x.wm_dnd(on ? 1 : 0);
  }
  screen(on: boolean) {
    this.x.wm_screen(on ? 1 : 0);
  }
  goodbye() {
    this.x.wm_goodbye();
  }
  progress(pct: number, done = false) {
    this.x.wm_progress(pct, done ? 1 : 0);
  }
  progressStop() {
    this.x.wm_progress_stop();
  }
  question(q: DeviceQuestion): boolean {
    return this.core.withText(JSON.stringify(q), (p) => this.x.wm_question(p) === 0);
  }
  questionEnd(reason = "cancelled") {
    this.core.withText(reason, (p) => this.x.wm_question_end(p));
  }
  /** scene.play, or scene.queue (`queue`). */
  sceneOp(layout: string, params: Record<string, unknown> | undefined, ttlS: number, queue: boolean): boolean {
    const play = (q: number) => this.core.withText(layout, (l) => this.x.wm_scene_op(l, q, ttlS, queue ? 1 : 0) === 0);
    return params ? this.core.withText(JSON.stringify(params), play) : play(0);
  }
  lab(): LabSwitch[] {
    const out: LabSwitch[] = [];
    for (let i = 0; i < this.x.wm_lab_n(); i++) {
      const r = new Int32Array(this.x.memory.buffer, this.x.wm_lab_range(i), 4);
      out.push({ index: i, name: this.cString(this.x.wm_lab_name(i)), value: r[0], def: r[1], lo: r[2], hi: r[3] });
    }
    return out;
  }
  labSet(index: number, value: number) {
    this.x.wm_lab_set(index, value);
  }
}

const STATS_S = 4;

async function file(url: string): Promise<Uint8Array> {
  const r = await fetch(url);
  if (!r.ok) throw new Error(`${url}: ${r.status}`);
  return new Uint8Array(await r.arrayBuffer());
}

export class MoltySim {
  core!: MoltyCore;
  /** The simulator's calls on that Molty. */
  dev!: DeviceCalls;
  private ctx: CanvasRenderingContext2D;
  private image: ImageData;
  private raf = 0;
  private speed: SimSpeed = "browser";
  private last = 0;
  private due = 0;
  private next = 30;
  private clock = 0;
  private intervals: Array<[number, number]> = [];
  private down = false;
  private lab = new Map<number, number>();

  private constructor(
    private canvas: HTMLCanvasElement,
    private module: WebAssembly.Module,
    private creature: Uint8Array,
    private scenes: Uint8Array,
    private options: SimOptions,
  ) {
    this.ctx = canvas.getContext("2d")!;
    this.image = this.ctx.createImageData(PANEL, PANEL);
    canvas.addEventListener("pointerdown", this.press);
    canvas.addEventListener("pointermove", this.move);
    canvas.addEventListener("pointerup", this.lift);
    canvas.addEventListener("pointercancel", this.lift);
  }

  static async create(canvas: HTMLCanvasElement, options: SimOptions): Promise<MoltySim> {
    const [module, creature, scenes] = await Promise.all([
      loadModule(),
      file(options.creature.url),
      file(options.scenes),
    ]);
    const sim = new MoltySim(canvas, module, creature, scenes, options);
    await sim.boot();
    return sim;
  }

  /** Power on (again): a new instance, its boot screen first. The speed and the lab's switches
   *  are set again (the device keeps none of them across a restart; the page does). */
  async boot() {
    cancelAnimationFrame(this.raf);
    const core = await MoltyCore.create(this.module);
    const dev = new DeviceCalls(core);
    if (!core.creature(this.options.creature.id, this.creature) || !core.pack(this.scenes))
      throw new Error("the packages did not load");
    dev.sense(true);
    if (!core.start((Math.random() * 2 ** 32) >>> 0, true)) throw new Error("the module did not start");
    dev.device(this.speed === "device");
    for (const [i, v] of this.lab) dev.labSet(i, v);
    this.core = core;
    this.dev = dev;
    this.clock = 0;
    this.intervals = [];
    this.last = this.due = performance.now();
    this.next = 30;
    this.raf = requestAnimationFrame(this.tick);
  }

  setSpeed(speed: SimSpeed) {
    this.speed = speed;
    this.dev.device(speed === "device");
    this.intervals = [];
    this.due = performance.now();
  }
  setLab(index: number, value: number) {
    this.lab.set(index, value);
    this.dev.labSet(index, value);
  }
  get seconds(): number {
    return this.clock;
  }

  destroy() {
    cancelAnimationFrame(this.raf);
    this.canvas.removeEventListener("pointerdown", this.press);
    this.canvas.removeEventListener("pointermove", this.move);
    this.canvas.removeEventListener("pointerup", this.lift);
    this.canvas.removeEventListener("pointercancel", this.lift);
  }

  /** Run n frames now, unseen (the e2e check moves the device on without waiting). */
  step(n: number) {
    for (let i = 0; i < n; i++) this.frame(this.speed === "device" ? this.next : 1000 / 60);
    this.paint();
  }

  private at(e: PointerEvent): [number, number] {
    const r = this.canvas.getBoundingClientRect();
    return [((e.clientX - r.left) * PANEL) / r.width, ((e.clientY - r.top) * PANEL) / r.height];
  }
  private press = (e: PointerEvent) => {
    this.down = true;
    this.canvas.setPointerCapture(e.pointerId);
    this.core.touch(true, ...this.at(e));
  };
  private move = (e: PointerEvent) => {
    if (this.down) this.core.touch(true, ...this.at(e));
  };
  private lift = () => {
    this.down = false;
    this.core.touch(false);
  };

  /** One frame, ms after the last on the device's clock. */
  private frame(ms: number) {
    this.core.frame(Math.round(ms * 1000));
    this.clock += ms / 1000;
    const info = this.dev.frameInfo();
    this.next = info.frameMs;
    for (const e of this.dev.events()) this.options.onEvent({ ...e, at: this.clock });
    return info;
  }
  private paint() {
    this.image.data.set(this.core.pixels());
    this.ctx.putImageData(this.image, 0, 0);
  }

  private tick = (now: number) => {
    this.raf = requestAnimationFrame(this.tick);
    let info: FrameInfo | null = null;
    let interval = 0;
    if (this.speed === "browser") {
      if (now - this.last < 1000 / 60 - 2) return; // at most 60 a second, as the page's other screens
      interval = Math.min(100, now - this.last);
      info = this.frame(interval);
    } else if (now >= this.due) {
      // the device's time for the last frame passes before the next shows; a tab that slept starts afresh
      if (now - this.due > 250) this.due = now;
      interval = this.next;
      info = this.frame(interval);
      this.due += interval;
    }
    this.last = now;
    if (!info) return;
    this.paint();
    this.intervals.push([this.clock, interval]);
    while (this.intervals.length && this.intervals[0][0] < this.clock - STATS_S) this.intervals.shift();
    const sorted = this.intervals.map(([, v]) => v).sort((a, b) => a - b);
    const q = (p: number) => sorted[Math.min(sorted.length - 1, Math.floor(p * (sorted.length - 1) + 0.5))] ?? 0;
    const sum = sorted.reduce((a, b) => a + b, 0);
    this.options.onStats?.({
      speed: this.speed,
      fps: sum ? (1000 * sorted.length) / sum : 0,
      p50: q(0.5),
      p95: q(0.95),
      worst: sorted.at(-1) ?? 0,
      last: info,
    });
  };
}

/** A device on `canvas` (PANEL x PANEL pixels). */
export function mountMoltySim(canvas: HTMLCanvasElement, options: SimOptions): Promise<MoltySim> {
  return MoltySim.create(canvas, options);
}
