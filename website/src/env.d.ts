/// <reference types="astro/client" />

/* The REAL fluid engine from creatures/js (aliased in astro.config.mjs). Plain JS, typed here
   for the parts services/molty-stage uses. */

declare module "@creatures/fluid/sim.js" {
  export class FluidSim {
    constructor(W: number, H: number, cellW: number, cellH: number);
    W: number;
    H: number;
    N: number;
    A: number;
    inside: number;
    X: Float32Array;
    Y: Float32Array;
    mask: Uint8Array;
    sponge: Float32Array;
    rim: Float32Array;
    BGM: Float32Array;
    h: Float32Array;
    hp: Float32Array;
    hn: Float32Array;
    ink: Float32Array;
    r: Float32Array;
    g: Float32Array;
    b: Float32Array;
    acc: Float32Array;
    u: Float32Array;
    v: Float32Array;
    T: Float32Array;
    TR: Float32Array;
    TG: Float32Array;
    TB: Float32Array;
    TA: Float32Array;
    TE: Float32Array;
    TF: Float32Array;
    TEX: Float32Array;
    TEY: Float32Array;
    view: Float32Array;
    edgeW: number;
    swirlKick: number;
    p: Record<string, number> & { bgColor: [number, number, number] };
    impulse(x: number, y: number, strength?: number, radius?: number): void;
    push(x: number, y: number, du: number, dv: number, radius?: number): void;
    applyBackground(t: number, breath: number): void;
    step(dt: number): void;
    computeView(ambient: Float32Array | null): void;
  }
}

declare module "@creatures/fluid/render.js" {
  import type { FluidSim } from "@creatures/fluid/sim.js";
  export class GlyphRenderer {
    constructor(canvas: HTMLCanvasElement);
    g: CanvasRenderingContext2D;
    draw(sim: FluidSim, opt: Record<string, unknown>): number;
  }
}

declare module "@creatures/fluid/synth.js" {
  export const GlyphRamps: Record<string, string>;
}

declare module "@creatures/fluid/targets.js" {
  import type { FluidSim } from "@creatures/fluid/sim.js";
  export class CreatureTarget {
    constructor(sim: FluidSim);
    renderer: { forceContextLoss(): void };
    setCreature(entry: unknown): void;
    setMood(mood: string): void;
    update(dt: number): void;
    fill(sim: FluidSim, t: number, swell: number): void;
    dispose(): void;
  }
}

declare module "@creatures/cute/index.js" {
  export const CUTE_CATALOG: ReadonlyArray<{
    id: string;
    name: string;
    spec: { palette: Record<string, string> };
  }>;
}

declare module "@molty-catalog/catalog.json" {
  const catalog: { version: number; creatures: Array<{ id: string; name: string; file: string }> };
  export default catalog;
}

declare module "@creatures/setup/fluid-stage.js" {
  import type { FluidSim } from "@creatures/fluid/sim.js";
  /** The device's round 412 px screen (the console embeds it). */
  export class FluidStage {
    constructor(px?: number);
    el: HTMLCanvasElement;
    sim: FluidSim;
    textT: { setSentence(tokens: ReaderToken[]): void; setWord(token: ReaderToken): void };
    blob(): void;
    set(name: "blob" | "text" | "creature"): void;
    ripple(x: number, y: number, strength: number, radius: number): void;
  }
  export interface ReaderToken {
    word: string;
    orp: number;
    core: number;
    para: boolean;
  }
}

declare module "@creatures/fluid/reader.js" {
  import type { ReaderToken } from "@creatures/setup/fluid-stage.js";
  export function tokenize(text: string): ReaderToken[];
  export function wordDelay(token: ReaderToken, wpm: number): number;
}
