// The relay's reflex engines on a list of texts, for tools/sim `molty-sim reflex-bench`:
//   node --experimental-strip-types --import ./tools/ts-resolve.mjs tools/reflex-bench.ts <engine> [liveliness] < items.jsonl
// stdin: one {"text": ...} per line; stdout: one verdict per line {pick, raw?, soft?, conf?, p?, cat?, ms, chars, tokens?, error?},
// gated at the liveliness (default normal) as the relay's session gates it (reflexGate).
// engine: keywords | jev (TYPESAFE_API_KEY) | openai (OPENAI_API_KEY). The exact code the relay runs
// (src/core/reflex.ts), called one at a time so each latency is one call's. Keys come from the
// environment and are never printed.
import { createInterface } from "node:readline";
import { DEFAULT_LIVELINESS, REFLEX_LIVELINESS, isLiveliness, keywordReflex, reflexGate, remoteReflex } from "../src/core/reflex.ts";

const engine = process.argv[2];
if (!["keywords", "jev", "openai"].includes(engine)) {
  console.error("usage: reflex-bench.ts keywords|jev|openai < items.jsonl");
  process.exit(2);
}
const lv = process.argv[3] ?? DEFAULT_LIVELINESS;
if (!isLiveliness(lv)) {
  console.error(`liveliness: calm | normal | lively`);
  process.exit(2);
}
const key = engine === "jev" ? process.env.TYPESAFE_API_KEY : engine === "openai" ? process.env.OPENAI_API_KEY : undefined;
if (engine !== "keywords" && !key) {
  console.error(`${engine}: no key in the environment (${engine === "jev" ? "TYPESAFE_API_KEY" : "OPENAI_API_KEY"})`);
  process.exit(3);
}
const clock = { now: () => performance.now(), setTimeout: (f: () => void, ms: number) => setTimeout(f, ms), clearTimeout: (h: unknown) => clearTimeout(h as ReturnType<typeof setTimeout>) };
const timeoutMs = Number(process.env.REFLEX_TIMEOUT_MS ?? 5000);

for await (const line of createInterface({ input: process.stdin })) {
  if (!line.trim()) continue;
  const { text } = JSON.parse(line) as { text: string };
  let v;
  if (engine === "keywords") {
    const t0 = performance.now();
    v = keywordReflex(text);
    v.ms = performance.now() - t0;
  } else v = await remoteReflex(engine as "jev" | "openai", text, { key, fetch: (r) => fetch(r), clock, timeoutMs });
  if (!v.error) v = reflexGate(v, REFLEX_LIVELINESS[lv]);
  const { engine: _e, catP: _cp, ...out } = v;
  process.stdout.write(JSON.stringify({ ...out, ms: Math.round(v.ms * 10) / 10 }) + "\n");
}
