// The landing's opening, as one: the title's word forms out of the dust (MoltyWord), and as it
// starts the hero's device turns in (Device), its cat already walking in from the window's edge
// (MoltyScreen), and the dust round it comes up in step (PageWater, keyed on <html data-hero>).
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

/** Where the hero's device is: <html data-hero> ("turning", then its screen's "dark" / "live" /
 *  "still"), which the CSS of the dust round it keys on. */
export function heroCue(state: string) {
  document.documentElement.dataset.hero = state;
}
