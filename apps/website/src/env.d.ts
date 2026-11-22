/// <reference types="astro/client" />

/* The repo's own files, aliased in astro.config.mjs: the firmware's renderer built for the web
   (@molty-wasm, device/firmware/web/built) and the packages it plays (@packages). Fetched by
   URL at run time (the ?url imports); a page's frontmatter may also read a package at build. */
declare module "@packages/*.json" {
  const value: unknown;
  export default value;
}

/** ESP Web Tools' installer dialog (the pinned 10.4.0 browser bundle's chunk): registers
 *  <ewt-install-dialog> on import. */
declare module "esp-web-tools/dist/web/install-dialog-im156JnI.js";

/** The molty-astro skill's sky (skills/molty-astro/scripts/sky.mjs, plain JS): the simulator's
 *  scenes are computed by it (astro.config.mjs runs it in the browser). */
declare module "@skills/molty-astro/scripts/sky.mjs" {
  type Scene = [layout: string, params: Record<string, unknown>, ttl?: number];
  export interface Show {
    scenes: Scene[];
    say?: string;
    read?: string;
    facts?: unknown;
  }
  export const SIGNS: string[];
  export const instant: (d: string) => Date;
  export function chart(date: Date, opts?: { all?: boolean }): Show;
  export function tonight(date: Date): Show;
  export function closeup(date: Date, id: string): Show;
  export function aspect(kind: string, date: Date): Show;
  export const EVENTS: Record<string, (date: Date) => Show>;
}
