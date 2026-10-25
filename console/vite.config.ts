import { defineConfig, build, type Plugin } from 'vite';
import { viteSingleFile } from 'vite-plugin-singlefile';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

// One self-contained HTML file (dist/index.html). The firmware stages it into
// the SPIFFS image and serves it at "/" (see firmware/main/CMakeLists.txt).
// Dev: `pnpm dev` and open http://localhost:8170/?device=http://<device-ip>
//
// Two extras for the setup flow:
//  - `@viewer/…` → creatures/js (the REAL fluid engine + cute-kit Moltys;
//    imported, never forked) and `@molty/…` → creatures/molty (.molty files).
//  - `virtual:molty-stage` — the animated round screen (src/stage/entry.js + the viewer
//    code) built as a SEPARATE ES module and inlined here as a string. three.js (~600 KB)
//    stays out of the firmware-served file: the stage imports it from the CDN at runtime,
//    and the console falls back to the live device preview when that import fails.
export const THREE_CDN = 'https://cdn.jsdelivr.net/npm/three@0.170.0/build/three.module.js';
const viewer = fileURLToPath(new URL('../creatures/js', import.meta.url));
const molty = fileURLToPath(new URL('../creatures/molty', import.meta.url));
const alias = { '@viewer': viewer, '@molty': molty };
/** Product version from the repo-root VERSION file (scripts/version.mjs) → __MOLTY_VERSION__. */
const VERSION = readFileSync(fileURLToPath(new URL('../VERSION', import.meta.url)), 'utf8').trim();

/** `import b64 from '…/x.molty'` → the file's bytes as a base64 string. */
const moltyFiles = (): Plugin => ({
  name: 'molty-b64',
  enforce: 'pre',
  load(id) {
    if (!id.endsWith('.molty')) return null;
    return `export default ${JSON.stringify(readFileSync(id).toString('base64'))};`;
  },
});

const STAGE_ID = 'virtual:molty-stage';
const stageModule = (): Plugin => ({
  name: 'molty-stage',
  resolveId: (id) => (id === STAGE_ID ? '\0' + STAGE_ID : null),
  async load(id) {
    if (id !== '\0' + STAGE_ID) return null;
    const out = await build({
      configFile: false, logLevel: 'warn', resolve: { alias },
      plugins: [{ name: 'three-cdn', enforce: 'pre', resolveId: (s) => (s === 'three' ? { id: THREE_CDN, external: true } : null) }],
      build: {
        write: false, target: 'es2020', minify: true,
        lib: { entry: fileURLToPath(new URL('./src/stage/entry.js', import.meta.url)), formats: ['es'], fileName: 'stage' },
      },
    });
    const res = (Array.isArray(out) ? out[0] : out) as { output: Array<{ type: string; code?: string }> };
    const chunk = res.output.find((o) => o.type === 'chunk');
    if (!chunk?.code) throw new Error('molty-stage: no chunk');
    return `export default ${JSON.stringify(chunk.code)};`;
  },
});

export default defineConfig({
  plugins: [moltyFiles(), stageModule(), viteSingleFile()],
  define: { __MOLTY_VERSION__: JSON.stringify(VERSION) },
  resolve: { alias },
  build: { target: 'es2020', cssMinify: true, reportCompressedSize: true, assetsInlineLimit: 1 << 20 },
  server: { port: 8170, strictPort: true, fs: { allow: ['..'] } },
  test: { include: ['src/**/*.test.ts'] },
} as Parameters<typeof defineConfig>[0]);
