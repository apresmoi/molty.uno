// One water: the page's own (services/page-water mounts it and says so here) as a screen meets
// it. A screen takes the page's water at its rim before each of its frames, and gives its own
// water the page's numbers, so a wave crosses the rim as one front.

/** The page's water, as its screens need it. */
export interface PageWater {
  /** How far a wave travels in one step of the water, CSS px. */
  speed: number;
  /** What a step keeps of a wave's height and of its motion now (fluid_params_t leak, damp):
   *  while a front is out the page's water keeps its waves, else it is the device's own. */
  leak: number;
  damp: number;
  /** It moves now (flat: nothing of it is drawn, and `under` gives null). */
  moving: boolean;
  /** The water under a screen's cells (`xy`: x, y each, in the screen's own pixels; `box`: where
   *  the screen's canvas is in the viewport; `px`: its pixels a side): its height now and a step
   *  ago for each, or null while the page's water is flat. */
  under(xy: Float32Array, box: DOMRect, px: number): Float32Array | null;
}

let water: PageWater | null = null;
/** The page's water, null without one (a page with no water, reduced motion). */
export const pageWater = () => water;
/** The page's water is this from now on (null: gone). */
export function setPageWater(w: PageWater | null) {
  water = w;
}
