// The relay's reflex engines on a list of texts, for tools/sim `molty-sim reflex-bench`:
//   node --experimental-strip-types --import ./tools/ts-resolve.mjs tools/reflex-bench.ts <engine> [liveliness] < items.jsonl
// stdin: one {"text": ...} per line; stdout: one verdict per line {pick, raw?, soft?, conf?, p?, cat?, ms, chars, tokens?, error?},
// gated at the liveliness (default normal) as the relay's session gates it (reflexGate).
// engine: keywords. The exact code the relay runs (src/core/reflex.ts). The box's engines: molty_voice/reflex.py bench.
import { createInterface } from "node:readline";
import { DEFAULT_LIVELINESS, REFLEX_LIVELINESS, isLiveliness, keywordReflex, reflexGate } from "../src/core/reflex.ts";

const engine = process.argv[2];
if (engine !== "keywords") {
  console.error("usage: reflex-bench.ts keywords < items.jsonl");
  process.exit(2);
}
const lv = process.argv[3] ?? DEFAULT_LIVELINESS;
if (!isLiveliness(lv)) {
  console.error(`liveliness: calm | normal | lively`);
  process.exit(2);
}

for await (const line of createInterface({ input: process.stdin })) {
  if (!line.trim()) continue;
  const { text } = JSON.parse(line) as { text: string };
  const t0 = performance.now();
  let v = keywordReflex(text);
  v.ms = performance.now() - t0;
  if (!v.error) v = reflexGate(v, REFLEX_LIVELINESS[lv]);
  const { engine: _e, catP: _cp, ...out } = v;
  process.stdout.write(JSON.stringify({ ...out, ms: Math.round(v.ms * 10) / 10 }) + "\n");
}
