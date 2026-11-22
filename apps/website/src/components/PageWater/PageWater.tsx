import { useEffect, useRef, useState } from "react";
import { mountPageWater } from "../../services/page-water";
import styles from "./PageWater.module.css";

/** The page's water. The loose dust is the page's own background (a tile, there without any
 *  script); here are the layers over it, under the page's text: the denser dust in focus round
 *  each device and heading (a second tile, laid by the service once it knows where they stand),
 *  the canvas a tap's wave moves the dust on (fixed to the viewport, drawn only while the water
 *  moves), and loose dust drifting on the page (by CSS alone). It finds the page (the element
 *  the dust is the background of) by `data-water`. On mount it also ends the intro's wait
 *  (<html data-fluid>: Head's "pending" becomes "live"), so Head's watchdog knows a script took
 *  over. Reduced motion: the dust is laid, no waves. The dust's colours follow the theme, so a
 *  theme switch mounts it afresh. */
export function PageWater() {
  const canvas = useRef<HTMLCanvasElement>(null);
  const near = useRef<HTMLSpanElement>(null);
  const theme = useTheme();
  useEffect(() => {
    if (!theme) return;
    const root = document.documentElement;
    if (root.dataset.fluid === "pending") root.dataset.fluid = "live";
    const page = canvas.current!.closest<HTMLElement>("[data-water]");
    if (!page) return;
    const water = mountPageWater(canvas.current!, near.current!, page, {
      paper: theme === "light",
      still: matchMedia("(prefers-reduced-motion: reduce)").matches,
    });
    return () => water.destroy();
  }, [theme]);
  return (
    <>
      <span ref={near} className={styles.near} aria-hidden="true" />
      <canvas ref={canvas} className={styles.water} data-page-water="" aria-hidden="true" />
      <span className={styles.drift} aria-hidden="true" />
    </>
  );
}

/** <html data-theme> (Starlight's picker and ThemeToggle set it), followed live; null before mount. */
function useTheme(): "dark" | "light" | null {
  const [theme, setTheme] = useState<"dark" | "light" | null>(null);
  useEffect(() => {
    const root = document.documentElement;
    const read = () => setTheme(root.dataset.theme === "light" ? "light" : "dark");
    read();
    const mo = new MutationObserver(read);
    mo.observe(root, { attributes: true, attributeFilter: ["data-theme"] });
    return () => mo.disconnect();
  }, []);
  return theme;
}
