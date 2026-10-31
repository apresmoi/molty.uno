// Molty's reflex over whole turns, for tools/sim `molty-sim reflex-turns`:
//   node --experimental-strip-types --import ./tools/ts-resolve.mjs tools/reflex-turns.ts <engine> [liveliness] < turns.jsonl
// stdin: one {"id", "kind", "text", "wpm", "end_ms"} per line. Each turn is spoken in real time, one
// partial per word (the box's stt.partial), into a ReflexSession — the exact code the relay runs
// (src/core/reflex.ts) — and the listen closes end_ms after the last word. The room's fire gate is
// emulated (closed after the listen, one reflex per the liveliness's gap). stdout: one line per turn
// {id, kind, words, speech_ms, fires: [{at, pick, soft?}], decisions: [{n, at, pick, raw?, c?, fired?, err?}]},
// times in ms from the first word. engine: keywords | jev (TYPESAFE_API_KEY, never printed).
import { createInterface } from "node:readline";
import * as R from "../src/core/reflex.ts";

const engine = process.argv[2];
const liveliness = process.argv[3];
if (!["keywords", "jev", "openai"].includes(engine)) {
  console.error("usage: reflex-turns.ts keywords|jev|openai [calm|normal|lively] < turns.jsonl");
  process.exit(2);
}
const keys = { jev: process.env.TYPESAFE_API_KEY, openai: process.env.OPENAI_API_KEY };
if (engine !== "keywords" && !keys[engine as "jev" | "openai"]) {
  console.error(`${engine}: no key in the environment`);
  process.exit(3);
}
const clock = { now: () => performance.now(), setTimeout: (f: () => void, ms: number) => setTimeout(f, ms), clearTimeout: (h: unknown) => clearTimeout(h as ReturnType<typeof setTimeout>) };
// the room's reflex gap for this liveliness (before the liveliness setting: REFLEX.gapMs)
const anyR = R as unknown as Record<string, any>;
const profile = liveliness && anyR.REFLEX_LIVELINESS ? anyR.REFLEX_LIVELINESS[liveliness] : undefined;
if (liveliness && anyR.REFLEX_LIVELINESS && !profile) {
  console.error(`unknown liveliness ${liveliness}`);
  process.exit(2);
}
const gapMs: number = profile?.gapMs ?? anyR.REFLEX.gapMs;
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

for await (const line of createInterface({ input: process.stdin })) {
  if (!line.trim()) continue;
  const turn = JSON.parse(line) as { id: string; kind: string; text: string; wpm: number; end_ms: number };
  const words = turn.text.split(/\s+/).filter(Boolean);
  const perWord = 60000 / turn.wpm;
  let t0 = 0;
  let closed = false;
  let lastFire = -Infinity;
  const fires: Array<{ at: number; pick: string; soft?: boolean }> = [];
  const decisions: Array<Record<string, unknown>> = [];
  const session = new R.ReflexSession({
    mode: engine,
    compare: false,
    avail: { local: false, jev: !!keys.jev, openai: !!keys.openai },
    keys,
    fetch: (r: Request) => fetch(r),
    clock,
    ...(liveliness ? { liveliness } : {}),
    fire: async (pick: string, soft?: boolean) => {
      if (closed) return "closed";
      const now = clock.now();
      if (now - lastFire < gapMs) return "rate_limited";
      lastFire = now;
      fires.push({ at: Math.round(now - t0), pick, ...(soft ? { soft: true } : {}) });
      return "ok";
    },
    note: (n: Record<string, unknown>) => {
      const { n: nn, pick, raw, c, fired, err, soft } = n as Record<string, unknown>;
      decisions.push({ n: nn, at: Math.round((n.at as number) - t0), pick, ...(raw ? { raw } : {}), ...(c !== undefined ? { c } : {}), ...(fired ? { fired } : {}), ...(err ? { err } : {}), ...(soft ? { soft } : {}) });
    },
  } as any);
  t0 = clock.now();
  for (let i = 0; i < words.length; i++) {
    const due = t0 + i * perWord;
    const wait = due - clock.now();
    if (wait > 0) await sleep(wait);
    session.text(words.slice(0, i + 1).join(" "));
  }
  const speechMs = Math.round(clock.now() - t0);
  await sleep(turn.end_ms);
  closed = true;
  // the room: a decision landing after the listen closed is "closed" (noted: the turn trace's fired:"closed")
  await session.idle();
  session.close();
  process.stdout.write(JSON.stringify({ id: turn.id, kind: turn.kind, words: words.length, speech_ms: speechMs, fires, decisions }) + "\n");
}
