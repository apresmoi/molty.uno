// The landing's opening, as one: the title's word forms out of the dust (MoltyWord), and as it
// starts the hero's device turns in (Device) with its screen on its face, and the dust round it
// comes up in step (PageWater, keyed on <html data-hero>). Then its cat walks in from the
// window's edge (MoltyScreen), and touches the glass only once the device stands.
// Reduced motion: all of it shows at once.

let formed: () => void = () => {};
const started = new Promise<void>((done) => (formed = done));
/** The title's word started forming on screen. */
export const titleForming = () => formed();
/** Resolves `after` seconds once the title started forming (or 2.5 s from now if it never does). */
export const afterTitle = (after: number) =>
  Promise.race([
    started.then(() => new Promise((r) => setTimeout(r, after * 1000))),
    new Promise((r) => setTimeout(r, 2500)),
  ]);

/** The hero's cat sets off this long after the title starts forming, at the soonest. */
export const CAT_AFTER_S = 1;
let standsAt: (at: number) => void = () => {};
const stood = new Promise<number>((done) => (standsAt = done));
/** The hero's device turns in: it stands `ms` from now. */
export const heroStandsIn = (ms: number) => standsAt(performance.now() + ms);
/** Resolves when the hero's cat sets off, `glassS` page seconds at the least from touching the
 *  glass: CAT_AFTER_S after the title starts forming, and late enough that it touches the glass
 *  no sooner than its device stands (a device that never turns in: not waited for). */
export function catSetsOff(glassS: number): Promise<unknown> {
  const stands = Promise.race([stood, afterTitle(CAT_AFTER_S + 2).then(() => 0)]);
  return Promise.all([afterTitle(CAT_AFTER_S), stands]).then(
    ([, at]) => new Promise((r) => setTimeout(r, at - glassS * 1000 - performance.now())),
  );
}

let sat = false;
let onSat: () => void = () => {};
let over: Promise<unknown> | null = null;
/** The hero's cat sits: the opening is over. */
export const heroSits = () => {
  sat = true;
  onSat();
};
/** Resolves once the opening is over (the hero's cat sits, or 8 s from the first call if it never
 *  does; at once on a page without the hero): the devices below the fold prepare only then. */
export function afterOpening(): Promise<unknown> {
  over ??=
    sat || !document.querySelector('[data-device="arrive"]')
      ? Promise.resolve()
      : Promise.race([new Promise<void>((done) => (onSat = done)), new Promise((r) => setTimeout(r, 8000))]);
  return over;
}

/** Where the hero's device is: <html data-hero> ("turning", then its screen's "dark" / "live" /
 *  "still"), which the CSS of the dust round it keys on. */
export function heroCue(state: string) {
  document.documentElement.dataset.hero = state;
}
