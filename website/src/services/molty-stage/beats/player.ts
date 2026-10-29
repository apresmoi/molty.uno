// Runs one glass beat on one slot's rig (see api.ts for how beats are written).
import type { CuteCreature, RigObject } from "@creatures/cute/kit.js";
import { neutralGlass, type MoltyGlass, type MoltyPose, type MoltyRig } from "../moltyMesh";
import { WINDOW, type Beat, type BeatCtx, type Face, type Fx, type Mood, type Particle } from "./api";

/** What the player needs from the page: the slot's box and the fluid's two forces. */
export interface FxHost {
  rect(): DOMRect;
  impulse(clientX: number, clientY: number, strength: number, radius: number): void;
  push(clientX: number, clientY: number, dx: number, dy: number, radius: number): void;
}

const MAX_PARTICLES = 12;

const neutralFace = (f: Face) => {
  f.gaze = f.lx = f.ly = f.still = f.tilt = f.tiltW = 0;
  f.turn = f.nod = f.bob = f.squash = f.pitch = f.lean = f.px = 0;
  f.lid = f.mouth = -1;
};

interface Snap {
  o: RigObject;
  v: number[];
}

export class BeatRun {
  t = 0;
  private t0 = 0;
  private dt = 0;
  private readonly g: MoltyGlass = neutralGlass();
  private readonly f = {} as Face;
  private readonly snaps = new Map<RigObject, Snap>();
  private readonly particles: Particle[] = [];
  private readonly ctx: BeatCtx;
  private readonly seed: number;

  constructor(
    readonly beat: Beat,
    private readonly rig: MoltyRig,
    private readonly host: FxHost,
    seed: number,
  ) {
    this.seed = seed;
    neutralFace(this.f);
    this.g.blush = false; // no blush at the glass
    const kit = rig.kit as CuteCreature;
    const self = this;
    const fx: Fx = {
      ripple: (x, y, strength = 0.5, radius = 0.1) => self.ripple(x, y, strength, radius),
      push: (x, y, dx, dy, radius = 0.15) => self.push(x, y, dx, dy, radius),
      rim: (strength, n = 0) => self.rim(strength, n),
      emit: (p) => {
        if (self.particles.length >= MAX_PARTICLES) self.particles.shift();
        self.particles.push({ ...p });
      },
      bolt: (x0, y0, x1, y1, strength = 0.6) => {
        const n = 7;
        for (let i = 0; i <= n; i++) {
          const q = i / n,
            j = i % 2 ? 0.12 : -0.12; // the zig and the zag
          self.ripple(x0 + (x1 - x0) * q + (i && i < n ? j : 0), y0 + (y1 - y0) * q, strength, 0.06);
        }
      },
    };
    this.ctx = {
      g: this.g,
      f: this.f,
      get dt() {
        return self.dt;
      },
      crossed: (at) => self.t0 < at && self.t >= at,
      mood: (m: Mood) => rig.setMood(m),
      kit,
      part: (name) => (kit.parts[name] ?? []).map((o) => self.snap(o)),
      socket: (name) => (kit.sock[name] ?? []).map((r) => self.snap(r.g)),
      node: (o) => self.snap(o),
      where: (at) => {
        const p = rig.where(at);
        return p ? [p[0] / WINDOW, p[1] / WINDOW] : null;
      },
      get eyeR() {
        return rig.eyeR;
      },
      fx,
      state: {},
      rand: (k) => {
        let h = (self.seed ^ Math.imul(k + 1, 2654435761)) >>> 0;
        h = Math.imul(h ^ (h >>> 15), 2246822507) >>> 0;
        return ((h ^ (h >>> 13)) >>> 0) / 4294967296;
      },
    };
    rig.glass = this.g;
    rig.setMood("listening");
    const f = this.f;
    rig.patch = (p: MoltyPose) => {
      p.look.x += (f.lx - p.look.x) * f.gaze;
      p.look.y += (f.ly - p.look.y) * f.gaze;
      if (f.lid >= 0) p.lid = f.lid;
      if (f.mouth >= 0) p.mouth = f.mouth;
      const k = 1 - f.still;
      p.pos.y = p.pos.y * k + f.bob;
      p.pos.x += f.px;
      p.squash = p.squash * k + f.squash;
      p.head.x = p.head.x * k + f.nod;
      p.rot.x = p.rot.x * k + f.pitch;
      p.rot.z += f.lean;
      p.head.z += (f.tilt - p.head.z) * f.tiltW;
      p.head.y += f.turn;
    };
    rig.post = beat.post ? () => beat.post!(self.ctx, self.t) : null;
    beat.start?.(this.ctx);
  }

  /** Advance one frame (before the rig poses). Returns false once the beat is over. */
  frame(dt: number): boolean {
    this.t0 = this.t;
    this.t += dt;
    this.dt = dt;
    const g = this.g;
    g.face = g.pin = g.press = g.focus = g.ox = g.oy = g.roll = g.yaw = g.tintK = 0;
    g.zoom = g.sx = g.sy = 1;
    g.focusAt = "face";
    neutralFace(this.f);
    this.beat.frame(this.ctx, this.t);
    // the particles: move, trail a ripple, die
    for (let i = this.particles.length - 1; i >= 0; i--) {
      const p = this.particles[i]!;
      p.life -= dt;
      if (p.life <= 0) {
        this.particles.splice(i, 1);
        continue;
      }
      p.vy -= (p.g ?? 0) * dt;
      p.x += p.vx * dt;
      p.y += p.vy * dt;
      this.ripple(p.x, p.y, p.strength, p.radius ?? 0.06);
    }
    return this.t < this.beat.len;
  }

  /** Put the rig back: no glass, no patch, every snapshotted node restored. */
  end() {
    this.rig.glass = null;
    this.rig.patch = null;
    this.rig.post = null;
    for (const { o, v } of this.snaps.values()) {
      o.position.set(v[0]!, v[1]!, v[2]!);
      o.rotation.set(v[3]!, v[4]!, v[5]!);
      o.scale.set(v[6]!, v[7]!, v[8]!);
      o.visible = v[9] === 1;
    }
    this.snaps.clear();
    this.particles.length = 0;
  }

  private snap(o: RigObject): RigObject {
    if (!this.snaps.has(o))
      this.snaps.set(o, {
        o,
        v: [
          o.position.x,
          o.position.y,
          o.position.z,
          o.rotation.x,
          o.rotation.y,
          o.rotation.z,
          o.scale.x,
          o.scale.y,
          o.scale.z,
          o.visible ? 1 : 0,
        ],
      });
    return o;
  }

  // window units → client px: (0, 0) the slot's centre, y up, 1 = half the slot's side
  private client(x: number, y: number): [number, number, number] {
    const r = this.host.rect(),
      R = Math.min(r.width, r.height) / 2;
    return [r.left + r.width / 2 + x * R, r.top + r.height / 2 - y * R, R];
  }

  private ripple(x: number, y: number, strength: number, radius: number) {
    const [cx, cy, R] = this.client(x, y);
    // the fluid's radii are in stage units: ±1 across the viewport's width
    this.host.impulse(cx, cy, strength, Math.max(0.005, (2 * radius * R) / Math.max(1, innerWidth)));
  }

  private push(x: number, y: number, dx: number, dy: number, radius: number) {
    const [cx, cy, R] = this.client(x, y);
    this.host.push(cx, cy, dx * R, -dy * R, Math.max(0.005, (2 * radius * R) / Math.max(1, innerWidth)));
  }

  private rim(strength: number, n: number) {
    const s = 1.08; // just outside the window
    if (!n) {
      this.ripple(-s, 0, strength, 0.15);
      this.ripple(s, 0, strength, 0.15);
      this.ripple(0, -s, strength, 0.15);
      return;
    }
    for (let i = 0; i < n; i++) {
      // round the sides and the foot, never over the heading above
      const a = Math.PI - 0.35 + (i / (n - 1)) * (Math.PI + 0.7); // left, foot, right (y up)
      this.ripple(Math.cos(a) * s, Math.sin(a) * s, strength, 0.15);
    }
  }
}
