// Print + gate the bundle size. Budget: 300 KB gzipped. What's in it (2026-11-04): the app
// and the viewer's fluid engine for the setup screen, no Moltys (they are the device's
// creature packages) and no three.js. The firmware serves it from SPIFFS/PSRAM (POST
// /dev/html caps at 4 MB raw).
import { readFileSync } from 'node:fs';
import { gzipSync } from 'node:zlib';
const buf = readFileSync(new URL('../dist/index.html', import.meta.url));
const gz = gzipSync(buf, { level: 9 }).length;
console.log(`dist/index.html: ${buf.length} B raw, ${gz} B gzip`);
if (gz > 300 * 1024) { console.error('over the 300 KB gzip budget'); process.exit(1); }
if (buf.length > 4 * 1024 * 1024) { console.error('over the firmware /dev/html 4 MB cap'); process.exit(1); }
if (/\bfrom\s*["']three["']/.test(buf.toString())) { console.error('three.js leaked into the console (vite.config.ts no-three)'); process.exit(1); }
