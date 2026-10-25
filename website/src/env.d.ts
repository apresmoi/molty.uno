/// <reference types="astro/client" />

/** The REAL fluid engine from creatures/js (aliased in astro.config.mjs). Plain JS, typed here. */
declare module "@creatures/setup/fluid-stage.js" {
  export class FluidStage {
    constructor(px?: number);
    el: HTMLCanvasElement;
    show(id: string): void;
    blob(): void;
    word(w: string): void;
    setMood(mood: string): void;
    setSize(px: number): void;
  }
  export const CUTE_CATALOG: ReadonlyArray<{ id: string; name: string; archetype: string }>;
}

declare module "@molty-catalog/catalog.json" {
  const catalog: { version: number; creatures: Array<{ id: string; name: string; file: string }> };
  export default catalog;
}
