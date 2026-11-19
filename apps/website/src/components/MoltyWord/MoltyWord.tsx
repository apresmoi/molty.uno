import { useEffect, useRef, useState } from "react";
import { mountMoltyScreen, type MoltyScreenHandle } from "../../services/molty-screen";
import { WORDS_URL, WORD_PX } from "../../features/molty/content";
import styles from "./MoltyWord.module.css";

/** A heading's word as the device draws a scene: thin sharp lines the dust gathers along, on a
 *  round window of the firmware's renderer playing the word's layout (config/words.mjs). It sits
 *  centred on its heading (the parent positions it; `--word-canvas` is its size) and takes no
 *  pointer: a tap on the page that lands on it is its finger. It runs while it is near the
 *  viewport and is put away when far. `data-word`: "live" once it draws (the heading's own text
 *  then steps aside), "off" if the renderer cannot load (the text stays). */
export function MoltyWord({ word }: { word: string }) {
  const canvas = useRef<HTMLCanvasElement>(null);
  const [state, setState] = useState<"live" | "off">();
  useEffect(() => {
    const reducedMotion = matchMedia("(prefers-reduced-motion: reduce)").matches;
    let handle: MoltyScreenHandle | null = null;
    let near = false;
    let busy = false;
    let alive = true;
    const up = () => {
      if (handle || busy) return;
      busy = true;
      mountMoltyScreen(canvas.current!, {
        scenes: WORDS_URL,
        px: WORD_PX,
        boot: false,
        scene: { layout: word },
        pageTaps: true,
        reducedMotion,
      }).then(
        (h) => {
          busy = false;
          if (!alive || !near) return h.destroy();
          handle = h;
          setState("live");
        },
        () => {
          busy = false;
          if (alive) setState("off");
        },
      );
    };
    const watch = new IntersectionObserver(
      (entries) => {
        near = entries[entries.length - 1]!.isIntersecting;
        if (near) return up();
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
    <span className={styles.root} data-molty-word="" data-word={state} aria-hidden="true">
      <canvas ref={canvas} className={styles.canvas} width={WORD_PX} height={WORD_PX} />
    </span>
  );
}
