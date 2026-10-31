// The relay's own guard for the eval: JSON lines {line, human, history} in, {ok, text|why} out.
// node --experimental-strip-types tools/sim/opener_guard.mjs
import { createInterface } from "node:readline";
import { guardOpener } from "../../services/relay/src/core/opener.ts";

for await (const raw of createInterface({ input: process.stdin })) {
  const q = JSON.parse(raw);
  console.log(JSON.stringify(guardOpener(q.line, q.human, q.history ?? [])));
}
