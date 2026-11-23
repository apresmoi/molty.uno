// The page's water: one with the screens'. Its calm dust is the page's own background (tiles,
// there without any script: scripts/dust-tile.mjs): loose everywhere, and denser in focus round
// each device and each heading (a second tile, laid here by where those stand). A tap anywhere
// is a bump in the water's field: the firmware's own water and its own drawer of dots, on a
// grid as wide as the window (services/molty-screen field.ts). The field draws on one canvas
// fixed to the viewport, only while the water moves, and only the cells the water moves; every
// screen takes the field's water at its rim, a heading's window all over.
//
// PAGE-SIDE, not the firmware's: where the dust is (config/water.mjs FOCUS, and web_field.c),
// and what is in this file: the canvas, where it stands as the page scrolls, and the taps.
// Reduced motion: the dust is laid, nothing moves.
import {
  CELL,
  MoltyField,
  TILE,
  WATER,
  instantiate,
  loadModule,
  onFrame,
  setPageWater,
  type FieldExports,
  type Focus,
} from "../molty-screen";
import { BUTTON, CARRY, FOCUS, LIFE, SPEED } from "../../config/water.mjs";

/** The canvas reaches this many cells beyond the viewport: the page scrolls that far before the
 *  field has to be stood again (every cell drawn afresh). */
const MARGIN = { cols: 2, rows: 8 };

/** The field's pixels a CSS px on this page: the page shows a tile (TILE px a side) at its
 *  dust's size, which is a screen's pixel as the page's reference screen shows it
 *  (styles/tokens.css --m-dust-size). */
export function grain(page: Element | null): number {
  const size = page ? parseFloat(getComputedStyle(page).backgroundSize) : NaN;
  return size > 0 ? TILE / size : WATER.panel / 420;
}

/** What a tap on a control lands on: a button, a link, a field, a disclosure, anything with a
 *  control's role or that takes the keyboard's focus (a tap on its text or its icon too). */
const CONTROLS =
  'a[href], button, input, select, textarea, label, summary, [role="button"], [role="link"], [role="tab"], [role="switch"], [role="checkbox"], [role="radio"], [role="option"], [role="menuitem"], [contenteditable]:not([contenteditable="false"]), [tabindex]:not([tabindex="-1"])';

/** Whether a tap on `target` is a tap on a control (its wave BUTTON of a full one). A screen's
 *  glass is its water, not a control. */
function control(target: Element | null): boolean {
  return !!target?.closest?.(CONTROLS) && !target.closest("[data-molty-screen]");
}

export interface PageWaterHandle {
  destroy(): void;
}

/** Mount the page's water. `page` is the element the dust is the background of; `near` covers
 *  it and holds the denser dust (this lays it round what focuses it: `[data-dust-focus]`);
 *  `canvas` is fixed to the viewport's corner (this sizes and moves it). `still`: the dust is
 *  laid and that is all (reduced motion). */
export function mountPageWater(
  canvas: HTMLCanvasElement,
  near: HTMLElement,
  page: HTMLElement,
  options: { paper: boolean; still: boolean },
): PageWaterHandle {
  const ctx = canvas.getContext("2d")!;
  let k = grain(page);
  let field: MoltyField | null = null;
  let making: Promise<void> | null = null;
  let alive = true;
  let stop: (() => void) | null = null;
  /** A front is out: the water is the page's (CARRY); else it is the device's own. */
  let front = false;
  /** The page's cell under the canvas's first one. */
  let col = 0,
    row = 0;
  let laid = "";
  const taps: Array<{ x: number; y: number; radius: number; strength: number }> = [];

  /** What focuses the dust, in CSS px from the page's corner. */
  const foci = (): Focus[] => {
    const o = page.getBoundingClientRect();
    return [...page.querySelectorAll<HTMLElement>("[data-dust-focus]")].map((el) => {
      const r = el.getBoundingClientRect();
      const f = FOCUS[el.dataset.dustFocus as keyof typeof FOCUS];
      return {
        x: r.left - o.left + r.width / 2,
        y: r.top - o.top + r.height / 2,
        rx: f.rx * r.width,
        ry: f.ry * r.height,
        stops: f.stops,
      };
    });
  };
  /** The denser dust shows by the same foci the field lays its own by: a mask on the second tile. */
  const lay = () => {
    const all = foci().filter((f) => f.rx > 0 && f.ry > 0);
    const key = JSON.stringify(all);
    if (key === laid) return;
    laid = key;
    const mask = all
      .map(
        (f) =>
          `radial-gradient(ellipse ${f.rx}px ${f.ry}px at ${f.x}px ${f.y}px, ${f.stops.map(([t, a]) => `rgb(0 0 0 / ${a}) ${t * 100}%`).join(", ")}, transparent 100%)`,
      )
      .join(", ");
    near.style.setProperty("-webkit-mask-image", mask || "none");
    near.style.maskImage = mask || "none";
    near.dataset.laid = "";
    field?.focus(all.map((f) => ({ ...f, x: f.x * k, y: f.y * k, rx: f.rx * k, ry: f.ry * k })));
  };
  lay();
  /** The page changed size: the dust is laid again (and, with water, a field that no longer
   *  fits the window is dropped). */
  let resized = lay;
  const watch = new ResizeObserver(() => resized());
  watch.observe(page);
  if (options.still) return { destroy: () => watch.disconnect() };

  /** The field as wide as the window (and a margin), at the page's grain: made when first
   *  needed, and again when the window no longer is what it was made for. */
  const stale = () =>
    !!field &&
    (grain(page) !== k ||
      field.width !== (Math.ceil((innerWidth * k) / CELL.w) + 2 * MARGIN.cols) * CELL.w ||
      field.height !== (Math.ceil((innerHeight * k) / CELL.h) + 2 * MARGIN.rows) * CELL.h);
  const make = () =>
    (making ??= loadModule()
      .then((module) => instantiate<FieldExports>(module))
      .then((x) => {
        if (!alive) return;
        k = grain(page);
        const [r, g, b] = (getComputedStyle(page).backgroundColor.match(/[\d.]+/g) ?? []).map(Number);
        field = MoltyField.open(x, {
          cols: Math.ceil((innerWidth * k) / CELL.w) + 2 * MARGIN.cols,
          rows: Math.ceil((innerHeight * k) / CELL.h) + 2 * MARGIN.rows,
          background: ((r ?? 0) << 16) | ((g ?? 0) << 8) | (b ?? 0),
          paper: options.paper,
          // the page's speed in the field's own cells
          c2: (speed() / (CELL.w / k)) ** 2,
          ...CARRY,
          life: LIFE,
        });
        if (!field) return;
        canvas.width = field.width;
        canvas.height = field.height;
        canvas.style.width = `${field.width / k}px`;
        canvas.style.height = `${field.height / k}px`;
        laid = "";
        lay();
      })
      .catch(() => {})); // without the module the page keeps its calm dust
  const drop = () => {
    sleep();
    field?.destroy();
    field = null;
    making = null;
    k = grain(page);
  };
  resized = () => {
    if (stale()) drop();
    lay();
  };
  /** A wave's speed: the device's own on the page's reference screen, SPEED times, CSS px a step. */
  const speed = () => (SPEED * Math.sqrt(WATER.c2) * (WATER.panel / k)) / WATER.cells;

  /** The canvas stands over the viewport, on the page's own lattice: stood again (the water
   *  carried along) when the page has scrolled past its margin. */
  const stand = () => {
    if (!field) return;
    const o = page.getBoundingClientRect();
    const at = () => [o.left + (col * CELL.w) / k, o.top + (row * CELL.h) / k] as const;
    let [x, y] = at();
    if (x > 0 || y > 0 || x + field.width / k < innerWidth || y + field.height / k < innerHeight) {
      col = Math.floor((-o.left * k) / CELL.w) - MARGIN.cols;
      row = Math.floor((-o.top * k) / CELL.h) - MARGIN.rows;
      field.place(col, row);
      [x, y] = at();
    }
    canvas.style.translate = `${x}px ${y}px`;
  };

  const frame = (ms: number) => {
    if (!field) return;
    stand();
    const state = field.step(ms / 1000);
    front = state === "front";
    const boxes = field.draw();
    if (boxes.length) {
      const image = new ImageData(field.pixels() as Uint8ClampedArray<ArrayBuffer>, field.width, field.height);
      for (let i = 0; i < boxes.length; i += 4)
        ctx.putImageData(image, 0, 0, boxes[i]!, boxes[i + 1]!, boxes[i + 2]!, boxes[i + 3]!);
    }
    if (state === "flat") sleep();
  };
  /** The water is flat: the canvas is clear, the page's own tiles show. */
  const sleep = () => {
    stop?.();
    stop = null;
    front = false;
    ctx.clearRect(0, 0, canvas.width, canvas.height);
  };

  const tap = (e: PointerEvent) => {
    if (e.button !== 0 && e.pointerType === "mouse") return;
    // on a screen: the bump its own finger makes there (mole_fluid.c: by the glass's radius);
    // on the page: the same, of the page's reference screen; on a control: as wide, fainter
    const target = e.target as Element | null;
    const screen = target?.closest?.("[data-molty-screen]");
    const o = page.getBoundingClientRect();
    taps.push({
      x: e.clientX - o.left,
      y: e.clientY - o.top,
      radius: (WATER.tapRadius * (screen ? screen.getBoundingClientRect().width : WATER.panel / k)) / 2,
      strength: WATER.tap * (control(target) ? BUTTON : 1),
    });
    if (stale()) drop();
    void make().then(() => {
      if (!field || !alive) return;
      lay();
      stand();
      for (const t of taps.splice(0)) field.tap(t.x * k, t.y * k, t.strength, t.radius * k);
      front = true;
      stop ??= onFrame(frame, true);
    });
  };
  const warm = () => void make();
  window.addEventListener("pointerdown", tap, true);
  window.addEventListener("pointermove", warm, { once: true, passive: true });

  setPageWater({
    get speed() {
      return speed();
    },
    get leak() {
      return front ? CARRY.leak : WATER.leak;
    },
    get damp() {
      return front ? CARRY.damp : WATER.damp;
    },
    get moving() {
      return !!stop;
    },
    under(xy, box, px) {
      if (!field || !stop) return null;
      const o = page.getBoundingClientRect();
      return field.sample(xy, (box.left - o.left) * k, (box.top - o.top) * k, (box.width / px) * k);
    },
  });

  return {
    destroy() {
      alive = false;
      window.removeEventListener("pointerdown", tap, true);
      window.removeEventListener("pointermove", warm);
      watch.disconnect();
      setPageWater(null);
      drop();
    },
  };
}
