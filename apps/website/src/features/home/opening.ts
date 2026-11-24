// The landing's opening, as one: the title's word forms out of the dust (MoltyWord), and as it
// starts the hero's device turns in (Device) with its screen on its face, and the dust round it
// comes up in step (PageWater, keyed on <html data-hero>). Then its cat walks in from the
// window's edge (MoltyScreen), and touches the glass only once the device stands.
// Reduced motion: all of it shows at once.
//
// It starts only once all of it is ready: the title waits for the hero's screen to be prepared
// (its module and package in, its first frames played unseen, its cat up and waiting), so a first
// visit plays as a repeat visit does instead of preparing the hero while the title forms. Short
// of that, the page waits with its text and dust. Last resort (a module or package that never
// comes): the opening's clock starts OPENING_CAP_MS after the page was opened whatever is
// ready; the device turns in and the cat sets off on that clock, a screen not ready by then
// shows once it is (dark first, as the device stands) and the title forms when it can.

/** The opening starts at the latest this long after the page was opened (ms). */
export const OPENING_CAP_MS = 8000;
const wait = (ms: number) => new Promise((r) => setTimeout(r, Math.max(0, ms)));
const cap = wait(OPENING_CAP_MS - performance.now());

let prepared: () => void = () => {};
const prepare = new Promise<void>((done) => (prepared = done));
/** The hero's screen is prepared, or will not be (it failed, or is not near the view). */
export const heroPrepared = () => prepared();
/** Resolves once the hero's screen is prepared (at once on a page without the hero; at the cap
 *  at the latest): the title starts forming only then. */
export const heroReady = (): Promise<unknown> =>
  document.querySelector('[data-device="arrive"]') ? Promise.race([prepare, cap]) : Promise.resolve();

let formed: () => void = () => {};
const started = Promise.race([new Promise<void>((done) => (formed = done)), cap]);
/** The title's word started forming on screen (or will not: it failed, or is not near the view). */
export const titleForming = () => formed();
/** Resolves `after` seconds once the title started forming (or the cap passed, if it never does). */
export const afterTitle = (after: number) => started.then(() => wait(after * 1000));

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

let up: () => void = () => {};
const sceneUp = new Promise<void>((done) => (up = done));
let over: Promise<unknown> | null = null;
/** Resolves once the hero stands with its scene up (or 8 s from the first call if it never does;
 *  at once on a page without the hero): the devices below the fold prepare only then, while its
 *  cat still walks in. */
export function afterHeroUp(): Promise<unknown> {
  const stands = () => stood.then((at) => new Promise((r) => setTimeout(r, at - performance.now())));
  over ??= !document.querySelector('[data-device="arrive"]')
    ? Promise.resolve()
    : Promise.race([Promise.all([stands(), sceneUp]), new Promise((r) => setTimeout(r, 8000))]);
  return over;
}

/** Where the hero's device is: <html data-hero> ("turning", then its screen's "dark" / "live" /
 *  "still"), which the CSS of the dust round it keys on. */
export function heroCue(state: string) {
  document.documentElement.dataset.hero = state;
  if (state === "live" || state === "still") up();
}
