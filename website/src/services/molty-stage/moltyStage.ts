// Owns the live Molty screen runtime: loads the REAL engine (creatures/js/setup/fluid-stage.js,
// which imports three.js from the CDN) once, and keeps one FluidStage per named slot.
//
// A FluidStage runs its own requestAnimationFrame loop for the page's lifetime and idles
// while its canvas is detached (it checks `isConnected`). So "pausing" = detaching the
// canvas, and React StrictMode replays or remounts reuse the same stage per slot.
import type { FluidStage } from "@creatures/setup/fluid-stage.js";

type Engine = typeof import("@creatures/setup/fluid-stage.js");
let engine: Promise<Engine> | null = null;
const stages = new Map<string, FluidStage>();

export function loadEngine(timeoutMs = 10000): Promise<Engine> {
  engine ??= Promise.race([
    import("@creatures/setup/fluid-stage.js"),
    new Promise<never>((_, reject) => setTimeout(() => reject(new Error("molty engine timeout")), timeoutMs)),
  ]).catch((error) => {
    engine = null; // let a later mount try again
    throw error;
  });
  return engine;
}

export async function acquireStage(slot: string): Promise<FluidStage> {
  const { FluidStage } = await loadEngine();
  let stage = stages.get(slot);
  if (!stage) {
    stage = new FluidStage(412);
    stages.set(slot, stage);
  }
  return stage;
}

/** Attach the stage's canvas to `host` only while the host is on screen. Returns cleanup. */
export function attachWhileVisible(stage: FluidStage, host: HTMLElement): () => void {
  const observer = new IntersectionObserver(
    ([entry]) => {
      if (entry?.isIntersecting) {
        if (stage.el.parentElement !== host) host.replaceChildren(stage.el);
      } else stage.el.remove();
    },
    { rootMargin: "120px" },
  );
  observer.observe(host);
  return () => {
    observer.disconnect();
    if (stage.el.parentElement === host) stage.el.remove();
  };
}

/** What the fluid should be: the calm blob, a word condensed out of it, or a Molty. */
export type ScreenCue = { kind: "blob" } | { kind: "word"; word: string } | { kind: "molty"; id: string };
export type CueStep = { cue: ScreenCue; seconds: number };
