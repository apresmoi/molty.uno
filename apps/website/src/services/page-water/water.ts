// The page's water: one wave from every tap, anywhere on the page. The calm dust itself is a
// background tile (no script: features/water/dust.mjs, scripts/dust-tile.mjs); this draws only
// the wave over it, on one canvas fixed to the viewport, and only while a wave is travelling.
//
// One water with the screens: a tap inside a Molty screen (or a word) starts the page's wave at
// the same point, at the speed that screen's own water has, so the ring the firmware draws
// inside the glass and the ring drawn here are one ring: it crosses the rim and goes on.
//
// PAGE-SIDE, not the firmware's renderer (it draws a round panel, at most 511 px across in high
// definition): the wave's shape here is a travelling ring, with the firmware's speed and decay
// (fluid_sim.c) and the firmware's dust rule under it (fluid_fine.c: where the water is lit,
// more of the ordered specks show, and brighter). Reduced motion: no waves.
import { CALM, FINE, TILE, WAVE_KEEP, WAVE_SPEED, ign, speck } from "../../config/water.mjs";

/** A screen's width a tap on the bare page stands for (the hero's device), px. */
const NOMINAL = 420;
/** The crest's width (its gaussian sigma) as a share of that width: the tap's own size on a
 *  screen (mole_fluid.c ripples a finger's tap over 0.09 of the glass's radius). */
const CREST = 0.045;
/** How much of a drawing's dust the crest gathers (FINE.PEAK is a drawing's own edge). */
const GATHER = 0.45;
/** A wave is over once it is this faint. */
const FAINT = 0.05;

interface Wave {
  /** Where it started, from the page's own top left corner. */
  x: number;
  y: number;
  speed: number;
  sigma: number;
  t0: number;
}

export interface PageWaterHandle {
  destroy(): void;
}

/** Mount the wave layer on `canvas` (fixed to the viewport; CSS sizes it). `origin` is the element
 *  the dust tile is the background of: the specks a wave lights are that tile's. */
export function mountPageWater(
  canvas: HTMLCanvasElement,
  origin: HTMLElement,
  options: { paper: boolean },
): PageWaterHandle {
  const ctx = canvas.getContext("2d")!;
  const waves: Wave[] = [];
  let raf = 0;
  let image: ImageData | null = null;
  // the threshold of every pixel of the tile, once
  const threshold = new Float32Array(TILE * TILE);
  for (let y = 0; y < TILE; y++) for (let x = 0; x < TILE; x++) threshold[y * TILE + x] = ign(x, y);
  // a speck's colour by how lit it is, 0..255 (little-endian RGBA)
  const colour = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    const [r, g, b] = speck(FINE.LEVEL_FAR + (1 - FINE.LEVEL_FAR) * (i / 255), options.paper) as [
      number,
      number,
      number,
    ];
    colour[i] = (255 << 24) | (b << 16) | (g << 8) | r;
  }

  const frame = (now: number) => {
    raf = 0;
    const w = canvas.clientWidth,
      h = canvas.clientHeight;
    if (canvas.width !== w || canvas.height !== h || !image) {
      canvas.width = w;
      canvas.height = h;
      image = ctx.createImageData(w, h);
    }
    const px = new Uint32Array(image.data.buffer);
    px.fill(0);
    const o = origin.getBoundingClientRect();
    for (let k = waves.length - 1; k >= 0; k--) {
      const wave = waves[k]!;
      const t = (now - wave.t0) / 1000;
      const r = wave.speed * t;
      // it fades as the firmware's water does
      const height = WAVE_KEEP ** t;
      if (height < FAINT) {
        waves.splice(k, 1);
        continue;
      }
      const cx = wave.x + o.left,
        cy = wave.y + o.top;
      const reach = 2.5 * wave.sigma,
        outer = r + reach,
        inner = Math.max(0, r - reach);
      for (let y = Math.max(0, Math.floor(cy - outer)); y <= Math.min(h - 1, Math.ceil(cy + outer)); y++) {
        const dy = y - cy;
        const far = Math.sqrt(Math.max(0, outer * outer - dy * dy));
        const near = Math.abs(dy) < inner ? Math.sqrt(inner * inner - dy * dy) : 0;
        const ty = ((((y - Math.round(o.top)) % TILE) + TILE) % TILE) * TILE;
        // the ring crosses this row in two runs (one where it is still a disc)
        for (const [from, to] of near > 0
          ? [
              [cx - far, cx - near],
              [cx + near, cx + far],
            ]
          : [[cx - far, cx + far]]) {
          for (let x = Math.max(0, Math.floor(from!)); x <= Math.min(w - 1, Math.ceil(to!)); x++) {
            const dx = x - cx;
            const e = (Math.sqrt(dx * dx + dy * dy) - r) / wave.sigma;
            const lit = height * Math.exp(-e * e);
            if (lit < 0.02) continue;
            const density = CALM + (FINE.AMBIENT - CALM) * Math.min(1, 2 * lit) + FINE.PEAK * GATHER * lit;
            const tx = (((x - Math.round(o.left)) % TILE) + TILE) % TILE;
            if (threshold[ty + tx]! < density) px[y * w + x] = colour[Math.min(255, Math.round(lit * 255))]!;
          }
        }
      }
    }
    ctx.putImageData(image, 0, 0);
    if (waves.length && !document.hidden) raf = requestAnimationFrame(frame);
  };

  const tap = (e: PointerEvent) => {
    if (e.button !== 0 && e.pointerType === "mouse") return;
    // on a screen or a word: its own water's speed, so the two rings are one
    // (a word's canvas takes no pointer: it is found by where the tap landed)
    const inside = (el: Element) => {
      const r = el.getBoundingClientRect();
      return e.clientX >= r.left && e.clientX <= r.right && e.clientY >= r.top && e.clientY <= r.bottom;
    };
    const screen =
      (e.target as Element | null)?.closest?.("[data-molty-screen]") ??
      [...document.querySelectorAll("[data-molty-word]")].find(inside);
    const width = screen ? screen.getBoundingClientRect().width : NOMINAL;
    const o = origin.getBoundingClientRect();
    waves.push({
      x: e.clientX - o.left,
      y: e.clientY - o.top,
      speed: WAVE_SPEED * width,
      sigma: CREST * width,
      t0: performance.now(),
    });
    if (waves.length > 8) waves.shift();
    if (!raf) raf = requestAnimationFrame(frame);
  };
  const resume = () => {
    if (waves.length && !raf && !document.hidden) raf = requestAnimationFrame(frame);
  };
  window.addEventListener("pointerdown", tap, true);
  document.addEventListener("visibilitychange", resume);

  return {
    destroy() {
      window.removeEventListener("pointerdown", tap, true);
      document.removeEventListener("visibilitychange", resume);
      cancelAnimationFrame(raf);
    },
  };
}
