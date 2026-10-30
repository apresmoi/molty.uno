import { useEffect, useRef, useState } from "react";
import { mountPageFluid, type PageFluidHandle } from "../../services/molty-stage";
import styles from "./FluidField.module.css";

/** The one live fluid behind the landing page. It reads its targets (headings, Molty slots,
 *  quiet and pool zones) from data-fluid-* attributes in the page, so the page stays static
 *  HTML that reads fine without it. Sets <html data-fluid="live"> once it draws (@glyphcss/fluid), and
 *  data-fluid-moltys="live" once the Moltys can emerge (their three.js rig loaded); "off" when
 *  it cannot (the page shows its plain text and stills). From Head's "pending" (the intro's
 *  start state, set before paint) it moves data-fluid to "loading" on mount, so Head's
 *  watchdog knows a script took over. The fluid's own colours depend on the theme (light hands
 *  them over pre-inverted), so a theme switch mounts it afresh. */
export function FluidField() {
  const host = useRef<HTMLDivElement>(null);
  const theme = useTheme();
  useEffect(() => {
    if (!theme) return;
    const root = document.documentElement;
    let handle: PageFluidHandle | null = null;
    let alive = true;
    if (root.dataset.fluid === "pending") root.dataset.fluid = "loading";
    mountPageFluid(host.current!, {
      reducedMotion: matchMedia("(prefers-reduced-motion: reduce)").matches,
      theme,
      onState: (state) => {
        if (!alive) return;
        if (state === "live") root.dataset.fluid = "live";
        else root.dataset.fluidMoltys = state === "moltys" ? "live" : "off";
      },
    }).then(
      (h) => (alive ? (handle = h) : h.destroy()),
      () => {
        // engine unavailable: the page shows its plain text and still portraits
        if (!alive) return;
        root.dataset.fluid = "off";
        root.dataset.fluidMoltys = "off";
      },
    );
    return () => {
      alive = false;
      handle?.destroy();
      delete root.dataset.fluid;
      delete root.dataset.fluidMoltys;
    };
  }, [theme]);
  return <div ref={host} className={styles.field} data-fluid-field="" aria-hidden="true" />;
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
