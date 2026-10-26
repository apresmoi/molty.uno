/// <reference types="astro/client" />

/* The REAL creature engine from creatures/js (aliased in astro.config.mjs). Plain JS, typed
   here for the parts services/molty-stage uses: the cute kit and its mood engine are the
   Moltys' rig; @glyphcss/fluid draws them. */

declare module "@creatures/cute/kit.js" {
  /** The slice of a three.js Object3D the rig's scene graph is read through. */
  interface RigObject {
    isMesh?: boolean;
    matrixWorld: { elements: ArrayLike<number> };
    geometry?: {
      getAttribute(name: "position"): {
        count: number;
        getX(i: number): number;
        getY(i: number): number;
        getZ(i: number): number;
      };
      getIndex(): { count: number; getX(i: number): number } | null;
    };
    material?: { color: { getHexString(): string } };
  }
  export class CuteCreature {
    constructor(entry: unknown);
    root: { updateMatrixWorld(force?: boolean): void; traverseVisible(fn: (o: RigObject) => void): void };
    envelope: { min: { x: number; y: number; z: number }; max: { x: number; y: number; z: number }; rise?: number };
    update(dt: number, pose: unknown): void;
    dispose(): void;
  }
}

declare module "@creatures/mood.js" {
  export class MoodEngine {
    set(mood: string): void;
    update(dt: number): unknown;
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

/** ESP Web Tools' installer dialog (the pinned 10.4.0 browser bundle's chunk): registers
 *  <ewt-install-dialog> on import. */
declare module "esp-web-tools/dist/web/install-dialog-im156JnI.js";
