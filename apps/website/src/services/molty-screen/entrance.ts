// The entrance (the hero): the cat starts beyond the right edge of the window, walks across the
// page and into its screen, and sits where it always sits. No DOM here: the screen (screen.ts)
// and its test (scripts/tests/entrance.test.ts) step the same class.
import type { MoltyCore } from "./core";

/** All in the firmware's world units, measured on its own frames (device/firmware/web/web_main.c,
 *  WM_TRACE). */
export const ENTER = {
  /** World units a panel is wide (creature_dots.h CD_VIEW_SIZE; the picture has no perspective
   *  across): how far beyond the window's edge the cat starts. */
  view: 1.75,
  /** The cat's nose is this far ahead of where it stands: it starts that much beyond the edge. */
  nose: 0.85,
  /** Played unseen first: it gets up. */
  lead: 1.0,
  /** Its clock runs this much faster while it walks in (its own walk's cadence and ground speed
   *  together: its feet stay planted) ... */
  rate: 2.2,
  /** ... and this much once it is at its place (it stops, turns to you and sits). */
  settle: 4.5,
  /** At its place: this near the frame's middle. */
  near: 0.12,
  /** The most it moves a firmware second (creature_clip.c CC_PACE). */
  pace: 0.6,
  /** Its picture touches the glass from no farther across than this (measured: 1.52 to 1.56). */
  glass: 1.5,
};
/** Walking in, the cat also drifts toward the viewer: starting [across, back] it ends at its own
 *  place. Measured for each start; a start in between takes the next one further out. */
// prettier-ignore
export const ENTER_STARTS: Array<[number, number]> = [
  [1.0, 0.885], [1.25, 1.044], [1.5, 0.853], [1.75, 1.006], [2.0, 1.144], [2.25, 0.937], [2.5, 1.066], [2.75, 1.205], [3.0, 0.99],
  [3.25, 1.111], [3.5, 1.233], [3.75, 1.364], [4.0, 1.141], [4.5, 1.375], [5.0, 1.275], [5.5, 1.502], [6.0, 1.395],
];
/** The start that puts the cat wholly beyond a point `aside` world units to the right. Past the
 *  table (a wide, short window) it starts a table's step further out than that (it walks a little
 *  in the unseen lead) with the last drift: it attends at its place all the same (web_cat.c), and
 *  its walk from outside does not count against the call's 8 s (web_cat_coming). */
const enterFrom = (aside: number): [number, number] =>
  ENTER_STARTS.find(([across]) => across >= aside + ENTER.nose) ?? [aside + ENTER.nose + 0.25, ENTER_STARTS.at(-1)![1]];

const SETTLE_STEP_MS = 1000 / 30;

/** `us` of the firmware's clock: a sped-up clock goes in steps no longer than a device frame (the
 *  creature's player takes no more than that of one frame's time). */
export function play(us: number, frame: (us: number) => void) {
  const [n, dt] = steps(us);
  for (let i = 0; i < n; i++) frame(dt);
}
/** `play`'s steps for `us`: how many, and the firmware's clock in each. */
export function steps(us: number): [number, number] {
  const n = Math.ceil(us / 30000);
  return [n, us / n];
}

/** The cat walking in on the screen's Molty and, when there is one, on the window beside it: the
 *  same Molty (same seed, same calls, same frames), its camera looking where the cat is. Every
 *  call that moves the cat goes to both, so the two stay one cat to the hand-over. */
export class Entrance {
  readonly core: MoltyCore;
  readonly twin: MoltyCore | null;
  /** False when there was no creature to call. */
  readonly walking: boolean;
  /** How far the window beside the screen looks to the side (panel px). */
  aside = 0;

  /** `aside`: world units from the screen's middle to the window's right edge. The creature is
   *  on screen after a few frames; then it is put beyond that edge and called. Its getting up is
   *  played unseen (`lead`) before the first step. */
  constructor(core: MoltyCore, twin: MoltyCore | null, aside: number) {
    this.core = core;
    this.twin = twin;
    const [across, back] = enterFrom(aside);
    let ok = true;
    for (const c of this.cores()) {
      for (let i = 0; i < 3; i++) c.frame(SETTLE_STEP_MS * 1000);
      ok = c.enter(across, back) && ok;
    }
    this.walking = ok;
  }

  /** Its getting up, played unseen: a yield a step (the screen spreads them over the page's frames). */
  *lead() {
    if (!this.walking) return;
    for (let t = 0; t < ENTER.lead * 1000; t += SETTLE_STEP_MS) yield this.advance(SETTLE_STEP_MS * 1000);
  }

  /** Page seconds it takes at the least to touch the glass from where it is. */
  glassS(): number {
    return Math.max(0, (this.core.across() - ENTER.glass) / (ENTER.pace * ENTER.rate));
  }

  private cores() {
    return this.twin ? [this.core, this.twin] : [this.core];
  }
  private advance(us: number) {
    play(us, (dt) => {
      if (this.twin) {
        this.aside = this.twin.pan(this.core.across());
        this.twin.frame(dt);
      }
      this.core.frame(dt);
    });
  }

  /** `ms` of the page's time, the clock sped up. False once it sits: the screen's own cat takes
   *  over. `entering` is asked of both: the call moves the cat (web_molty.c wm_entering). */
  step(ms: number): boolean {
    const rate = Math.abs(this.core.across()) < ENTER.near ? ENTER.settle : ENTER.rate;
    this.advance(Math.round(ms * 1000 * rate));
    this.twin?.entering();
    return this.core.entering();
  }
}
