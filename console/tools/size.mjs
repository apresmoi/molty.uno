// Print + gate the bundle size. Budget: 300 KB gzipped. What's in it (2026-10-25):
// app ~20 KB, fluid engine + 29 cute-kit Moltys as an inlined module string (three.js
// itself is NOT bundled — loaded from the CDN at runtime), 29 WebP thumbnails, 29 .molty
// files. The firmware serves it from SPIFFS/PSRAM (POST /dev/html caps at 4 MB raw).
import { readFileSync } from 'node:fs';
import { gzipSync } from 'node:zlib';
const buf = readFileSync(new URL('../dist/index.html', import.meta.url));
const gz = gzipSync(buf, { level: 9 }).length;
console.log(`dist/index.html: ${buf.length} B raw, ${gz} B gzip`);
if (gz > 300 * 1024) { console.error('over the 300 KB gzip budget'); process.exit(1); }
if (buf.length > 4 * 1024 * 1024) { console.error('over the firmware /dev/html 4 MB cap'); process.exit(1); }
