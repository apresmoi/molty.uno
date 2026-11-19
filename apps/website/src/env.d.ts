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
