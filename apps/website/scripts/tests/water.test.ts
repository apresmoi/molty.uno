// The page's water and words against the firmware they follow. The water is the firmware's own
// (the module: device/firmware/web/web_field.c runs fluid_sim.c and fluid_draw.c on a wide
// grid); what is the website's is where the page's dust is, and its numbers are the firmware's,
// pinned here: a change there fails this until the page follows. One water: a wave crosses a
// screen's rim as one front, and a heading rides it. A tap's ring fades as it spreads and the page
// is still a few seconds after the last tap, on any window. The words are a scenes package the
// firmware's renderer must take, each within what it draws in high definition.
// Run: pnpm test (node --test; Node 22 strips the types).
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { BUTTON, CARRY, HEADING_GAIN, LIFE, SPEED } from "../../src/config/water.mjs";
import { WORDS, wordsPackage } from "../../src/config/words.mjs";
import { TILES, readTile, renderTile } from "../dust-tile.mjs";
import { MoltyCore, instantiate } from "../../src/services/molty-screen/core.ts";
import { CELL, MoltyField, TILE, WATER, type FieldExports } from "../../src/services/molty-screen/field.ts";

const repo = (path: string) => readFileSync(fileURLToPath(new URL(`../../../../${path}`, import.meta.url)));
const define = (source: string, name: string) => {
  const m = new RegExp(`#define\\s+${name}\\s+\\(?([-0-9.]+)f?\\b`).exec(source);
  assert.ok(m, `${name} is defined`);
  return Number(m[1]);
};
const module = await WebAssembly.compile(repo("device/firmware/web/built/molty.wasm"));
const field_c = repo("device/firmware/web/web_field.c").toString();

test("the page's dust is the motes of a creature's world (creature_dots.c)", () => {
  const c = repo("device/firmware/main/creature/creature_dots.c").toString();
  // where a mote is, how bright, which of its two colours
  assert.ok(c.includes(`seed >= ${define(field_c, "WF_MOTE_FAR")} + ${define(field_c, "WF_MOTE_NEAR")} * band`), "a dot in fifty, more toward the horizon");
  assert.ok(c.includes("(0.3f + 0.55f * tw) * (0.55f + 0.45f * d->mote_band[m])") && field_c.includes("al = 0.3f + 0.55f * tw") && field_c.includes("al * 0.55f"));
  assert.ok(c.includes(`kv > ${define(field_c, "WF_MOTE_CUT")}f ? pack(cc3) : 0`), "a mote this faint does not show");
  assert.ok(c.includes("tw > 0.8f ? MOTE : DIM") && field_c.includes("tw > 0.8f ? WF_BRIGHT : WF_DIM"));
  assert.ok(c.includes("DIM[3] = { 0x5c / 255.f, 0x56 / 255.f, 0x88 / 255.f }, MOTE[3] = { 0xb4 / 255.f, 0xac / 255.f, 0xec / 255.f }"));
  assert.ok(field_c.includes("WF_DIM[3] = { 0x5c, 0x56, 0x88 }, WF_BRIGHT[3] = { 0xb4, 0xac, 0xec }"));
  // its hash
  const hash = "uint32_t h = (uint32_t)i * 374761393u + (uint32_t)j * 668265263u + (uint32_t)k * 2147483647u;\n    h = (h ^ (h >> 13)) * 1274126177u;\n    return h ^ (h >> 16);";
  assert.ok(c.includes(hash) && field_c.includes(hash), "hash3u");
});

test("... its water and its drawer take the firmware's numbers", () => {
  const sim = repo("device/firmware/main/fluid/fluid_sim.c").toString();
  const mole = repo("device/firmware/main/fluid/mole_fluid.c").toString();
  const draw = repo("device/firmware/main/fluid/fluid_draw.c").toString();
  assert.match(sim, new RegExp(`p->c2 = ${WATER.c2}f;\\s+p->damp = ${WATER.damp}f; p->leak = ${WATER.leak}f;`));
  // a 466 px panel's grid (fluid_grid_for_stage)
  assert.match(sim, /lrintf\(52\.f \* \(float\)stage \/ FLUID_STAGE_REF\)/);
  assert.equal(WATER.cells, Math.round((52 * WATER.panel) / define(repo("device/firmware/main/fluid/fluid_sim.h").toString(), "FLUID_STAGE_REF")));
  assert.ok(mole.includes(`ripple(x, y, ${WATER.tap}f, ${WATER.tapRadius}f);`), "a finger's tap");
  assert.equal(define(field_c, "WF_REFR"), define(draw, "DOT_REFR"));
  assert.ok(mole.includes("#define FLUID_STEP         (1.f / 60.f)") && field_c.includes("#define WF_STEP    (1.f / 60.f)"));
  assert.deepEqual([CELL.w, CELL.h], [define(field_c, "WF_CW"), define(field_c, "WF_CH")]);
  assert.equal(TILE, 104 * define(field_c, "WF_TILE_M"));
});

test("the dust tiles are pictures of the field at rest (node scripts/dust-tile.mjs)", async () => {
  for (const tile of TILES)
    assert.ok((await readTile(tile)).equals(await renderTile(tile)), `src/assets/dust/${tile.kind}-${tile.theme}.png is stale`);
});

// ---------------------------------------------------------------- one water

/** The landing at 1280 x 800: its reference screen shows 352 px, a tile 784. Places in CSS px. */
const HERO = 352;
const K = TILE / (Math.round((HERO * TILE) / WATER.panel / 4) * 4);
/** A wave's speed there, CSS px a step (services/page-water does the same sum). */
const STEP = (SPEED * Math.sqrt(WATER.c2) * (WATER.panel / K)) / WATER.cells;
const openField = async (cols: number, rows: number) =>
  MoltyField.open(await instantiate<FieldExports>(module), {
    cols,
    rows,
    background: 0,
    paper: false,
    c2: (STEP / (CELL.w / K)) ** 2,
    ...CARRY,
    life: LIFE,
  })!;
/** One frame of both, on one clock: the screen meets the page's water as it is (a heading's window `gain` times
 *  over), then each takes the frame. */
function frame(field: MoltyField, core: MoltyCore, box: { left: number; top: number; size: number; px: number }, gain = 1) {
  core.coupled(field.sample(core.cells().slice(), box.left * K, box.top * K, (box.size / box.px) * K), gain);
  core.frame(16667);
  field.step(1 / 60);
}
/** The crests along a line of heights (a step apart): where, and how high, the outermost first. */
function crests(line: Array<number | null>, step: number): Array<[number, number]> {
  const found: Array<[number, number]> = [];
  for (let q = line.length - 2; q > 0; q--) {
    const [a, b, c] = [line[q - 1], line[q], line[q + 1]];
    if (a != null && b != null && c != null && b > 0 && b >= a && b > c) found.push([q * step, b]);
  }
  return found;
}

for (const size of [HERO, 540])
  test(`a wave crosses the rim of a ${size} px screen as one front`, async () => {
    const field = await openField(212, 94);
    // a screen whose own water is still but for what comes in (a scene up: no creature stirs it)
    const core = await MoltyCore.create(module);
    assert.ok(core.pack(Buffer.from(JSON.stringify(wordsPackage()))) && core.start(5, false, WATER.panel) && core.scene("talk", undefined, 600));
    for (let i = 0; i < 240; i++) core.frame(16667);
    const box = { left: 700, top: 240, size, px: WATER.panel };
    const [cx, cy] = [box.left + size / 2, box.top + size / 2];
    const W = core.gridW;
    core.water((STEP / (size / W)) ** 2, CARRY.leak, CARRY.damp);
    core.couple(0.74, 0.86);
    // a tap on the page just left of the screen (its ring fades as it goes): its ray through the
    // screen's middle, every 2 px
    const tap = [box.left - 20, cy] as const;
    const ray = new Float32Array(2 * 420);
    for (let q = 0; q < 420; q++) ray.set([tap[0] + q * 2, cy], q * 2);
    const inGlass = (x: number) => Math.abs(x - cx) < 0.7 * (size / 2);
    /** The screen's own water along the ray (its middle row: between two rows of cells). */
    const inScreen = () => {
      const h = core.height();
      return Array.from({ length: 420 }, (_, q) => {
        const x = ray[2 * q]!;
        if (!inGlass(x)) return null;
        const gx = ((x - box.left) / size) * W - 0.5,
          i = Math.floor(gx),
          row = (core.gridH / 2 - 1) * W;
        return (((h[row + i]! + h[row + W + i]!) / 2) * (1 - (gx - i)) + ((h[row + i + 1]! + h[row + W + i + 1]!) / 2) * (gx - i)) / 1024;
      });
    };
    // (what the scene's coming left in its water: the wave is what comes over that)
    for (let f = 0; f < 90; f++) frame(field, core, box);
    const calm = inScreen();
    field.tap(tap[0] * K, tap[1] * K, WATER.tap, WATER.tapRadius * (WATER.panel / 2));
    let seen = 0,
      worst = 0,
      sum = 0;
    for (let f = 1; f <= 400; f++) {
      frame(field, core, box);
      const page = field.sample(ray, 0, 0, K);
      const out = Array.from({ length: 420 }, (_, q) => (inGlass(ray[2 * q]!) ? page[2 * q]! : null));
      const glass = inScreen().map((v, q) => (v == null ? null : v - calm[q]!));
      // the page's front (its outermost crest a dot would move by), while it is well inside the glass
      const [rp, ap] = crests(out, 2).find(([, a]) => a >= 0.06) ?? [0, 0];
      if (!ap || !inGlass(tap[0] + rp + 12) || !inGlass(tap[0] + rp - 12)) continue;
      // its height, from the trough behind it
      const rise = (line: Array<number | null>, r: number, top: number) => top - Math.min(...line.slice(Math.max(0, r / 2 - 20), r / 2).map((v) => v ?? top));
      const hp = rise(out, rp, ap);
      if (hp < 0.15) continue; // (while it is a wave a dot moves by)
      // ... and the screen's own water: its crest there (a screen shown larger than the page's reference has
      // coarser cells: its crest is a little lower, wider and later)
      const near = size === HERO ? 8 : 16;
      const [rs, as] = crests(glass, 2)
        .filter(([r]) => Math.abs(r - rp) <= near)
        .reduce((best, c) => (c[1] > best[1] ? c : best), [1e9, 0] as [number, number]);
      seen++;
      assert.ok(Math.abs(rp - rs) <= near, `frame ${f}: the front is ${rp} px from the tap on the page, and the screen has no crest within ${near} px of there`);
      worst = Math.max(worst, Math.abs(rp - rs));
      sum += Math.abs(rp - rs);
      const hs = rise(glass, rs, as);
      // (the page's reference screen: the same height; one shown larger has coarser cells for the same wave, and
      // its crest is lower and wider the farther it has come: half the page's at the far rim)
      assert.ok(size === HERO ? Math.abs(hs - hp) < 0.2 * hp : hs > 0.4 * hp && hs < 1.2 * hp, `frame ${f}: the crest stands ${hp.toFixed(3)} over its trough on the page and ${hs.toFixed(3)} in the screen`);
    }
    assert.ok(seen > 25, `the front was followed through the glass for ${seen} frames`);
    console.log(`      ${size} px: the front in the screen is ${(sum / seen).toFixed(1)} px from the page's on average over ${seen} frames, ${worst} px at most`);
    field.destroy();
  });

test("a tap on the glass leaves through the rim and does not come back", async () => {
  const field = await openField(212, 94);
  const core = await MoltyCore.create(module);
  assert.ok(core.creature("cat", repo("packages/cat/cat.json")) && core.start(5, false, WATER.panel));
  for (let i = 0; i < 120; i++) core.frame(16667);
  const box = { left: 700, top: 240, size: HERO, px: WATER.panel };
  core.water(WATER.c2 * SPEED ** 2, CARRY.leak, CARRY.damp); // the page's speed, as screen.ts gives it
  core.couple(0.74, 0.86);
  // the finger at the glass's middle, and the same bump on the page
  core.touch(true, WATER.panel / 2, WATER.panel / 2);
  field.tap((box.left + HERO / 2) * K, (box.top + HERO / 2) * K, WATER.tap, WATER.tapRadius * (HERO / 2) * K);
  const energy = () => core.height().reduce((s, v) => s + (v / 1024) ** 2, 0);
  let peak = 0;
  for (let f = 1; f <= 300; f++) {
    frame(field, core, box);
    if (f === 2) core.touch(false);
    if (f < 60) peak = Math.max(peak, energy());
  }
  // 5 s on, the ring is long past the rim (1.5 s away): what is left in the glass is a few percent
  assert.ok(energy() < 0.05 * peak, `the glass keeps ${((energy() / peak) * 100).toFixed(1)}% of the tap's water`);
  field.destroy();
});

// ---------------------------------------------------------------- a tap's ring dies on the way

/** The windows the page is seen in, and its reference screen there (styles/tokens.css --m-screen), CSS px. */
const WINDOWS = [
  [390, 844, 337.6],
  [1024, 768, 337.9],
  [1440, 900, 396],
  [1920, 1080, 420],
  [2560, 1440, 420],
] as const;
/** The page's field for a window, as services/page-water opens it (its grain, a margin of 2 x 8 cells, its speed),
 *  with the page's numbers unless others are given (`speed`: times the device's). Places are CSS px of the viewport. `more`: cells past its right. */
async function pageField(vw: number, vh: number, screen: number, o: { carry?: typeof CARRY; speed?: number; life?: number; more?: number } = {}) {
  const k = TILE / (Math.round((screen * TILE) / WATER.panel / 4) * 4);
  const x = await instantiate<FieldExports>(module);
  const step = ((o.speed ?? SPEED) * Math.sqrt(WATER.c2) * (WATER.panel / k)) / WATER.cells;
  const cols = Math.ceil((vw * k) / CELL.w) + 4 + (o.more ?? 0),
    rows = Math.ceil((vh * k) / CELL.h) + 16;
  const field = MoltyField.open(x, { cols, rows, background: 0, paper: false, c2: (step / (CELL.w / k)) ** 2, ...(o.carry ?? CARRY), life: o.life })!;
  field.place(-2, -8);
  const W = x.wf_grid_w(),
    q0 = (x.wf_window_j() + 8) * W + x.wf_window_i() + 2,
    ay = (2 * x.wf_grid_h()) / W;
  /** The drawer's shift of a dot at the viewport's cell (i, j), sub-dots (web_field.c lay_crest): it shows from 0.5. */
  const shift = (i: number, j: number) => {
    const h = new Int16Array(x.memory.buffer, x.wf_h(), W * x.wf_grid_h()),
      q = q0 + j * W + i;
    return [(h[q + 1]! - h[q - 1]!) * (3.3 / 1024) * 2, (h[q + W]! - h[q - W]!) * (3.3 / 1024) * 2 * ay] as const;
  };
  const cell = (px: number) => Math.floor((px * k) / CELL.w);
  /** Whether any dot of the viewport's box (CSS px) moves now, and the largest shift there. */
  const moves = (x0 = 0, y0 = 0, x1 = vw, y1 = vh) => {
    let top = 0;
    for (let j = Math.floor((y0 * k) / CELL.h); j < Math.ceil((y1 * k) / CELL.h); j++)
      for (let i = cell(x0); i < Math.ceil((x1 * k) / CELL.w); i++) top = Math.max(top, Math.hypot(...shift(i, j)));
    return top;
  };
  const tap = (px: number, py: number, strength = WATER.tap) => field.tap(px * k, py * k, strength, (WATER.tapRadius * (WATER.panel / k)) / 2 * k);
  return { field, tap, moves, shift, cols: cols - 4 - (o.more ?? 0), rows: rows - 16 };
}
/** Taps (s, CSS px) and the water run on until flat (or `most` s): when the last dot of the viewport moved and when
 *  the water was flat, s after the last tap. */
function after(p: Awaited<ReturnType<typeof pageField>>, taps: Array<[number, number, number]>, most = 30) {
  const end = taps.at(-1)![0];
  let last = 0;
  for (let f = 0; f <= (end + most) * 60; f++) {
    for (const [t, x, y] of taps) if (Math.round(t * 60) === f) p.tap(x, y);
    const state = p.field.step(1 / 60);
    const t = (f + 1) / 60 - end;
    if (p.moves() >= 0.5) last = t;
    if (state === "flat" && t > 0) return { last, flat: t };
  }
  return { last, flat: Infinity };
}
/** A burst: twelve taps over the window, a quarter of a second apart. */
const burst = (vw: number, vh: number): Array<[number, number, number]> =>
  [[0.5, 0.5], [0.22, 0.3], [0.8, 0.62], [0.35, 0.8], [0.66, 0.2], [0.12, 0.7], [0.9, 0.35], [0.5, 0.15], [0.3, 0.55], [0.72, 0.85], [0.08, 0.12], [0.58, 0.42]].map(([fx, fy], q) => [q / 4, fx! * vw, fy! * vh]);

test("a tap's ring dies on the way: on any window, after any taps, the dust is still within 4.5 s and the water flat by itself within 6 s", async () => {
  for (const [vw, vh, screen] of WINDOWS)
    for (const [what, taps] of [
      ["a tap in the middle", [[0, vw / 2, vh / 2]]],
      ["a tap near a corner", [[0, 40, 40]]],
      ["a burst of twelve taps", burst(vw, vh)],
    ] as Array<[string, Array<[number, number, number]>]>) {
      const p = await pageField(vw, vh, screen);
      const r = after(p, taps);
      assert.ok(r.last <= 4.5, `${vw} x ${vh}, ${what}: the last dot moves ${r.last.toFixed(1)} s after it`);
      assert.ok(r.flat <= 6, `${vw} x ${vh}, ${what}: the water is flat ${r.flat.toFixed(1)} s after it`);
      p.field.destroy();
    }
});

test("... it loses its height as it spreads, does not come back off the rim, and is a little faster than the device's", async () => {
  // 1920 x 1080, a tap 200 px in from the left edge: along its row, where the dots move and when they move most
  let p = await pageField(1920, 1080, 420);
  p.tap(200, 540);
  const marks = [100, 300, 700];
  const seen = marks.map(() => 0),
    most = marks.map(() => [0, 0]);
  for (let f = 1; f <= 6 * 60; f++) {
    p.field.step(1 / 60);
    marks.forEach((d, n) => {
      const m = p.moves(200 + d, 530, 200 + d + 8, 550);
      if (m >= 0.5) seen[n]++;
      if (m > most[n]![0]!) most[n] = [m, f];
    });
  }
  assert.ok(seen[0]! > 10 && seen[1]! > 5, `the ring moves dots 100 px and 300 px from the tap (${seen[0]} and ${seen[1]} frames)`);
  assert.ok(seen[2] === 0, `700 px from the tap it moves none (it did for ${seen[2]} frames)`);
  assert.ok(most[0]![0]! > 2 * most[1]![0]!, `it is lower farther out (${most[0]![0]!.toFixed(2)} sub-dots at 100 px, ${most[1]![0]!.toFixed(2)} at 300)`);
  const speed = (200 * 60) / (most[1]![1]! - most[0]![1]!);
  assert.ok(speed >= 150 && speed <= 200, `its crest goes ${speed.toFixed(0)} CSS px/s (the device's own speed is 141 there)`);
  p.field.destroy();
  // 1440 x 900, a tap 40 px from the right edge, on the page's field and on one whose right rim is 100 cells farther:
  // what the two differ by inside the viewport is what the rim sends back, and it moves no dot
  p = await pageField(1440, 900, 396);
  const far = await pageField(1440, 900, 396, { more: 100 });
  p.tap(1400, 450);
  far.tap(1400, 450);
  let echo = 0;
  for (let f = 1; f <= 8 * 60; f++) {
    p.field.step(1 / 60);
    far.field.step(1 / 60);
    if (f % 3) continue;
    for (let j = 0; j < p.rows; j += 2)
      for (let i = p.cols - 60; i < p.cols; i++) {
        const [ax, ay] = p.shift(i, j),
          [bx, by] = far.shift(i, j);
        echo = Math.max(echo, Math.hypot(ax - bx, ay - by));
      }
  }
  assert.ok(echo < 0.5, `the rim's echo shifts a dot ${echo.toFixed(2)} sub-dots (a dot moves from 0.5)`);
  far.field.destroy();
  p.field.destroy();
});

test(`a tap on a control raises a ring ${BUTTON} as high as a tap on bare page, as wide and as fast`, async () => {
  // 1440 x 900, a tap in the middle: the water's steepest slope along its row, every few frames for 2 s
  const ring = async (strength: number) => {
    const p = await pageField(1440, 900, 396);
    p.tap(720, 450, strength);
    const seen: Array<[number, number]> = [];
    for (let f = 1; f <= 120; f++) {
      p.field.step(1 / 60);
      if (f % 10) continue;
      let top = 0,
        at = 0;
      for (let x = 720; x < 1400; x += 4) {
        const m = p.moves(x, 440, x + 4, 460);
        if (m > top) [top, at] = [m, x];
      }
      seen.push([top, at]);
    }
    p.field.destroy();
    return seen;
  };
  const [full, faint] = [await ring(WATER.tap), await ring(WATER.tap * BUTTON)];
  const seen = full.filter(([h]) => h >= 1).length;
  assert.ok(seen >= 6, `a full tap's crest moves dots for ${seen} of the samples`);
  full.forEach(([h, x], n) => {
    const [fh, fx] = faint[n]!;
    if (h < 1) return;
    assert.ok(Math.abs(fh / h - BUTTON) < 0.04, `${(n + 1) / 6} s after the tap: its crest is ${((100 * fh) / h).toFixed(0)}% as high (${fh.toFixed(2)} sub-dots against ${h.toFixed(2)})`);
    assert.ok(Math.abs(fx - x) <= 8, `${(n + 1) / 6} s after the tap: its crest is where a full one's is (${fx} px against ${x})`);
  });
});

test(`water that keeps its waves is still laid flat ${LIFE} s after the last tap`, async () => {
  // what the page's water once was (a step kept 0.999 of a wave): on a 2560 x 1440 window a burst and a tap near a
  // corner left it moving for over 5 minutes, the rounding of its heights feeding it more than a step took
  const p = await pageField(2560, 1440, 420, { carry: { leak: 1, damp: 0.998 }, speed: 1, life: LIFE });
  const r = after(p, [...burst(2560, 1440), [3, 40, 40]], 60);
  assert.ok(r.flat <= LIFE + 0.1, `flat ${r.flat.toFixed(1)} s after the last tap`);
  // ... and a tap after it makes a ring again
  p.tap(1280, 720);
  p.field.step(1 / 60);
  p.field.step(1 / 60);
  assert.ok(p.moves() >= 0.5, "a tap after it moves the dust");
  p.field.destroy();
});

// ---------------------------------------------------------------- the headings ride it

/** A heading's window as the page mounts it (MoltyWord): on the haze, all its water the page's. */
async function heading(layout: string, px: number) {
  const core = await MoltyCore.create(module);
  assert.ok(core.pack(Buffer.from(JSON.stringify(wordsPackage()))) && core.start(3, false, px));
  core.haze();
  assert.ok(core.scene(layout, undefined, 600));
  core.water((STEP / (px / K / core.gridW)) ** 2, CARRY.leak, CARRY.damp);
  core.couple(0, 0);
  return core;
}
/** The word's own lines: the pixels its colour is strongest in, while the water is flat; and
 *  how many of them still are its lines' (not the dimmer dots a loosened line breaks into). */
const light = (px: Uint8ClampedArray, i: number) => Math.max(px[i]!, px[i + 1]!, px[i + 2]!);
function lines(core: MoltyCore): { at: number[]; full: number } {
  const px = core.pixels();
  let full = 0;
  for (let i = 0; i < px.length; i += 4) full = Math.max(full, light(px, i));
  const at: number[] = [];
  for (let i = 0; i < px.length; i += 4) if (light(px, i) >= 0.7 * full) at.push(i);
  return { at, full };
}
const kept = (core: MoltyCore, { at, full }: { at: number[]; full: number }) => {
  const px = core.pixels();
  return at.filter((i) => light(px, i) >= 0.5 * full).length / at.length;
};

test("every heading rides the page's water: a wave loosens its lines as the front arrives, and they sharpen back", async () => {
  const PX = 512; // the window's side here (the page gives it as many as its dust has across it)
  const size = PX / K;
  for (const [layout] of WORDS) {
    const field = await openField(212, 94);
    const core = await heading(layout, PX);
    const box = { left: 500, top: 200, size, px: PX };
    for (let f = 0; f < 240; f++) frame(field, core, box, HEADING_GAIN);
    const at = lines(core);
    assert.ok(at.at.length > 200 && kept(core, at) === 1, `${layout}: its lines stand on flat water (${at.at.length} px)`);
    // a tap beside it, left of its window: the front reaches its first line after their distance / the speed
    const tap = [box.left - 0.3 * size, box.top + size / 2] as const;
    field.tap(tap[0] * K, tap[1] * K, WATER.tap, WATER.tapRadius * (WATER.panel / 2));
    const nearest = Math.min(...at.at.map((i) => Math.hypot(box.left + (((i / 4) % PX) / PX) * size - tap[0], box.top + (Math.floor(i / 4 / PX) / PX) * size - tap[1])));
    const arrive = nearest / STEP;
    let before = 1,
      loosest = 1;
    for (let f = 1; f <= arrive + 240; f++) {
      frame(field, core, box, HEADING_GAIN);
      const k = kept(core, at);
      if (f < arrive - 30) before = Math.min(before, k);
      else loosest = Math.min(loosest, k);
    }
    assert.ok(before > 0.99, `${layout}: nothing moves before the front arrives (${(before * 100).toFixed(0)}% of its lines stand)`);
    assert.ok(loosest < 0.97, `${layout}: the front loosens its lines (${(loosest * 100).toFixed(1)}% stand at the least)`);
    assert.ok(loosest > 0.3, `${layout}: it stays a word (${(loosest * 100).toFixed(0)}% of its lines stand at the least)`);
    for (let f = 0; f < 900; f++) frame(field, core, box, HEADING_GAIN);
    assert.ok(kept(core, at) > 0.99, `${layout}: its lines are back (${(kept(core, at) * 100).toFixed(1)}%)`);
    field.destroy();
  }
});

test("a tap on a heading loosens it most", async () => {
  const PX = 512,
    size = PX / K;
  const loosest = async (dx: number) => {
    const field = await openField(212, 94);
    const core = await heading("molty-uno", PX);
    const box = { left: 500, top: 200, size, px: PX };
    for (let f = 0; f < 240; f++) frame(field, core, box, HEADING_GAIN);
    const at = lines(core);
    field.tap((box.left + size / 2 + dx) * K, (box.top + size / 2) * K, WATER.tap, WATER.tapRadius * (WATER.panel / 2));
    let least = 1;
    for (let f = 0; f < 400; f++) {
      frame(field, core, box, HEADING_GAIN);
      least = Math.min(least, kept(core, at));
    }
    field.destroy();
    return least;
  };
  const [on, beside, far] = [await loosest(0), await loosest(-0.6 * size), await loosest(-1.5 * size)];
  assert.ok(on < beside && beside <= far, `on it ${(on * 100).toFixed(0)}% of its lines stand, beside it ${(beside * 100).toFixed(0)}%, far from it ${(far * 100).toFixed(0)}%`);
});

test("the words are a package the renderer takes, each drawn in high definition", async () => {
  const json = JSON.stringify(wordsPackage());
  assert.equal(repo("apps/website/src/assets/molty/words.json").toString().trim(), json, "words.json is stale: node scripts/words-package.mjs");
  const core = await MoltyCore.create(module);
  assert.ok(core.pack(Buffer.from(json)));
  for (const [name] of WORDS) {
    const layout = wordsPackage().layouts[name]!;
    // a line is a shape a segment; the renderer draws at most 256 in high definition
    const shapes = layout.items.reduce((n: number, it: { pts?: unknown[] }) => n + (it.pts ? it.pts.length - 1 : 1), 0);
    assert.ok(shapes <= 256, `${name}: ${shapes} shapes`);
  }
});
