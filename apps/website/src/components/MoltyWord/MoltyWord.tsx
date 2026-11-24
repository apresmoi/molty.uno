import { useEffect, useRef, useState } from "react";
import { mountMoltyScreen, type MoltyScreenHandle } from "../../services/molty-screen";
import { grain } from "../../services/page-water";
import { WORDS_URL, WORD_PX, WORD_PX_MAX } from "../../features/molty/content";
import { heroReady, titleForming } from "../../features/home/opening";
import styles from "./MoltyWord.module.css";

/** A heading's word as the device draws a scene: thin sharp lines the dust gathers along, on a
 *  round window of the firmware's renderer playing the word's layout (config/words.mjs). It sits
 *  centred on its heading (the parent positions it; `--word-canvas` is its size) and takes no
 *  pointer. Its dust is the page's grain (the window has as many pixels as the page's dust has
 *  across it) and its water is the page's: a wave loosens its lines where the crest passes and
 *  they sharpen back, a tap on it most. It runs while it is near the viewport and is put away
 *  when far. `data-word`: "live" once it draws (the heading's own text
 *  then steps aside), "off" if the renderer cannot load (the text stays). `forms`: the page's
 *  title, it forms in view the first time (features/home/opening.ts). */
export function MoltyWord({ word, forms = false }: { word: string; forms?: boolean }) {
  const canvas = useRef<HTMLCanvasElement>(null);
  const [state, setState] = useState<"live" | "off">();
  const [forming, setForming] = useState(false);
  useEffect(() => {
    const reducedMotion = matchMedia("(prefers-reduced-motion: reduce)").matches;
    let first = forms && !reducedMotion; // it forms in view
    let handle: MoltyScreenHandle | null = null;
    let near = false;
    let busy = false;
    let alive = true;
    const up = () => {
      if (handle || busy) return;
      busy = true;
      // a pixel of the window is a pixel of the page's dust
      const el = canvas.current!;
      const px =
        Math.min(WORD_PX_MAX, Math.round((el.clientWidth * grain(el.closest("[data-water]"))) / 2) * 2) || WORD_PX;
      el.width = el.height = px;
      mountMoltyScreen(el, {
        scenes: WORDS_URL,
        px,
        boot: false,
        scene: { layout: word },
        heading: true,
        forming: first,
        // the opening starts once the hero's screen is ready too (features/home/opening.ts)
        shown: first ? heroReady() : undefined,
        reducedMotion,
      }).then(
        (h) => {
          busy = false;
          if (!alive || !near) return h.destroy();
          handle = h;
          if (first) {
            first = false; // once: back in view later, it is as it always is
            setForming(true);
            titleForming();
          }
          setState("live");
        },
        () => {
          busy = false;
          if (first) titleForming(); // no word to wait for: the opening goes on without it
          first = false;
          if (alive) setState("off");
        },
      );
    };
    const watch = new IntersectionObserver(
      (entries) => {
        near = entries[entries.length - 1]!.isIntersecting;
        if (near) return up();
        if (first) titleForming(); // far from the view at first: the opening does not wait for it
        first = false;
        handle?.destroy();
        handle = null;
        setState((s) => (s === "live" ? undefined : s));
      },
      { rootMargin: "40% 0px" },
    );
    watch.observe(canvas.current!);
    return () => {
      alive = false;
      watch.disconnect();
      handle?.destroy();
    };
  }, [word]);
  return (
    <span
      className={styles.root}
      data-molty-word=""
      data-word={state}
      data-forming={forming || undefined}
      aria-hidden="true"
    >
      <canvas ref={canvas} className={styles.canvas} width={WORD_PX} height={WORD_PX} />
    </span>
  );
}
