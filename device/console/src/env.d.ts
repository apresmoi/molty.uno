/// <reference types="vite/client" />
/** Product version (repo-root VERSION), injected by vite.config.ts `define`. */
declare const __MOLTY_VERSION__: string;
declare module 'virtual:molty-stage' {
  /** ES module source of src/stage/entry.js (+ viewer code); imports three from the CDN. */
  const code: string;
  export default code;
}
declare module '*.molty' {
  /** The .molty file's bytes, base64. */
  const b64: string;
  export default b64;
}
