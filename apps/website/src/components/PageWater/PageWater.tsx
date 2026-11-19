import { useEffect, useRef, useState } from "react";
import { mountPageWater } from "../../services/page-water";
import styles from "./PageWater.module.css";

/** The page's water: the calm dust is the page's own background (a tile, there without any
 *  script); this is the layer a tap's wave travels on, fixed to the viewport, drawn only while a
 *  wave is out. It finds the page (the element the dust is the background of) by `data-water`.
 *  On mount it also ends the intro's wait (<html data-fluid>: Head's "pending" becomes "live"),
 *  so Head's watchdog knows a script took over. Reduced motion: no waves. The specks' colours
 *  follow the theme, so a theme switch mounts it afresh. */
export function PageWater() {
  const canvas = useRef<HTMLCanvasElement>(null);
  const theme = useTheme();
  useEffect(() => {
    if (!theme) return;
    const root = document.documentElement;
    if (root.dataset.fluid === "pending") root.dataset.fluid = "live";
    const page = canvas.current!.closest<HTMLElement>("[data-water]");
    if (!page || matchMedia("(prefers-reduced-motion: reduce)").matches) return;
    const water = mountPageWater(canvas.current!, page, { paper: theme === "light" });
    return () => water.destroy();
  }, [theme]);
  return <canvas ref={canvas} className={styles.water} data-page-water="" aria-hidden="true" />;
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
