// The landing page's one fluid: @glyphcss/fluid (molty's sim, bit-identical, drawn as braille
// DOM text in one <pre>, the sim in a Web Worker) glued to the document behind the page. This
// file is only the GLUE between the page and the package; it reads its targets from the DOM:
//   [data-fluid-word]   a heading; its own text condenses out of the fluid while on screen and
//                       melts back when scrolled past (`data-fluid-color`, `data-fluid-delay`)
//   [data-fluid-quiet]  the fluid recedes under prose
//   [data-fluid-pool]   the fluid pools here (an invitation to touch it)
// The pointer stirs it and taps ripple it (the package's window pointer). 30 fps, ≤176
// columns; with prefers-reduced-motion the package draws one settled still per change.
// The Moltys are not in this fluid: each lives on its own screen (services/molty-screen).
import { createGlyphFluid, GlyphFluidTargets, type GlyphFluid } from "@glyphcss/fluid";
import FluidWorker from "@glyphcss/fluid/worker?worker";
import { inverted, paperSource } from "./paper";

const FPS = 30;
const FLUID = "#6f8f82"; // the dim braille field
// …on paper (the light theme, paper.ts): a quiet grey-green dot, handed over pre-inverted
const PAPER_FIELD = "#757a78";
const timeout = <T>(p: Promise<T>, ms: number) =>
  Promise.race([p, new Promise<never>((_, no) => setTimeout(() => no(new Error("fluid engine timeout")), ms))]);

export interface PageFluidHandle {
  destroy(): void;
}

export interface PageFluidOptions {
  reducedMotion: boolean;
  /** "light": the page inverts the fluid into ink on paper (paper.ts); its own colours are
   *  handed over pre-inverted. The theme is fixed per mount. */
  theme: "dark" | "light";
}

/** Resolves once the field is drawing. */
export async function mountPageFluid(host: HTMLElement, options: PageFluidOptions): Promise<PageFluidHandle> {
  const fluid = createGlyphFluid(host, {
    glue: "document",
    pointer: "window",
    fps: FPS,
    fpsFallback: false,
    worker: () => new FluidWorker(),
    field: { color: options.theme === "light" ? inverted(PAPER_FIELD) : FLUID, level: 0.18 },
  });
  try {
    await timeout(fluid.ready, 10000);
  } catch (e) {
    fluid.destroy();
    throw e;
  }
  for (const el of document.querySelectorAll<HTMLElement>("[data-fluid-word]")) addWord(fluid, el, options);
  for (const el of document.querySelectorAll<HTMLElement>("[data-fluid-quiet],[data-fluid-pool]"))
    fluid.addZone({ el, kind: el.hasAttribute("data-fluid-pool") ? "pool" : "quiet" });
  return { destroy: () => fluid.destroy() };
}

function addWord(fluid: GlyphFluid, el: HTMLElement, options: PageFluidOptions) {
  const given = el.dataset.fluidColor;
  const hex = given && /^#[\da-f]{6}$/i.test(given) ? given : FLUID;
  // on paper: pre-inverted and darkened to 3:1
  const color = options.theme === "light" ? paperSource(hex) : hex;
  fluid.addTarget(
    // reduced motion: every heading stands settled (no delay, no scroll band)
    GlyphFluidTargets.text(
      el,
      options.reducedMotion
        ? { color, visibility: "always" }
        : { color, delay: Number(el.dataset.fluidDelay ?? 0), visibility: "viewport" },
    ),
  );
}
