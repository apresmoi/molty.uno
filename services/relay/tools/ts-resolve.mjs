// Lets Node run the relay's TypeScript sources directly (node --experimental-strip-types): an import
// of "./x.js" that does not exist resolves to "./x.ts". Used by tools/reflex-bench.ts only.
import { register } from "node:module";

register(
  "data:text/javascript," +
    encodeURIComponent(`
import { existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
export async function resolve(spec, ctx, next) {
  if (spec.endsWith(".js") && (spec.startsWith("./") || spec.startsWith("../")) && ctx.parentURL?.startsWith("file:")) {
    const ts = new URL(spec.slice(0, -3) + ".ts", ctx.parentURL);
    if (!existsSync(fileURLToPath(new URL(spec, ctx.parentURL))) && existsSync(fileURLToPath(ts))) return next(ts.href, ctx);
  }
  return next(spec, ctx);
}`),
);
