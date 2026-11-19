// The page's water: the firmware's own water (the wave of fluid_sim.c) and its own drawer of
// crisp dots (fluid_draw.c), on a grid as wide as the window, from one more instance of the
// module (device/firmware/web/web_field.c, which also says where the page's dust is: that part
// is the website's). This file is only its calls, with no DOM in it: the page (services/
// page-water), the dust tiles' script and the tests drive the same class.

/** A cell of the field, pixels: the canvas a field draws into is `cells * CELL` of them. */
export const CELL = { w: 8, h: 13 };
/** The dust repeats every this many pixels, both ways: a tile's side. */
export const TILE = 1040;
/** The device's water (fluid_sim.c fluid_params_default): its speed squared in cells a step, on
 *  a grid `cells` wide over a `panel` px screen; what a step keeps of a wave's height and of
 *  its motion; and a finger's tap (mole_fluid.c): how high, how wide in the glass's radii. */
export const WATER = { c2: 0.11, cells: 59, panel: 466, leak: 0.985, damp: 0.975, tap: 1.6, tapRadius: 0.09 };

export interface FieldExports {
  memory: WebAssembly.Memory;
  _initialize(): void;
  wm_alloc(n: number): number;
  wm_free(p: number): void;
  wf_open(ww: number, wh: number, bg: number, paper: number, c2: number, leak: number, damp: number): number;
  wf_close(): void;
  wf_place(pi: number, pj: number): void;
  wf_focus(f: number, n: number): void;
  wf_tap(x: number, y: number, strength: number, r: number): void;
  wf_step(dt: number): number;
  wf_draw(): number;
  wf_rects(): number;
  wf_pixels(): number;
  wf_picture(ambient: number): number;
  wf_sample(xy: number, n: number, ox: number, oy: number, scale: number, out: number): void;
  wf_grid_w(): number;
  wf_grid_h(): number;
  wf_window_i(): number;
  wf_window_j(): number;
  wf_h(): number;
}

/** What focuses the page's dust: a centre and radii (the field's pixels, on the page), and how
 *  much in focus (0..1) at up to four distances (0..1 of the radius) from the centre out. */
export interface Focus {
  x: number;
  y: number;
  rx: number;
  ry: number;
  stops: ReadonlyArray<readonly [number, number]>;
}

export interface FieldOptions {
  /** The window, in cells. */
  cols: number;
  rows: number;
  /** The page's colour under the dust, 0xRRGGBB, and whether it is paper (the dust is ink). */
  background: number;
  paper: boolean;
  /** The water's numbers (fluid_params_t). */
  c2: number;
  leak: number;
  damp: number;
}

/** One field (one instance of the module). All places are the field's pixels on the page. */
export class MoltyField {
  private x: FieldExports;
  readonly width: number;
  readonly height: number;
  /** Scratch in the module's memory for `sample`. */
  private lent = 0;
  private lentSize = 0;

  private constructor(x: FieldExports, o: FieldOptions) {
    this.x = x;
    this.width = o.cols * CELL.w;
    this.height = o.rows * CELL.h;
  }

  /** On a fresh instance of the module (core.ts `instantiate`). Null when the window is wider
   *  than the largest field. */
  static open(x: FieldExports, o: FieldOptions): MoltyField | null {
    return x.wf_open(o.cols, o.rows, o.background, o.paper ? 1 : 0, o.c2, o.leak, o.damp) === 0
      ? new MoltyField(x, o)
      : null;
  }

  private floats(n: number): number {
    if (n > this.lentSize) {
      if (this.lent) this.x.wm_free(this.lent);
      this.lent = this.x.wm_alloc(n * 4);
      this.lentSize = n;
    }
    return this.lent;
  }

  /** The page's cell (column, row) under the canvas's first one. */
  place(col: number, row: number) {
    this.x.wf_place(col, row);
  }
  focus(foci: Focus[]) {
    const p = this.floats(foci.length * 12);
    const f = new Float32Array(this.x.memory.buffer, p, foci.length * 12);
    foci.forEach((c, i) => {
      f.set([c.x, c.y, c.rx, c.ry], i * 12);
      for (let s = 0; s < 4; s++) f.set(c.stops[Math.min(s, c.stops.length - 1)]!, i * 12 + 4 + s * 2);
    });
    this.x.wf_focus(p, foci.length);
  }
  /** A tap: a bump `strength` high, `radius` px wide. */
  tap(x: number, y: number, strength: number, radius: number) {
    this.x.wf_tap(x, y, strength, radius);
  }
  /** `dt` seconds of the water: "front" while a front is out (the water keeps its waves: the
   *  field's own leak and damp), "settling" while what is left settles (the water is the
   *  device's), "flat" once it is flat again. */
  step(dt: number): "front" | "settling" | "flat" {
    return (["flat", "settling", "front"] as const)[this.x.wf_step(dt)]!;
  }
  /** Draw what changed: the boxes (x, y, w, h each) of `pixels` that did. */
  draw(): Int16Array {
    const n = this.x.wf_draw();
    return new Int16Array(this.x.memory.buffer, this.x.wf_rects(), n * 4);
  }
  /** The canvas's pixels (width x height RGBA): a cell the water moves is opaque (the page's
   *  colour, its dust where the water put it), a cell at rest clear. A view of the module's
   *  memory: read it before the next call. */
  pixels(): Uint8ClampedArray {
    return new Uint8ClampedArray(this.x.memory.buffer, this.x.wf_pixels(), this.width * this.height * 4);
  }
  /** The dust alone on flat water, all of it `ambient` in focus (0: loose, 1: in focus), clear
   *  between the dots: a field of TILE x TILE px gives one tile. */
  picture(ambient: number): Uint8ClampedArray {
    return new Uint8ClampedArray(this.x.memory.buffer, this.x.wf_picture(ambient), this.width * this.height * 4);
  }
  /** The water under points `xy` (x, y each, in some screen's pixels) placed on the page at
   *  (ox + x * scale, oy + y * scale): its height now and a step ago, each. A view of the
   *  module's memory. */
  sample(xy: Float32Array, ox: number, oy: number, scale: number): Float32Array {
    const p = this.floats(xy.length * 2);
    new Float32Array(this.x.memory.buffer, p, xy.length).set(xy);
    this.x.wf_sample(p, xy.length / 2, ox, oy, scale, p + xy.length * 4);
    return new Float32Array(this.x.memory.buffer, p + xy.length * 4, xy.length);
  }
  destroy() {
    this.x.wf_close();
  }
}
