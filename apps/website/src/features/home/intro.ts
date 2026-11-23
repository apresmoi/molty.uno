// The landing's intro, previewed by URL: `?intro=a|b|c` (how the title forms out of the dust)
// and `pace=calm|quick` (the hero's device, the dust round it and its screen as one sequence).
// Without either the page is as it always is. Reduced motion: no preview (all shows at once).

export type TitleIntro = "a" | "b" | "c";
export type IntroPace = "calm" | "quick";
export interface IntroPreview {
  title?: TitleIntro;
  pace?: IntroPace;
}

export const TITLE_INTROS: Record<TitleIntro, string> = {
  a: "condense",
  b: "calm from the middle",
  c: "cell by cell",
};

/** A pace: how long after the title starts forming the device turns in (s), and how much slower
 *  its turn is than its own (1: as it is). The dust round it and its screen keep step in CSS
 *  (PageWater, MoltyScreen: keyed on <html data-intro-pace>). */
export const PACES: Record<IntroPace, { after: number; stretch: number }> = {
  calm: { after: 0.9, stretch: 1.3 },
  quick: { after: 0, stretch: 1 },
};

/** The preview this page was opened with, or null (no parameter, reduced motion, the server). */
export function introPreview(): IntroPreview | null {
  if (typeof location === "undefined" || matchMedia("(prefers-reduced-motion: reduce)").matches) return null;
  const q = new URLSearchParams(location.search);
  const title = q.get("intro") as TitleIntro | null;
  const pace = q.get("pace") as IntroPace | null;
  const preview: IntroPreview = {
    title: title && title in TITLE_INTROS ? title : undefined,
    pace: pace && pace in PACES ? pace : undefined,
  };
  return preview.title || preview.pace ? preview : null;
}

let titleLive: () => void = () => {};
const titleStarted = new Promise<void>((done) => (titleLive = done));
/** The title's word started forming on screen. */
export const titleForming = () => titleLive();
/** Resolves `after` seconds once the title started forming (or 2.5 s from now if it never does). */
export const afterTitle = (after: number) =>
  Promise.race([
    titleStarted.then(() => new Promise((r) => setTimeout(r, after * 1000))),
    new Promise((r) => setTimeout(r, 2500)),
  ]);

/** Where the hero's device is (an intro preview with a pace): <html data-intro-device>
 *  ("turning", then its screen's "dark" / "live" / "still"), which the CSS of the dust round it
 *  and of its screen keys on. */
export function heroCue(state: string) {
  if (introPreview()?.pace) document.documentElement.dataset.introDevice = state;
}
