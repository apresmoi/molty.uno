// The demo's sky and weather (/demo/: src/features/sim/sky.ts and the controls in SimPage) on the committed module:
// a weather told is what the module reports, with its lifetime; the weather's setting off; the date set is the
// module's; the falling star button brings a star; the moon's nights to jump to are the right nights.
// Run: pnpm test (node --test; Node 22 strips the types).
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { MoltyCore, PANEL } from "../../src/services/molty-screen/core.ts";
import { MOON_NIGHTS, WEATHER_KINDS, moonLit, nextPhase, weatherLine } from "../../src/features/sim/sky.ts";

const ROOT = fileURLToPath(new URL("../../../../", import.meta.url));
const read = (path: string) => readFileSync(join(ROOT, path));
const module = await WebAssembly.compile(read("device/firmware/web/built/molty.wasm"));
const cat = read("packages/cat/cat.json");
const DT = 33_333;

async function molty() {
  const core = await MoltyCore.create(module);
  assert.ok(core.creature("cat", cat) && core.start(20261122, false, PANEL));
  return core;
}
const frames = (core: MoltyCore, n: number) => {
  for (let i = 0; i < n; i++) core.frame(DT);
};

test("a weather picked is told to the module, held as long as picked, and the setting turns it off", async () => {
  const core = await molty();
  frames(core, 30);
  assert.equal(core.read()?.weather?.kind, "clear", "nothing told: clear");
  assert.ok(core.weather(WEATHER_KINDS.indexOf("rain"), 600));
  frames(core, 30 * 13);
  const w = core.read()?.weather;
  assert.equal(w?.kind, "rain");
  assert.ok(w && w.s_left > 580 && w.s_left <= 600, `held for 600 s: ${w?.s_left} left`);
  assert.ok(w && w.level === 2, `eased to rain: ${w?.level}`);
  assert.match(weatherLine(w), /^rain · 10 min left · floor wet \d+%$/);
  assert.ok(core.state({ weather_on: false }));
  frames(core, 2);
  assert.equal(core.read()?.weather_on, false);
  assert.ok(core.state({ weather_on: true }) && core.weather(0, 0));
  frames(core, 2);
  assert.equal(core.read()?.weather?.kind, "clear");
});

test("the date set is the module's date; the sky's setting is the module's", async () => {
  const core = await molty();
  const t = Date.UTC(2016, 10, 14, 13, 52) / 1000;
  core.time(t);
  frames(core, 30);
  assert.ok(Math.abs(core.timeNow() - t - 1) < 0.1, `the module's date runs on from it: ${core.timeNow() - t} s`);
  assert.ok(core.state({ sky: false }));
  frames(core, 1);
  assert.equal(core.read()?.sky, false);
});

test("the falling star button brings a star", async () => {
  const a = await molty(), b = await molty();
  for (const c of [a, b]) {
    c.time(Date.UTC(2024, 3, 15, 19, 13) / 1000);
    frames(c, 90);
  }
  a.demoStar();
  let differ = 0;
  for (let i = 0; i < 30; i++) {
    a.frame(DT);
    b.frame(DT);
    const pa = a.pixels(), pb = b.pixels();
    let d = 0;
    for (let q = 0; q < pa.length; q += 4) if (pa[q] !== pb[q] || pa[q + 1] !== pb[q + 1] || pa[q + 2] !== pb[q + 2]) d++;
    differ = Math.max(differ, d);
  }
  assert.ok(differ > 20, `a star on the glass (${differ} px)`);
});

test("the nights to jump to: new, full, the supermoon and the micromoon", () => {
  const t = Date.UTC(2026, 10, 22, 12, 0);
  const nw = nextPhase(t, 0), full = nextPhase(t, 0.5);
  assert.ok(nw > t && nw - t <= 30 * 86_400_000 && moonLit(nw) < 0.01, `next new moon lit ${moonLit(nw)}`);
  assert.ok(full > t && full - t <= 30 * 86_400_000 && moonLit(full) > 0.99, `next full moon lit ${moonLit(full)}`);
  for (const [name, at] of MOON_NIGHTS) assert.ok(moonLit(at) > 0.97, `${name} full (${moonLit(at)})`);
});
