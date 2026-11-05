import { defineConfig, type Plugin } from 'vite';
import { viteSingleFile } from 'vite-plugin-singlefile';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

// One self-contained HTML file (dist/index.html). The firmware stages it into
// the SPIFFS image and serves it at "/" (see device/firmware/main/CMakeLists.txt).
// Dev: `pnpm dev` and open http://localhost:8170/?device=http://<device-ip>
//
// The setup flow's round screen (src/stage/stage.js) runs the viewer's fluid engine
// (`@viewer/…` → device/creatures/js, imported, never forked). The engine's targets module
// also holds the cute-kit creature (three.js), which the console never uses: `three` is an
// external and the viewer's modules are side-effect free, so tree-shaking drops both.
const viewer = fileURLToPath(new URL('../creatures/js', import.meta.url));
/** Product version from the repo-root VERSION file (tools/scripts/version.mjs) → __MOLTY_VERSION__. */
const VERSION = readFileSync(fileURLToPath(new URL('../../VERSION', import.meta.url)), 'utf8').trim();

/** `three` stays an external import nothing uses; tree-shaking drops it (tools/size.mjs checks). */
const noThree = (): Plugin => ({ name: 'no-three', enforce: 'pre', resolveId: (id) => (id === 'three' ? { id, external: true } : null) });

export default defineConfig({
  plugins: [noThree(), viteSingleFile()],
  define: { __MOLTY_VERSION__: JSON.stringify(VERSION) },
  resolve: { alias: { '@viewer': viewer } },
  build: { target: 'es2020', cssMinify: true, reportCompressedSize: true, assetsInlineLimit: 1 << 20,
    rollupOptions: { treeshake: { moduleSideEffects: (id: string) => id !== 'three' && !id.startsWith(viewer) } } },
  server: { port: 8170, strictPort: true, fs: { allow: ['../..'] } },
  test: { include: ['src/**/*.test.ts'] },
} as Parameters<typeof defineConfig>[0]);
