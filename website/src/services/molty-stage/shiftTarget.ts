// A target that can slide sideways across the field without being re-measured. The package
// measures a target's box only on relayout (which re-measures every target, text included:
// far too costly per frame), and a mesh target clips to the ellipse inscribed in its box. So
// a Molty running in keeps its own slot box (never clipped) and this wrapper writes that box
// `px` CSS px to the right: the inner target merges into a scratch frame, which is copied
// into the real frame shifted by whole cells. The sub-cell remainder comes back as world
// units for the rig to translate its polygons by, so the motion stays smooth.
import {
  createGlyphFluidTargetFrame,
  type GlyphFluidLayout,
  type GlyphFluidTargetFrame,
  type GlyphFluidTargetSpec,
} from "@glyphcss/fluid";

type Impl = ReturnType<GlyphFluidTargetSpec["create"]>;

/** A Molty firms up fast (the package's default is 0.4 s + 1 s): it has to read within
 *  ~0.3 s of arriving, and a running one before it arrives. */
const HOLD = { delay: 0, ramp: 0.3 };
const MIN_INK = 0.004; // mergeGlyphFluidCells' threshold for mesh targets

export class TargetShift {
  /** Whole cells the box is written to the right of its measured place. */
  di = 0;
  cw = 0;
  inner: Impl | null = null;

  /** Slide the target `px` CSS px right; returns the sub-cell remainder in units of `extent`
   *  across the box (what the polygons must move by along screen-x). 0 puts it back. */
  set(px: number, extent: number): number {
    const w = this.inner?.box()?.w;
    if (!px || !this.cw || !w) {
      this.di = 0;
      return 0;
    }
    const cells = px / this.cw,
      di = Math.floor(cells);
    this.di = di;
    return ((cells - di) * extent) / w;
  }

  /** Wrap a target spec so this shift applies to it. */
  wrap(spec: GlyphFluidTargetSpec): GlyphFluidTargetSpec {
    return {
      kind: spec.kind,
      create: (ctx) => new Shifted((this.inner = spec.create(ctx)), this),
    };
  }
}

class Shifted implements Impl {
  private scratch: GlyphFluidTargetFrame | null = null;

  constructor(
    private inner: Impl,
    private shift: TargetShift,
  ) {}

  get options() {
    return this.inner.options;
  }
  get strengthOn() {
    return this.inner.strengthOn;
  }
  get strengthOff() {
    return this.inner.strengthOff;
  }
  get hold() {
    return HOLD;
  }

  measure(layout: GlyphFluidLayout) {
    this.shift.cw = layout.cw;
    this.inner.measure(layout);
  }

  box() {
    const b = this.inner.box();
    return b && this.shift.di ? { ...b, i0: b.i0 + this.shift.di } : b;
  }

  changed() {
    return this.inner.changed?.() ?? false;
  }

  update(dt: number) {
    this.inner.update(dt);
  }

  write(frame: GlyphFluidTargetFrame, r0: number, swell: number) {
    const di = this.shift.di,
      b = this.inner.box();
    if (!di || !b) return this.inner.write(frame, r0, swell);
    const { cols: W, rows: H } = frame;
    if (!this.scratch || this.scratch.cols !== W || this.scratch.rows !== H)
      this.scratch = createGlyphFluidTargetFrame(W, H);
    const s = this.scratch;
    this.inner.write(s, r0, swell);
    // copy the box's cells di to the right (max-merge, like the package), clearing the scratch
    for (let jj = 0; jj < b.h; jj++) {
      const j = b.j0 + jj - r0;
      if (j < 1 || j >= H - 1) continue;
      for (let ii = 0; ii < b.w; ii++) {
        const i = b.i0 + ii;
        if (i < 1 || i >= W - 1) continue;
        const ks = j * W + i,
          v = s.T[ks]!;
        if (v <= 0) continue;
        s.T[ks] = 0;
        const it = i + di;
        if (it < 1 || it >= W - 1) continue;
        const k = j * W + it;
        if (v <= MIN_INK || v <= frame.T[k]!) continue;
        frame.T[k] = v;
        frame.TR[k] = s.TR[ks]!;
        frame.TG[k] = s.TG[ks]!;
        frame.TB[k] = s.TB[ks]!;
        frame.TF[k] = s.TF[ks]!;
        frame.TA[k] = s.TA[ks]!;
      }
    }
  }

  dispose() {
    this.inner.dispose();
  }
}
